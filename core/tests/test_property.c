/* SPDX-License-Identifier: Apache-2.0
 *
 * Property tests: random command streams from an authenticated
 * mgmt client against a PROTECTED store (plant + controllers + broker + governor),
 * checking after every plant step:
 *
 *   1. product never exceeds its food-safe limit (chilled 8, frozen -18, coldroom 5 °C)
 *   2. no defrost cycle runs longer than the G3 maximum for its case type
 *   3. the envelope never changes (no keyconsole approvals are issued here)
 *
 * Each run also kills the governor once at a random time (exercising G6).
 * Runs/commands: CK_PROP_RUNS (default 1000), CK_PROP_CMDS (default 1000).
 * Plant step is 1 s here (0.1 s in sim-host) to keep 10k-run sweeps fast.
 */
#include <math.h>
#include <stdlib.h>
#include <time.h>

#include "ck_test.h"
#include "coldkeep.h"

int ck_t_fail, ck_t_pass;
const char *ck_t_name = "property";

static const ck_case_type_t types[4] = {CASE_CHILLED, CASE_FROZEN, CASE_COLDROOM, CASE_CHILLED};

static float random_value(ck_rng_t *r, ck_cmd_type_t t, const ck_store_t *s, uint16_t case_id) {
  const uint32_t mode = ck_rng_below(r, 10);
  if (mode == 0) { /* hostile extremes */
    static const float wild[] = {1e9f, -1e9f, 0.0f, -0.0f, 60.0f, -60.0f, 604800.0f, 1.0f};
    return wild[ck_rng_below(r, 8)];
  }
  if (mode == 1) return NAN;
  switch (t) {
    case CMD_SET_SETPOINT: {
      if (case_id >= 1 && case_id <= s->n && ck_rng_below(r, 2)) {
        const ck_env_type_t *e = &s->gov.env.t[s->plant.st[case_id - 1].type];
        /* probe band edges and small steps */
        return ck_rng_range(r, e->sp_min - 3.0f, e->sp_max + 3.0f);
      }
      return ck_rng_range(r, -40.0f, 40.0f);
    }
    case CMD_START_DEFROST: return ck_rng_range(r, 1.0f, 86400.0f);
    case CMD_SET_FANS: return (float)ck_rng_below(r, 2);
    default: return ck_rng_range(r, -50.0f, 50.0f);
  }
}

typedef struct { uint64_t runs, cmds, steps, allowed, clamped, denied, needs_key; float worst_margin[4]; } stats_t;

