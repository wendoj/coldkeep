/* SPDX-License-Identifier: Apache-2.0
 * Thermal model sanity: steady state near setpoint, forced defrost heats,
 * icing degrades cooling. Uses the real controller for the thermostat. */
#include "ck_test.h"
#include "coldkeep.h"

typedef struct { float air_min, air_max, prod_min, prod_max; double t_air20_min, t_prod_lim_min; } run_t;

/* Run one case for `hours`; optionally force a defrost at t=0 with `force_s`. */
static run_t simulate(ck_case_type_t t, float hours, uint32_t force_s, float term) {
  ck_case_state_t s; ck_ctl_t c;
  ck_thermal_init(&s, 1, t, 20.0f);
  ck_ctl_init(&c, 1, t, false, 0);
  const ck_thermal_params_t *p = ck_thermal_defaults(t);
  uint64_t now = 0;
  /* settle 2 h first */
  for (int k = 0; k < 2 * 36000; k++) {
    ck_probe_t pr = {s.air_c, s.product_c, s.evap_c};
    ck_ctl_tick(&c, &pr, now, &s); ck_thermal_step(&s, p, 0.1f); now += CK_TICK_NS;
  }
  if (force_s) {
    ck_case_cmd_t cc = {.type = CASE_CMD_START_DEFROST, .duration_s = force_s, .term_c = term};
    ck_ctl_command(&c, &cc, now);
  }
  run_t r = {99, -99, 99, -99, -1, -1};
  const int steps = (int)(hours * 36000.0f);
  for (int k = 0; k < steps; k++) {
    ck_probe_t pr = {s.air_c, s.product_c, s.evap_c};
    ck_ctl_tick(&c, &pr, now, &s); ck_thermal_step(&s, p, 0.1f); now += CK_TICK_NS;
    if (s.air_c < r.air_min) r.air_min = s.air_c;
    if (s.air_c > r.air_max) r.air_max = s.air_c;
    if (s.product_c < r.prod_min) r.prod_min = s.product_c;
    if (s.product_c > r.prod_max) r.prod_max = s.product_c;
    if (r.t_air20_min < 0 && s.air_c > 20.0f) r.t_air20_min = k / 600.0;
    if (r.t_prod_lim_min < 0 && s.product_c > p->limit) r.t_prod_lim_min = k / 600.0;
  }
  return r;
}

static void steady_state_near_setpoint(void) {
  for (int t = CASE_CHILLED; t <= CASE_COLDROOM; t++) {
    const ck_thermal_params_t *p = ck_thermal_defaults((ck_case_type_t)t);
    run_t r = simulate((ck_case_type_t)t, 4.0f, 0, 0);
    CHECK_MSG(r.air_min > p->setpoint - p->hyst - 0.5f && r.air_max < p->setpoint + p->hyst + 0.5f,
              "%s air %.2f..%.2f", ck_case_type_name((ck_case_type_t)t), r.air_min, r.air_max);
    CHECK_MSG(r.prod_min > p->setpoint - 0.5f && r.prod_max < p->setpoint + 0.5f,
              "%s prod %.2f..%.2f", ck_case_type_name((ck_case_type_t)t), r.prod_min, r.prod_max);
  }
}

static void forced_defrost_heats_chilled(void) {
  /* calibration target: forced continuous defrost drives chilled air past 20 °C in ~30–60 min */
  run_t r = simulate(CASE_CHILLED, 2.0f, 86400, CK_TERM_NONE);
  CHECK_MSG(r.t_air20_min >= 30.0 && r.t_air20_min <= 60.0, "air>20 at %.1f min", r.t_air20_min);
  CHECK(r.t_prod_lim_min > 0);
  CHECK(r.air_max > 35.0f);
}

static void terminated_defrost_is_benign(void) {
  run_t r = simulate(CASE_CHILLED, 2.0f, 45 * 60, 10.0f);
  CHECK_MSG(r.prod_max < 4.0f, "prod max %.2f", r.prod_max);
  r = simulate(CASE_FROZEN, 2.0f, 60 * 60, 10.0f);
  CHECK_MSG(r.prod_max < -21.0f, "frozen prod max %.2f", r.prod_max);
}

static void icing_degrades_cooling(void) {
  /* Frozen coil never rises above 0 °C without defrost, so frost accumulates. */
  ck_case_state_t s; ck_ctl_t c;
  ck_thermal_init(&s, 1, CASE_FROZEN, 20.0f);
  s.ice_j = 0;
  ck_ctl_init(&c, 1, CASE_FROZEN, false, 0);
  const ck_thermal_params_t *p = ck_thermal_defaults(CASE_FROZEN);
  uint64_t now = 0;
  float eff0 = ck_thermal_cool_eff(&s, p);
  double duty_first = 0, duty_last = 0;
  const int hour = 36000;
  for (int k = 0; k < 24 * hour; k++) {
    ck_probe_t pr = {s.air_c, s.product_c, s.evap_c};
    ck_ctl_tick(&c, &pr, now, &s); ck_thermal_step(&s, p, 0.1f); now += CK_TICK_NS;
    if (k >= 1 * hour && k < 3 * hour) duty_first += s.compressor_on;
    if (k >= 22 * hour) duty_last += s.compressor_on;
  }
  CHECK(eff0 == 1.0f);
  CHECK_MSG(ck_thermal_cool_eff(&s, p) < 0.6f, "eff %.2f", ck_thermal_cool_eff(&s, p));
  CHECK_MSG(duty_last > duty_first * 1.2, "duty %.0f -> %.0f", duty_first, duty_last);
  /* a defrost melts it */
  ck_case_cmd_t cc = {.type = CASE_CMD_START_DEFROST, .duration_s = 3600, .term_c = 10.0f};
  CHECK_EQ_INT(ck_ctl_command(&c, &cc, now), 0);
  for (int k = 0; k < 3600 * 10; k++) {
    ck_probe_t pr = {s.air_c, s.product_c, s.evap_c};
    ck_ctl_tick(&c, &pr, now, &s); ck_thermal_step(&s, p, 0.1f); now += CK_TICK_NS;
  }
  CHECK_MSG(s.ice_j < 0.3e6f, "ice after defrost %.0f", s.ice_j);
}

static void bit_reproducible(void) {
  run_t a = simulate(CASE_CHILLED, 1.0f, 86400, CK_TERM_NONE);
  run_t b = simulate(CASE_CHILLED, 1.0f, 86400, CK_TERM_NONE);
  CHECK(memcmp(&a, &b, sizeof a) == 0);
}

void suite_thermal(void) {
  RUN(steady_state_near_setpoint);
  RUN(forced_defrost_heats_chilled);
  RUN(terminated_defrost_is_benign);
  RUN(icing_degrades_cooling);
  RUN(bit_reproducible);
}
