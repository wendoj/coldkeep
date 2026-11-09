/* SPDX-License-Identifier: Apache-2.0 — metrics: minutes above limit, peaks, counts */
#include "ck_test.h"
#include "coldkeep.h"

static void minutes_and_peaks(void) {
  ck_case_state_t st[2];
  ck_thermal_init(&st[0], 1, CASE_CHILLED, 20);
  ck_thermal_init(&st[1], 2, CASE_FROZEN, 20);
  ck_metrics_t m; ck_metrics_init(&m, 2, st);
  uint64_t now = 0;
  /* 10 min below, 15 min above (chilled at 9 °C), 5 min below */
  for (int k = 0; k < 30 * 60; k++) {
    st[0].product_c = (k >= 600 && k < 1500) ? 9.0f : 3.0f;
    if (k == 700) st[0].air_c = 30.0f;
    now += CK_NS_PER_S;
    ck_metrics_sample(&m, st, now, 1.0f);
  }
  CHECK_NEAR(ck_metrics_minutes_above(&m, 0), 15.0, 1e-9);
  CHECK_NEAR(ck_metrics_minutes_above(&m, 1), 0.0, 1e-9);
  CHECK_NEAR(ck_metrics_total_minutes_above(&m), 15.0, 1e-9);
  CHECK(m.c[0].peak_product_c == 9.0f);
  CHECK(m.c[0].peak_air_c == 30.0f);
  CHECK_EQ_INT(m.c[0].excursions, 1);
  CHECK(ck_metrics_food_at_risk_kg(&m) == ck_thermal_defaults(CASE_CHILLED)->food_kg);
}

static void counts(void) {
  ck_case_state_t st[1]; ck_thermal_init(&st[0], 1, CASE_CHILLED, 20);
  ck_metrics_t m; ck_metrics_init(&m, 1, st);
  ck_metrics_decision(&m, VERDICT_ALLOW);
  ck_metrics_decision(&m, VERDICT_CLAMP);
  ck_metrics_decision(&m, VERDICT_DENY);
  ck_metrics_decision(&m, VERDICT_NEEDS_KEY);
  ck_metrics_decision(&m, VERDICT_DENY);
  CHECK_EQ_INT(m.applied, 2);
  CHECK_EQ_INT(m.blocked, 3);
  CHECK_EQ_INT(m.verdicts[VERDICT_DENY], 2);
}

static void longest_defrost(void) {
  ck_case_state_t st[1]; ck_thermal_init(&st[0], 1, CASE_CHILLED, 20);
  ck_metrics_t m; ck_metrics_init(&m, 1, st);
  uint64_t now = 0;
  for (int k = 0; k < 4000; k++) {
    st[0].defrost_on = (k >= 100 && k < 400) || (k >= 1000 && k < 3700);
    now += CK_NS_PER_S;
    ck_metrics_sample(&m, st, now, 1.0f);
  }
  CHECK_NEAR((double)m.c[0].defrost_s_max, 2700.0, 1.0);
}

static void compliance_period(void) {
  ck_case_state_t st[1]; ck_thermal_init(&st[0], 1, CASE_CHILLED, 20);
  ck_metrics_t m; ck_metrics_init(&m, 1, st);
  int due = 0;
  for (uint64_t t = 0; t <= CK_S(3600); t += CK_S(1)) due += ck_metrics_compliance_due(&m, t);
  CHECK_EQ_INT(due, 5); /* 0, 15, 30, 45, 60 min */
}

void suite_metrics(void) {
  RUN(minutes_and_peaks);
  RUN(counts);
  RUN(longest_defrost);
  RUN(compliance_period);
}
