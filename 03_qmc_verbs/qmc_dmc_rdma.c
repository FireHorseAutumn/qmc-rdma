// qmc_dmc_rdma.c — two-node diffusion Monte Carlo over hand-written ibverbs.
//
// Same physics as dmc_serial.c (1D harmonic oscillator, E0 = 0.5), but the
// walker population is split across two nodes and every RDMA primitive does
// real physics work:
//
//   RDMA READ           -> master pulls the worker's per-round stats (energy
//                          sum, population) with zero worker-CPU involvement
//   RDMA WRITE          -> master pushes the new reference energy E_T into
//                          the worker's control block
//   WRITE + immediate   -> surplus walkers pushed into the peer's registered
//                          inbox (load balancing); the IMM consumes a posted
//                          RECV on the target, which is how SEND/RECEIVE-style
//                          notification enters the program
//
// QP bring-up is manual (RESET -> INIT -> RTR -> RTS). Only the connection
// handshake (QPN/GID/rkey exchange) goes over a plain TCP socket.
//
// Protocol notes:
//   - The CQ is shared between sends and RECVs. rdma_op waits for its OWN
//     completion (matched by wr_id/opcode); a WRITE+IMM arrival consumed
//     there is counted pending and handed to absorb_inbox, which owns the
//     inbox payload and replaces the consumed RECV.
//   - Inbox ownership: n == 0 means free. The sender RDMA-READs the peer's
//     inbox->n until it is free before pushing (single writer per inbox, so
//     observing free owns it). absorb_inbox runs at the TOP of each round so
//     a peer waiting on inbox space can never deadlock against the E_T
//     control exchange.
//
// Usage (fabric up via ../01_bringup/bringup.sh; shared netns mode means both
// rxe devices are visible in both namespaces, so each node picks its own):
//   ns1:  ip netns exec ns1 ./qmc_dmc_rdma --listen              (default dev rxe0)
//   ns2:  ip netns exec ns2 ./qmc_dmc_rdma --connect 10.0.0.1    (default dev rxe1)
//   optional: --dev <name> overrides the device; --seed=N overrides the
//   per-role RNG default (master and worker streams must differ)
//
// Build: gcc -O2 -Wall -o qmc_dmc_rdma qmc_dmc_rdma.c -libverbs -lm
#include <arpa/inet.h>
#include <infiniband/verbs.h>
#include <math.h>
#include <netinet/in.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

/* ---------------- physics parameters (match dmc_serial.c) ---------------- */
#define ALPHA      0.8
#define TAU        0.01
#define TARGET     1024            // combined population across both nodes
#define LOCAL_TGT  (TARGET / 2)
#define MAXW       4096            // per-node walker array capacity
#define GENS       3000
#define EQUIL      500
#define SYNC_GENS  50              // generations between sync rounds
#define MAX_MIG    128             // walker inbox capacity per round
#define FEEDBACK   0.1

#define TCP_PORT   4751
#define N_RECVS    16              // posted RECV WQEs for WRITE-with-IMM

#define WRID_RDMA_OP   1ull        // rdma_op's signaled sends
#define WRID_RECV_BASE 0x100ull    // posted RECVs (WRITE+IMM arrivals)

#define die(...) do { fprintf(stderr, "FATAL: " __VA_ARGS__); \
                      fprintf(stderr, "\n"); exit(1); } while (0)

/* ---------------- registered-memory layout (shared over the wire) -------- */
struct stats_block {               // each node publishes; peer RDMA READs
    double   sum_EL;               // sum of local energies over the round
    uint64_t samples;              // sum of walker counts over the round
    uint32_t n;                    // current local walker count
    uint32_t pad;
    uint64_t gen;                  // written LAST (release); reader spins on it
};

struct control_block {             // master computes; worker waits on it
    double   E_T;                  // new reference energy
    uint64_t gen;                  // reader spins until gen advances
};

struct walker_inbox {              // peer RDMA WRITEs surplus walkers here
    uint32_t n;
    uint32_t pad;
    double   x[MAX_MIG];
};

/* ---------------- DMC core (identical stepping to dmc_serial.c) ---------- */
static uint64_t rng_state = 0x9E3779B97F4A7C15ull;

