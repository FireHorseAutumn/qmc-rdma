// dmc_serial.c — reference diffusion Monte Carlo, 1D harmonic oscillator.
//
// Importance-sampled DMC for H = -1/2 d^2/dx^2 + 1/2 x^2  (hbar=m=omega=1),
// exact ground state E0 = 0.5. Trial function psi_T(x) = exp(-alpha x^2 / 2)
// with alpha != 1 so the local energy is nontrivial:
//
//   E_L(x) = H psi_T / psi_T = alpha/2 + (1 - alpha^2) x^2 / 2
//   drift b(x) = d/dx ln psi_T = -alpha x
//
// Imaginary-time evolution of f = psi_T * psi is drift-diffusion-branching:
//   x  <- x + tau*b(x) + sqrt(tau)*N(0,1)      (drift, then diffuse)
//   copies = floor(exp(-tau*(E_L - E_T)) + u)  (branching, u ~ U[0,1))
//
// E_T (reference energy) is steered to hold the population at target:
//   E_T <- E_gen - FEEDBACK * log(n_post_branch / TARGET)
// The branching factor grows with E_T, so an overpopulation must LOWER E_T;
// the sign matters. E_gen is the pre-branching mean of E_L over the moved
// walkers — numerator and denominator must describe the same population.
//
// Mixed estimator: E0 ~ <E_L> over walkers, averaged after equilibration.
//
// This is the physics the RDMA version (qmc_dmc_rdma.c) distributes across
// two nodes; run this first to know the right answer.
//
// Build: gcc -O2 -Wall -o dmc_serial dmc_serial.c -lm
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ALPHA      0.8     // trial-function width (1.0 = exact ground state)
#define TAU        0.01    // imaginary time step
#define TARGET     1000    // target walker population
#define MAXW       8000    // walker array capacity
#define GENS       3000
#define EQUIL      500     // generations discarded before accumulating
#define FEEDBACK   0.1     // E_T population-feedback gain

static uint64_t rng_state = 0x9E3779B97F4A7C15ull;

static uint64_t xorshift(void) {
    uint64_t x = rng_state;
    x ^= x << 13; x ^= x >> 7; x ^= x << 17;
    rng_state = x;
    return x;
}

static double urand(void) { return (xorshift() >> 11) * (1.0 / 9007199254740992.0); }

static double gauss(void) {  // Box-Muller
    double u1 = urand(); if (u1 < 1e-300) u1 = 1e-300;
    double u2 = urand();
    return sqrt(-2.0 * log(u1)) * cos(2.0 * M_PI * u2);
}

static double local_energy(double x) {
    return ALPHA / 2.0 + (1.0 - ALPHA * ALPHA) * x * x / 2.0;
}

int main(int argc, char **argv) {
    double tau = TAU;
    int target = TARGET, gens = GENS, equil = EQUIL;
    int quiet = 0;
    for (int i = 1; i < argc; i++) {
        if (strncmp(argv[i], "--seed=", 7) == 0)
            rng_state = strtoull(argv[i] + 7, NULL, 0);
        else if (strncmp(argv[i], "--tau=", 6) == 0)
            tau = strtod(argv[i] + 6, NULL);
        else if (strncmp(argv[i], "--target=", 9) == 0)
            target = atoi(argv[i] + 9);
        else if (strncmp(argv[i], "--gens=", 7) == 0)
            gens = atoi(argv[i] + 7);
        else if (strncmp(argv[i], "--equil=", 8) == 0)
            equil = atoi(argv[i] + 8);
        else if (strcmp(argv[i], "--quiet") == 0)
            quiet = 1;
        else {
            fprintf(stderr, "unknown argument: %s\n"
                    "usage: dmc_serial [--seed=N] [--tau=X] [--target=N] "
                    "[--gens=N] [--equil=N] [--quiet]\n", argv[i]);
            return 1;
        }
    }
    // isfinite first: NaN passes tau <= 0.0 (NaN comparisons are false)
    if (!isfinite(tau) || tau <= 0.0 || target < 1 || target > MAXW ||
        gens < 1 || equil < 0 || equil >= gens) {
        fprintf(stderr, "bad parameters: tau=%g target=%d gens=%d equil=%d\n",
                tau, target, gens, equil);
        return 1;
    }
    const uint64_t seed = rng_state;

    double *walkers = malloc(MAXW * sizeof(double));
    double *next    = malloc(MAXW * sizeof(double));
    if (!walkers || !next) { perror("malloc"); return 1; }

    int n = target;
    for (int i = 0; i < n; i++) walkers[i] = gauss();  // diffuse start
    double E_T = 0.55;  // initial reference energy guess

    double E_acc = 0.0;
    long   E_cnt = 0;
    double pop_acc = 0.0;  // post-equilibration population sum

    for (int g = 0; g < gens; g++) {
        int m = 0;
        double sum_EL = 0.0;

        for (int i = 0; i < n; i++) {
            double x = walkers[i];
            x += tau * (-ALPHA * x);        // drift
            x += sqrt(tau) * gauss();       // diffuse
            double EL = local_energy(x);
            sum_EL += EL;

            int copies = (int)floor(exp(-tau * (EL - E_T)) + urand());
            if (copies > 3) copies = 3;     // bound replication fan-out
            for (int c = 0; c < copies && m < MAXW; c++) next[m++] = x;
        }
        if (m >= MAXW) fprintf(stderr, "gen %d: population hit MAXW cap\n", g);
        if (m == 0) { fprintf(stderr, "gen %d: population died\n", g); return 1; }

        // pre-branch sum over the pre-branch count: same population
        double E_gen = sum_EL / n;
        n = m;
        double *tmp = walkers; walkers = next; next = tmp;

        // overpopulation must lower E_T (branching grows with E_T)
        E_T = E_gen - FEEDBACK * log((double)n / target);

        if (g >= equil) { E_acc += E_gen; E_cnt++; pop_acc += n; }
        if (!quiet && g % 500 == 0)
            printf("gen %4d  N=%5d  E_gen=%.4f  E_T=%.4f\n", g, n, E_gen, E_T);
    }

    double E0 = E_acc / E_cnt;
    if (quiet) {
        printf("RESULT seed=%llu tau=%g gens=%d equil=%d target=%d "
               "E0=%.6f pop_final=%d pop_mean=%.2f\n",
               (unsigned long long)seed, tau, gens, equil, target,
               E0, n, pop_acc / E_cnt);
    } else {
        printf("\nDMC result:  E0 = %.4f  (exact 0.5000, alpha=%.1f, tau=%.3g)\n",
               E0, ALPHA, tau);
        printf("population ended at %d walkers (target %d, post-equil mean %.1f)\n",
               n, target, pop_acc / E_cnt);
    }
    free(walkers); free(next);
    return 0;
}