static int one_run(uint64_t run, uint32_t ncmds, stats_t *S) {
  static ck_store_t s;
  const uint64_t seed = ck_rng_derive(0xC01DCAFEULL, run);
  ck_store_init(&s, MODE_PROTECTED, seed, 4, types, 20.0f, 1.0f);
  ck_rng_t r; ck_rng_seed(&r, ck_rng_derive(seed, 999));
  ck_store_login(&s, 1, "admin", "admin");
  ck_store_login(&s, 2, "admin", "admin");

  uint64_t defrost_start[4] = {0};
  uint8_t in_def[4] = {0};
  const uint32_t kill_at = ck_rng_below(&r, ncmds);
  const bool hostile = (run & 1) != 0; /* odd runs: greedy attacker strategy */

  for (uint32_t k = 0; k < ncmds; k++) {
    /* gap: bursts (0 s) mixed with quiet periods up to 30 s */
    const uint32_t gap = ck_rng_below(&r, 4) == 0 ? 0 : ck_rng_below(&r, 31);
    for (uint32_t g = 0; g < gap; g++) {
      ck_store_step(&s);
      S->steps++;
      for (int i = 0; i < 4; i++) {
        const ck_env_type_t *e = &s.gov.env.t[s.plant.st[i].type];
        const float lim = s.m.c[i].limit_c;
        const float margin = lim - s.plant.st[i].product_c;
        if (margin < S->worst_margin[i]) S->worst_margin[i] = margin;
        if (s.plant.st[i].product_c > lim) {
          fprintf(stderr, "INVARIANT 1 violated: run %llu seed %llx case %d (%s) product %.3f > %.1f at t=%.0fs\n",
                  (unsigned long long)run, (unsigned long long)seed, i + 1, ck_case_type_name(s.plant.st[i].type),
                  (double)s.plant.st[i].product_c, (double)lim, (double)s.now / 1e9);
          return 1;
        }
        if (s.plant.st[i].defrost_on && !in_def[i]) { in_def[i] = 1; defrost_start[i] = s.now; }
        if (!s.plant.st[i].defrost_on) in_def[i] = 0;
        if (in_def[i] && s.now - defrost_start[i] > CK_S(e->defrost_max_s) + CK_S(1)) {
          fprintf(stderr, "INVARIANT 2 violated: run %llu case %d defrost %.0f s > %u s\n",
                  (unsigned long long)run, i + 1, (double)(s.now - defrost_start[i]) / 1e9, e->defrost_max_s);
          return 1;
        }
      }
      if (s.gov.env.version != 1 || s.gov.key_approvals != 0) {
        fprintf(stderr, "INVARIANT 3 violated: run %llu envelope v%u approvals %u\n",
                (unsigned long long)run, s.gov.env.version, s.gov.key_approvals);
        return 1;
      }
    }
    if (k == kill_at) ck_store_kill_governor(&s, ck_rng_range(&r, 1.0f, 60.0f));

    ck_cmd_type_t t;
    uint16_t case_id;
    float v;
    if (hostile && ck_rng_below(&r, 4) != 0) {
      /* greedy attacker: warmest legal setpoint + max defrost as often as allowed */
      case_id = (uint16_t)(1 + ck_rng_below(&r, 4));
      switch (ck_rng_below(&r, 3)) {
        /* always ask for +2 °C (the G2 step limit), never self-capped at the band */
        case 0: t = CMD_SET_SETPOINT; v = s.gov.c[case_id - 1].setpoint + 2.0f; break;
        case 1: t = CMD_START_DEFROST; v = 86400.0f; break;
        default: t = CMD_SET_FANS; v = 0.0f; break;
      }
    } else {
      t = (ck_cmd_type_t)(1 + ck_rng_below(&r, CK_CMD_MAX));
      case_id = (uint16_t)ck_rng_below(&r, 6); /* 0..5: includes invalid ids */
      v = random_value(&r, t, &s, case_id);
    }
    ck_decision_t d;
    const ck_verdict_t verdict = ck_store_mgmt(&s, 1 + ck_rng_below(&r, 2), t, case_id, v, &d);
    S->cmds++;
    S->allowed += verdict == VERDICT_ALLOW;
    S->clamped += verdict == VERDICT_CLAMP;
    S->denied += verdict == VERDICT_DENY;
    S->needs_key += verdict == VERDICT_NEEDS_KEY;
  }
  S->runs++;
  return 0;
}

int main(void) {
  const char *er = getenv("CK_PROP_RUNS"), *ec = getenv("CK_PROP_CMDS");
  const uint64_t runs = er ? strtoull(er, NULL, 10) : 1000;
  const uint32_t cmds = ec ? (uint32_t)strtoul(ec, NULL, 10) : 1000;
  stats_t S = {0};
  for (int i = 0; i < 4; i++) S.worst_margin[i] = 1e9f;
  const clock_t t0 = clock();
  int failed = 0;
  for (uint64_t run = 0; run < runs && !failed; run++) failed = one_run(run, cmds, &S);
  const double secs = (double)(clock() - t0) / CLOCKS_PER_SEC;
  printf("property: %llu runs x %u cmds, %llu commands, %.1f sim-hours, %.1f s cpu\n",
         (unsigned long long)S.runs, cmds, (unsigned long long)S.cmds, (double)S.steps / 3600.0, secs);
  printf("  verdicts ALLOW=%llu CLAMP=%llu DENY=%llu NEEDS_KEY=%llu\n",
         (unsigned long long)S.allowed, (unsigned long long)S.clamped,
         (unsigned long long)S.denied, (unsigned long long)S.needs_key);
  printf("  worst product margin to limit (°C): chilled %.2f, frozen %.2f, coldroom %.2f, chilled %.2f\n",
         (double)S.worst_margin[0], (double)S.worst_margin[1], (double)S.worst_margin[2], (double)S.worst_margin[3]);
  printf("property: %s\n", failed ? "FAILED" : "invariants 1-3 held");
  return failed;
}