static uint64_t xorshift(void) {
    uint64_t x = rng_state;
    x ^= x << 13; x ^= x >> 7; x ^= x << 17;
    return rng_state = x, x;
}
static double urand(void) { return (xorshift() >> 11) * (1.0 / 9007199254740992.0); }
static double gauss(void) {
    double u1 = urand(); if (u1 < 1e-300) u1 = 1e-300;
    return sqrt(-2.0 * log(u1)) * cos(2.0 * M_PI * urand());
}
static double local_energy(double x) {
    return ALPHA / 2.0 + (1.0 - ALPHA * ALPHA) * x * x / 2.0;
}

struct population {
    double   x[MAXW];
    int      n;
    double   E_T;
    double   E_acc;                // post-equilibration energy accumulator
    long     E_cnt;
};

/* One generation: drift, diffuse, branch. Returns generation mean E_L and
   adds this generation's energy sum / walker count into *sum / *samples.
   The energy sum covers the pre-branching walkers, so both the returned
   mean and *samples use the pre-branching count. */
static double dmc_generation(struct population *p, double *sum, uint64_t *samples) {
    static double next[MAXW];
    int m = 0;
    double s = 0.0;
    const int n_pre = p->n;

    for (int i = 0; i < p->n; i++) {
        double x = p->x[i];
        x += TAU * (-ALPHA * x);
        x += sqrt(TAU) * gauss();
        double EL = local_energy(x);
        s += EL;
        int copies = (int)floor(exp(-TAU * (EL - p->E_T)) + urand());
        if (copies > 3) copies = 3;
        for (int c = 0; c < copies && m < MAXW; c++) next[m++] = x;
    }
    if (m == 0) die("population died (E_T=%.3f)", p->E_T);
    memcpy(p->x, next, m * sizeof(double));
    p->n = m;
    *sum += s;
    *samples += n_pre;
    return s / n_pre;
}

/* ---------------- RDMA plumbing ------------------------------------------ */
struct rdma_ctx {
    struct ibv_context *ctx;
    struct ibv_pd      *pd;
    struct ibv_cq      *cq;
    struct ibv_qp      *qp;
    int                 gid_index;
    union ibv_gid       gid;
    uint16_t            lid;
    /* registered regions; lkeys differ per MR, so ops take lkey explicitly */
    struct stats_block   *stats;   struct ibv_mr *mr_stats;   // published
    struct control_block *ctrl;    struct ibv_mr *mr_ctrl;    // E_T in/out
    struct walker_inbox  *inbox;   struct ibv_mr *mr_inbox;   // incoming walkers
    struct walker_inbox  *stage;   struct ibv_mr *mr_stage;   // outgoing walkers
    struct stats_block   *scratch; struct ibv_mr *mr_scratch; // READ destination
    uint32_t           *flag;    struct ibv_mr *mr_flag;    // inbox-free READ scratch
    int                 pending_imm;   // recv completions rdma_op pulled off the CQ
    uint32_t            pending_imm_k; // their walker count (inbox is single-slot)
};

struct wire_info {                 // exchanged over the TCP bootstrap
    uint16_t lid;
    uint16_t gid_index;
    uint32_t qpn;
    uint32_t psn;
    uint8_t  gid[16];
    uint64_t addr_stats;   uint32_t rkey_stats;
    uint64_t addr_ctrl;    uint32_t rkey_ctrl;
    uint64_t addr_inbox;   uint32_t rkey_inbox;
};
static struct wire_info remote;

static void *xaligned(size_t sz) {
    void *p = NULL;
    if (posix_memalign(&p, 4096, sz)) die("posix_memalign");
    memset(p, 0, sz);
    return p;
}

static struct ibv_mr *reg(struct rdma_ctx *r, void *buf, size_t sz) {
    struct ibv_mr *mr = ibv_reg_mr(r->pd, buf, sz,
        IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_READ | IBV_ACCESS_REMOTE_WRITE);
    if (!mr) die("ibv_reg_mr");
    return mr;
}

/* Post one signaled RDMA READ or WRITE (+ optional imm) and wait for ITS
   completion, matched by wr_id and opcode. The CQ is shared with the posted
   RECVs, so a peer WRITE+IMM arrival can surface here first: count it and
   let absorb_inbox own the payload and the RECV replacement. */
