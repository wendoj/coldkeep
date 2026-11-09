/* SPDX-License-Identifier: Apache-2.0
 * Run metrics: minutes above limit, peaks, longest defrost, verdict counts.
 * The compare view's readouts are computed from these, never typed in. */
#include "coldkeep.h"

#include <string.h>

void ck_metrics_init(ck_metrics_t *m, uint16_t n, const ck_case_state_t *st) {
  memset(m, 0, sizeof *m);
  m->n = n > CK_MAX_CASES ? CK_MAX_CASES : n;
  for (uint16_t i = 0; i < m->n; i++) {
    const ck_thermal_params_t *p = ck_thermal_defaults(st[i].type);
    ck_case_metrics_t *c = &m->c[i];
    c->limit_c = p->limit;
    c->food_kg = p->food_kg;
    c->peak_product_c = st[i].product_c;
    c->peak_air_c = st[i].air_c;
  }
  m->compliance_period_ns = CK_S(15 * 60);
  m->next_compliance_ns = 0;
}

void ck_metrics_sample(ck_metrics_t *m, const ck_case_state_t *st, uint64_t now, float dt_s) {
  for (uint16_t i = 0; i < m->n; i++) {
    ck_case_metrics_t *c = &m->c[i];
    const ck_case_state_t *s = &st[i];
    if (s->product_c > c->peak_product_c) c->peak_product_c = s->product_c;
    if (s->air_c > c->peak_air_c) c->peak_air_c = s->air_c;
    const uint8_t above = s->product_c > c->limit_c;
    if (above) c->s_above += dt_s;
    if (above && !c->above) c->excursions++;
    c->above = above;
    if (s->defrost_on && !c->in_defrost) { c->in_defrost = 1; c->cur_defrost_start_ns = now; }
    if (c->in_defrost) {
      uint64_t len_s = (now - c->cur_defrost_start_ns) / CK_NS_PER_S;
      if (len_s > c->defrost_s_max) c->defrost_s_max = len_s;
      if (!s->defrost_on) c->in_defrost = 0;
    }
  }
}

void ck_metrics_decision(ck_metrics_t *m, ck_verdict_t v) {
  if (v >= VERDICT_ALLOW && v <= VERDICT_NEEDS_KEY) m->verdicts[v]++;
  if (v == VERDICT_ALLOW || v == VERDICT_CLAMP) m->applied++;
  else if (v == VERDICT_DENY || v == VERDICT_NEEDS_KEY) m->blocked++;
}

double ck_metrics_minutes_above(const ck_metrics_t *m, uint16_t i) {
  return i < m->n ? m->c[i].s_above / 60.0 : 0.0;
}

double ck_metrics_total_minutes_above(const ck_metrics_t *m) {
  double t = 0;
  for (uint16_t i = 0; i < m->n; i++) t += m->c[i].s_above / 60.0;
  return t;
}

float ck_metrics_food_at_risk_kg(const ck_metrics_t *m) {
  float kg = 0;
  for (uint16_t i = 0; i < m->n; i++)
    if (m->c[i].s_above > 0) kg += m->c[i].food_kg;
  return kg;
}

bool ck_metrics_compliance_due(ck_metrics_t *m, uint64_t now) {
  if (now < m->next_compliance_ns) return false;
  m->next_compliance_ns = now + m->compliance_period_ns;
  return true;
}