static void rdma_op(struct rdma_ctx *r, enum ibv_wr_opcode op, uint32_t lkey,
                    void *local, uint32_t len, uint64_t raddr, uint32_t rkey,
                    uint32_t imm) {
    struct ibv_sge sge = { .addr = (uintptr_t)local, .length = len, .lkey = lkey };
    struct ibv_send_wr wr = { 0 }, *bad;
    wr.wr_id      = WRID_RDMA_OP;
    wr.sg_list    = &sge;
    wr.num_sge    = 1;
    wr.opcode     = op;
    wr.send_flags = IBV_SEND_SIGNALED;
    wr.wr.rdma.remote_addr = raddr;
    wr.wr.rdma.rkey        = rkey;
    if (op == IBV_WR_RDMA_WRITE_WITH_IMM) wr.imm_data = imm;
    if (ibv_post_send(r->qp, &wr, &bad)) die("ibv_post_send");

    const enum ibv_wc_opcode want =
        op == IBV_WR_RDMA_READ ? IBV_WC_RDMA_READ : IBV_WC_RDMA_WRITE;
    for (;;) {
        struct ibv_wc wc;
        int ne = ibv_poll_cq(r->cq, 1, &wc);
        if (ne == 0) continue;
        if (ne < 0) die("ibv_poll_cq");
        if (wc.status != IBV_WC_SUCCESS)
            die("CQ error on %d: %s", op, ibv_wc_status_str(wc.status));
        if (wc.wr_id == WRID_RDMA_OP) {
            if (wc.opcode != want)
                die("completion opcode %d does not match posted op %d",
                    wc.opcode, op);
            return;
        }
        if (wc.wr_id >= WRID_RECV_BASE && wc.opcode == IBV_WC_RECV_RDMA_WITH_IMM) {
            if (r->pending_imm) die("second inbox payload while first unconsumed");
            r->pending_imm = 1;
            r->pending_imm_k = wc.imm_data;
            continue;
        }
        die("unexpected completion: wr_id %llu opcode %d",
            (unsigned long long)wc.wr_id, wc.opcode);
    }
}

static void post_recvs(struct rdma_ctx *r, int count) {
    for (int i = 0; i < count; i++) {
        struct ibv_recv_wr wr = { .wr_id = 0x100 + i, .num_sge = 0 }, *bad;
        if (ibv_post_recv(r->qp, &wr, &bad)) die("ibv_post_recv");
    }
}

/* Absorb one arrived inbox payload and release the slot: n = 0 is the
   "free" state the peer's ownership READ waits on. */
static void absorb_one(struct rdma_ctx *r, struct population *p, uint32_t k) {
    if (k > MAX_MIG || r->inbox->n != k) die("inbox desync");
    for (uint32_t j = 0; j < k && p->n < MAXW; j++)
        p->x[p->n++] = r->inbox->x[j];
    r->inbox->n = 0;
    __sync_synchronize();             // release inbox to the peer's NIC
    post_recvs(r, 1);                 // replace the consumed RECV
}

/* Drain WRITE-with-IMM arrivals: first the one rdma_op may already have
   pulled off the shared CQ, then any sitting on the CQ now. */
static void absorb_inbox(struct rdma_ctx *r, struct population *p) {
    if (r->pending_imm) {
        absorb_one(r, p, r->pending_imm_k);
        r->pending_imm = 0;
    }
    struct ibv_wc wc[8];
    int ne = ibv_poll_cq(r->cq, 8, wc);   // non-blocking
    if (ne < 0) die("ibv_poll_cq");
    for (int i = 0; i < ne; i++) {
        if (wc[i].status != IBV_WC_SUCCESS)
            die("recv CQ error: %s", ibv_wc_status_str(wc[i].status));
        if (wc[i].wr_id < WRID_RECV_BASE || wc[i].opcode != IBV_WC_RECV_RDMA_WITH_IMM)
            die("unexpected completion: wr_id %llu opcode %d",
                (unsigned long long)wc[i].wr_id, wc[i].opcode);
        absorb_one(r, p, wc[i].imm_data);
    }
}

/* ---------------- device + QP bring-up ----------------------------------- */
static void pick_gid(struct rdma_ctx *r) {
    struct ibv_port_attr pa;
    if (ibv_query_port(r->ctx, 1, &pa)) die("ibv_query_port");
    r->lid = pa.lid;
    for (int i = 0; i < pa.gid_tbl_len; i++) {
        struct ibv_gid_entry ge;
        if (ibv_query_gid_ex(r->ctx, 1, i, &ge, 0)) continue;
        if (ge.gid_type == IBV_GID_TYPE_ROCE_V2) {   // RXE exposes RoCEv2
            r->gid_index = i;
            r->gid = ge.gid;
            return;
        }
    }
    if (pa.gid_tbl_len > 1) {                        // fallback
        r->gid_index = 1;
        if (ibv_query_gid(r->ctx, 1, 1, &r->gid)) die("ibv_query_gid");
        return;
    }
    die("no usable GID (is rxe0 up in this namespace?)");
}

static void qp_to_rts(struct rdma_ctx *r) {
    struct ibv_qp_attr attr;

    memset(&attr, 0, sizeof attr);
    attr.qp_state        = IBV_QPS_INIT;
    attr.port_num        = 1;
    attr.pkey_index      = 0;
    attr.qp_access_flags = IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_READ |
                           IBV_ACCESS_REMOTE_WRITE;
    if (ibv_modify_qp(r->qp, &attr, IBV_QP_STATE | IBV_QP_PKEY_INDEX |
                      IBV_QP_PORT | IBV_QP_ACCESS_FLAGS)) die("QP -> INIT");

    memset(&attr, 0, sizeof attr);
    attr.qp_state              = IBV_QPS_RTR;
    attr.path_mtu              = IBV_MTU_1024;
    attr.dest_qp_num           = remote.qpn;
    attr.rq_psn                = remote.psn;
    attr.max_dest_rd_atomic    = 1;
    attr.min_rnr_timer         = 12;
    attr.ah_attr.is_global     = 1;
    attr.ah_attr.dlid          = remote.lid;
    attr.ah_attr.port_num      = 1;
    attr.ah_attr.grh.hop_limit = 1;
    attr.ah_attr.grh.sgid_index = r->gid_index;
    memcpy(&attr.ah_attr.grh.dgid, remote.gid, 16);
    if (ibv_modify_qp(r->qp, &attr, IBV_QP_STATE | IBV_QP_AV | IBV_QP_PATH_MTU |
                      IBV_QP_DEST_QPN | IBV_QP_RQ_PSN | IBV_QP_MAX_DEST_RD_ATOMIC |
                      IBV_QP_MIN_RNR_TIMER)) die("QP -> RTR");

    memset(&attr, 0, sizeof attr);
    attr.qp_state      = IBV_QPS_RTS;
    attr.timeout       = 14;
    attr.retry_cnt     = 7;
    attr.rnr_retry     = 7;
    attr.sq_psn        = 0;         // matches the psn we sent
    attr.max_rd_atomic = 1;
    if (ibv_modify_qp(r->qp, &attr, IBV_QP_STATE | IBV_QP_TIMEOUT |
                      IBV_QP_RETRY_CNT | IBV_QP_RNR_RETRY | IBV_QP_SQ_PSN |
                      IBV_QP_MAX_QP_RD_ATOMIC)) die("QP -> RTS");
}

/* ---------------- TCP bootstrap (connection-info exchange only) ----------- */
static void xwrite(int fd, const void *buf, size_t n) {
    const char *p = buf;
    while (n) { ssize_t w = write(fd, p, n); if (w <= 0) die("tcp write"); p += w; n -= w; }
}
static void xread(int fd, void *buf, size_t n) {
    char *p = buf;
    while (n) { ssize_t w = read(fd, p, n); if (w <= 0) die("tcp read"); p += w; n -= w; }
}

static int tcp_handshake(int is_master, const char *peer_ip) {
    int fd;
    if (is_master) {
        int l = socket(AF_INET, SOCK_STREAM, 0);
        int one = 1;
        setsockopt(l, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
        struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(TCP_PORT) };
        a.sin_addr.s_addr = INADDR_ANY;
        if (bind(l, (void *)&a, sizeof a) || listen(l, 1)) die("bind/listen :%d", TCP_PORT);
        fprintf(stderr, "[master] waiting for worker on tcp/%d ...\n", TCP_PORT);
        fd = accept(l, NULL, NULL);
        if (fd < 0) die("accept");
        close(l);
    } else {
        fd = socket(AF_INET, SOCK_STREAM, 0);
        struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(TCP_PORT) };
        if (inet_pton(AF_INET, peer_ip, &a.sin_addr) != 1) die("bad ip %s", peer_ip);
        if (connect(fd, (void *)&a, sizeof a)) die("connect %s:%d", peer_ip, TCP_PORT);
    }
    return fd;
}

/* ---------------- main ---------------------------------------------------- */
int main(int argc, char **argv) {
    int is_master;
    const char *peer_ip = NULL;
    const char *devname = NULL;
    if (argc >= 2 && !strcmp(argv[1], "--listen")) { is_master = 1; }
    else if (argc >= 3 && !strcmp(argv[1], "--connect")) { is_master = 0; peer_ip = argv[2]; }
    else die("usage: %s --listen | --connect <master-ip> [--dev D] [--seed=N]", argv[0]);
    int base = is_master ? 2 : 3;
    // distinct default streams per role; identical seeds would duplicate walkers
    uint64_t seed = is_master ? 0x9E3779B97F4A7C15ull : 0xD1B54A32D192ED03ull;
    for (int i = base; i < argc; i++) {
        if (!strcmp(argv[i], "--dev") && i + 1 < argc)
            devname = argv[++i];
        else if (!strncmp(argv[i], "--seed=", 7))
            seed = strtoull(argv[i] + 7, NULL, 0);
        else
            die("usage: %s --listen | --connect <master-ip> [--dev D] [--seed=N]", argv[0]);
    }
    if (!devname) devname = is_master ? "rxe0" : "rxe1";
    rng_state = seed;

    struct rdma_ctx *r = xaligned(sizeof *r);

    int ndev;
    struct ibv_device **devs = ibv_get_device_list(&ndev);
    if (!devs || ndev == 0) die("no RDMA device found (fabric up? rxe0 in this ns?)");
    struct ibv_device *pick = NULL;
    for (int i = 0; i < ndev; i++)
        if (!strcmp(ibv_get_device_name(devs[i]), devname)) pick = devs[i];
    if (!pick) {
        fprintf(stderr, "available devices:");
        for (int i = 0; i < ndev; i++) fprintf(stderr, " %s", ibv_get_device_name(devs[i]));
        fprintf(stderr, "\n");
        die("device %s not found", devname);
    }
    r->ctx = ibv_open_device(pick);
    if (!r->ctx) die("ibv_open_device %s", devname);
    fprintf(stderr, "[%s] using device %s, seed 0x%llx\n",
            is_master ? "master" : "worker", devname,
            (unsigned long long)seed);

    r->pd = ibv_alloc_pd(r->ctx);
    r->cq = ibv_create_cq(r->ctx, 256, NULL, NULL, 0);
    if (!r->pd || !r->cq) die("pd/cq");

    struct ibv_qp_init_attr qa = {
        .send_cq = r->cq, .recv_cq = r->cq,
        .cap = { .max_send_wr = 64, .max_recv_wr = N_RECVS + 8,
                 .max_send_sge = 1, .max_recv_sge = 1 },
        .qp_type = IBV_QPT_RC,
    };
    r->qp = ibv_create_qp(r->pd, &qa);
    if (!r->qp) die("ibv_create_qp");

    r->stats   = xaligned(sizeof *r->stats);   r->mr_stats   = reg(r, r->stats,   sizeof *r->stats);
    r->ctrl    = xaligned(sizeof *r->ctrl);    r->mr_ctrl    = reg(r, r->ctrl,    sizeof *r->ctrl);
    r->inbox   = xaligned(sizeof *r->inbox);   r->mr_inbox   = reg(r, r->inbox,   sizeof *r->inbox);
    r->stage   = xaligned(sizeof *r->stage);   r->mr_stage   = reg(r, r->stage,   sizeof *r->stage);
    r->scratch = xaligned(sizeof *r->scratch); r->mr_scratch = reg(r, r->scratch, sizeof *r->scratch);
    r->flag    = xaligned(sizeof *r->flag);    r->mr_flag    = reg(r, r->flag,    sizeof *r->flag);

    pick_gid(r);

    struct wire_info local = {
        .lid = r->lid, .gid_index = r->gid_index, .qpn = r->qp->qp_num, .psn = 0,
        .addr_stats = (uintptr_t)r->stats, .rkey_stats = r->mr_stats->rkey,
        .addr_ctrl  = (uintptr_t)r->ctrl,  .rkey_ctrl  = r->mr_ctrl->rkey,
        .addr_inbox = (uintptr_t)r->inbox, .rkey_inbox = r->mr_inbox->rkey,
    };
    memcpy(local.gid, &r->gid, 16);

    int tcp = tcp_handshake(is_master, peer_ip);
    xwrite(tcp, &local, sizeof local);
    xread(tcp, &remote, sizeof remote);
    close(tcp);

    qp_to_rts(r);
    post_recvs(r, N_RECVS);   // QP must be out of RESET before RECVs post
    fprintf(stderr, "[%s] QP up (RESET->INIT->RTR->RTS), remote qpn %u\n",
            is_master ? "master" : "worker", remote.qpn);

    /* ---------------- the distributed DMC loop ---------------- */
    struct population *p = xaligned(sizeof *p);
    p->n = LOCAL_TGT;
    p->E_T = 0.55;
    for (int i = 0; i < p->n; i++) p->x[i] = gauss();

    int rounds = GENS / SYNC_GENS;
    for (int round = 1; round <= rounds; round++) {
        /* Drain any walker arrival first: a peer waiting for inbox space can
           then never deadlock against the E_T exchange below. */
        absorb_inbox(r, p);

        double sum = 0.0;
        uint64_t samples = 0;
        for (int g = 0; g < SYNC_GENS; g++) {
            double E_gen = dmc_generation(p, &sum, &samples);
            int ggen = (round - 1) * SYNC_GENS + g;
            if (ggen >= EQUIL) { p->E_acc += E_gen; p->E_cnt++; }
        }

        /* publish stats; gen goes last with release ordering */
        r->stats->sum_EL  = sum;
        r->stats->samples = samples;
        r->stats->n       = p->n;
        __sync_synchronize();
        r->stats->gen = round;

        double E_T;
        if (is_master) {
            /* RDMA READ the worker's stats until its round matches */
            int tries = 0;
            for (;;) {
                rdma_op(r, IBV_WR_RDMA_READ, r->mr_scratch->lkey, r->scratch,
                        sizeof *r->scratch, remote.addr_stats, remote.rkey_stats, 0);
                __sync_synchronize();
                if (r->scratch->gen >= (uint64_t)round) break;
                if (++tries > 100000) die("worker stats never reached round %d", round);
                usleep(50);
            }
            uint64_t tot_samples = samples + r->scratch->samples;
            int      tot_n       = p->n + r->scratch->n;
            double E_round = (sum + r->scratch->sum_EL) / tot_samples;
            /* branching grows with E_T: overpopulation must LOWER E_T */
            E_T = E_round - FEEDBACK * log((double)tot_n / TARGET);

            r->ctrl->E_T = E_T;
            __sync_synchronize();
            r->ctrl->gen = round;
            /* push the new reference energy into the worker, one-sided */
            rdma_op(r, IBV_WR_RDMA_WRITE, r->mr_ctrl->lkey, r->ctrl,
                    sizeof *r->ctrl, remote.addr_ctrl, remote.rkey_ctrl, 0);
        } else {
            int spins = 0;
            while (r->ctrl->gen < (uint64_t)round) {
                if (++spins > 200000000) die("master control never reached round %d", round);
                __sync_synchronize();
            }
            E_T = r->ctrl->E_T;
        }
        p->E_T = E_T;

        /* load balancing: push surplus walkers into the peer's inbox. The
           inbox is single-slot: n == 0 means free. READ the peer's flag until
           free, then push — single writer per inbox, so observing free owns
           the slot until the peer's absorb_inbox releases it. */
        int surplus = p->n - LOCAL_TGT;
        if (surplus > 8) {
            for (int tries = 0;; tries++) {
                rdma_op(r, IBV_WR_RDMA_READ, r->mr_flag->lkey, r->flag,
                        sizeof *r->flag, remote.addr_inbox, remote.rkey_inbox, 0);
                __sync_synchronize();
                if (*r->flag == 0) break;
                if (tries > 100000) die("peer inbox never drained");
                usleep(50);
            }
            uint32_t k = surplus > MAX_MIG ? MAX_MIG : (uint32_t)surplus;
            r->stage->n = k;
            memcpy(r->stage->x, p->x + p->n - k, k * sizeof(double));
            __sync_synchronize();
            rdma_op(r, IBV_WR_RDMA_WRITE_WITH_IMM, r->mr_stage->lkey, r->stage,
                    offsetof(struct walker_inbox, x) + k * sizeof(double),
                    remote.addr_inbox, remote.rkey_inbox, k);
            p->n -= k;
        }

        if (round % 10 == 0 || round == 1)
            fprintf(stderr, "[%s] round %3d  N=%4d  E_T=%.4f\n",
                    is_master ? "master" : "worker", round, p->n, p->E_T);
    }

    printf("[%s] DMC result: E0 = %.4f over %ld post-equil generations, "
           "final local population %d (exact 0.5000)\n",
           is_master ? "master" : "worker",
           p->E_cnt ? p->E_acc / p->E_cnt : 0.0, p->E_cnt, p->n);
    return 0;
}
