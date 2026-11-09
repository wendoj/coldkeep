/* SPDX-License-Identifier: Apache-2.0
 *
 * Safety governor: rules G1–G5 (G6 is enforced in control.c). See docs/RULES.md.
 *
 * The governor is the only component allowed to issue plant commands in protected
 * mode. It never trusts cmd->user. The envelope changes only through
 * ck_gov_apply_approved(), which requires a keyconsole approval id.
 */
#include "coldkeep.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

void ck_envelope_default(ck_envelope_t *e) {
  memset(e, 0, sizeof *e);
  e->version = 1;
  e->t[CASE_CHILLED] = (ck_env_type_t){
    .sp_min = 0.0f, .sp_max = 5.0f, .sp_default = 3.0f, .warn_c = 6.0f, .limit_c = 8.0f,
    .defrost_max_s = 45 * 60, .sched_defrost_s = 30 * 60, .sched_interval_s = 6 * 3600,
    .term_c = 10.0f};
  e->t[CASE_FROZEN] = (ck_env_type_t){
    .sp_min = -25.0f, .sp_max = -20.0f, .sp_default = -22.0f, .warn_c = -19.0f,
    .limit_c = -18.0f, .defrost_max_s = 60 * 60, .sched_defrost_s = 40 * 60,
    .sched_interval_s = 8 * 3600, .term_c = 10.0f};
  e->t[CASE_COLDROOM] = (ck_env_type_t){
    .sp_min = 0.0f, .sp_max = 3.0f, .sp_default = 2.0f, .warn_c = 4.0f, .limit_c = 5.0f,
    .defrost_max_s = 45 * 60, .sched_defrost_s = 30 * 60, .sched_interval_s = 8 * 3600,
    .term_c = 8.0f};
  e->deny_margin_c = 10.0f;
  e->sp_max_step_c = 2.0f;
  e->sp_changes_per_h = 2;
  e->defrost_min_gap_s = 4 * 3600;
  e->defrost_max_frac = 0.25f;
  e->g4_clear_c = 1.0f;
}

const ck_env_type_t *ck_gov_env_for(const ck_gov_t *g, uint16_t case_id) {
  return &g->env.t[g->types[case_id - 1]];
}

void ck_gov_init(ck_gov_t *g, const ck_envelope_t *env, uint16_t n,
                 const ck_case_type_t *types, uint64_t now) {
  memset(g, 0, sizeof *g);
  g->env = *env;
  g->n = n > CK_MAX_CASES ? CK_MAX_CASES : n;
  for (uint16_t i = 0; i < g->n; i++) {
    g->types[i] = types[i];
    const ck_env_type_t *et = &g->env.t[types[i]];
    ck_gov_case_t *c = &g->c[i];
    c->setpoint = et->sp_default;
    c->lkg_setpoint = et->sp_default;
    c->lkg_candidate_since_ns = now;
    /* stagger scheduled defrosts: first one after one interval, 15 min apart */
    c->next_sched_ns = now + CK_S(et->sched_interval_s) + CK_S(900u * i);
    c->alarms_enabled = true;
  }
  g->next_auto_id = 1ULL << 62;
}

static void remember(ck_gov_t *g, const ck_decision_t *d) {
  g->recent[g->recent_head] = *d;
  g->recent_head = (g->recent_head + 1) % CK_GOV_RECENT;
  if (g->recent_n < CK_GOV_RECENT) g->recent_n++;
  if (d->verdict >= VERDICT_ALLOW && d->verdict <= VERDICT_NEEDS_KEY) g->verdicts[d->verdict]++;
  if (d->rule_id < 16) g->rule_hits[d->rule_id]++;
}

static bool finish(ck_gov_t *g, ck_decision_t *d, ck_verdict_t v, uint32_t rule,
                   float applied, const char *reason) {
  d->verdict = v; d->rule_id = rule; d->applied_value = applied;
  ck_strlcpy(d->reason, reason, sizeof d->reason);
  remember(g, d);
  return v == VERDICT_ALLOW || v == VERDICT_CLAMP;
}

static uint32_t changes_last_hour(ck_gov_case_t *c, uint64_t now) {
  uint32_t k = 0;
  for (uint32_t i = 0; i < c->sp_change_n; i++)
    if (now - c->sp_change_ns[i] < CK_S(3600)) c->sp_change_ns[k++] = c->sp_change_ns[i];
  c->sp_change_n = k;
  return k;
}

static uint32_t defrosting(const ck_gov_t *g, const ck_case_state_t *st) {
  uint32_t k = 0;
  for (uint16_t i = 0; i < g->n; i++) k += st[i].defrost_on ? 1u : 0u;
  return k;
}

static uint32_t max_concurrent(const ck_gov_t *g) {
  uint32_t m = (uint32_t)(g->env.defrost_max_frac * (float)g->n);
  return m < 1 ? 1 : m;
}

static bool is_dangerous(const ck_cmd_t *cmd) {
  switch (cmd->type) {
    case CMD_DISABLE_ALARMS: case CMD_SET_DEFROST_SCHEDULE:
    case CMD_CHANGE_ENVELOPE: case CMD_RAW_WRITE:
      return true;
    case CMD_SET_FANS:
      return cmd->value == 0.0f;
    default:
      return false;
  }
}

bool ck_gov_decide(ck_gov_t *g, const ck_cmd_t *cmd, const ck_case_state_t *st,
                   uint64_t now, ck_decision_t *d, ck_gov_action_t *act) {
  char why[96];
  memset(d, 0, sizeof *d);
  d->cmd = *cmd;
  memset(act, 0, sizeof *act);

  /* G5 first: dangerous commands never apply from here, whoever claims to send them */
  if (is_dangerous(cmd))
    return finish(g, d, VERDICT_NEEDS_KEY, CK_RULE_G5, NAN,
                  "dangerous change: needs local store-key approval");

  if (cmd->case_id < 1 || cmd->case_id > g->n || !isfinite(cmd->value))
    return finish(g, d, VERDICT_DENY, CK_RULE_SCHEMA, NAN, "invalid case or value");

  const uint16_t i = (uint16_t)(cmd->case_id - 1);
  ck_gov_case_t *c = &g->c[i];
  const ck_env_type_t *et = &g->env.t[g->types[i]];
  act->case_id = cmd->case_id;

  switch (cmd->type) {
    case CMD_SET_SETPOINT: {
      float v = cmd->value;
      ck_verdict_t verdict = VERDICT_ALLOW;
      uint32_t rule = CK_RULE_NONE;
      if (v < et->sp_min - g->env.deny_margin_c || v > et->sp_max + g->env.deny_margin_c) {
        snprintf(why, sizeof why, "setpoint %.1f far outside band %.1f..%.1f", (double)v,
                 (double)et->sp_min, (double)et->sp_max);
        return finish(g, d, VERDICT_DENY, CK_RULE_G1, NAN, why);
      }
      if (v < et->sp_min) { v = et->sp_min; verdict = VERDICT_CLAMP; rule = CK_RULE_G1; }
      if (v > et->sp_max) { v = et->sp_max; verdict = VERDICT_CLAMP; rule = CK_RULE_G1; }
      if (c->g4_active && v > c->setpoint)
        return finish(g, d, VERDICT_DENY, CK_RULE_G4, NAN,
                      "product above warning: setpoint raises blocked");
      const float step = v - c->setpoint;
      if (step > g->env.sp_max_step_c || step < -g->env.sp_max_step_c) {
        snprintf(why, sizeof why, "step %.1f exceeds +/-%.1f per change", (double)step,
                 (double)g->env.sp_max_step_c);
        return finish(g, d, VERDICT_DENY, CK_RULE_G2, NAN, why);
      }
      if (v != c->setpoint) {
        if (changes_last_hour(c, now) >= g->env.sp_changes_per_h)
          return finish(g, d, VERDICT_DENY, CK_RULE_G2, NAN, "too many setpoint changes this hour");
        if (c->sp_change_n < 8) c->sp_change_ns[c->sp_change_n++] = now;
        c->setpoint = v;
        c->lkg_candidate_since_ns = now;
      }
      act->cmd.type = CASE_CMD_SET_SETPOINT;
      act->cmd.setpoint_c = v;
      if (verdict == VERDICT_CLAMP) {
        snprintf(why, sizeof why, "clamped to band %.1f..%.1f", (double)et->sp_min, (double)et->sp_max);
        return finish(g, d, verdict, rule, v, why);
      }
      return finish(g, d, VERDICT_ALLOW, CK_RULE_NONE, v, "within envelope");
    }

    case CMD_START_DEFROST: {
      const bool manual = strcmp(cmd->origin, "schedule") != 0;
      if (cmd->value < 1.0f)
        return finish(g, d, VERDICT_DENY, CK_RULE_SCHEMA, NAN, "defrost duration must be >= 1 s");
      if (c->g4_active)
        return finish(g, d, VERDICT_DENY, CK_RULE_G4, NAN, "product above warning: no defrost");
      if (st[i].defrost_on)
        return finish(g, d, VERDICT_DENY, CK_RULE_G3, NAN, "case already defrosting");
      if (manual && c->has_manual_defrost &&
          now - c->last_manual_defrost_ns < CK_S(g->env.defrost_min_gap_s)) {
        snprintf(why, sizeof why, "min %u h between manual defrosts", g->env.defrost_min_gap_s / 3600);
        return finish(g, d, VERDICT_DENY, CK_RULE_G3, NAN, why);
      }
      if (defrosting(g, st) + 1 > max_concurrent(g)) {
        snprintf(why, sizeof why, "max %u cases defrosting at once", max_concurrent(g));
        return finish(g, d, VERDICT_DENY, CK_RULE_G3, NAN, why);
      }
      uint32_t dur = cmd->value > (float)et->defrost_max_s ? et->defrost_max_s
                                                           : (uint32_t)cmd->value;
      if (manual) { c->has_manual_defrost = true; c->last_manual_defrost_ns = now; }
      act->cmd.type = CASE_CMD_START_DEFROST;
      act->cmd.duration_s = dur;
      act->cmd.term_c = et->term_c;
      if (cmd->value > (float)et->defrost_max_s) {
        snprintf(why, sizeof why, "max defrost %u min", et->defrost_max_s / 60);
        return finish(g, d, VERDICT_CLAMP, CK_RULE_G3, (float)dur, why);
      }
      return finish(g, d, VERDICT_ALLOW, CK_RULE_NONE, (float)dur,
                    manual ? "manual defrost within envelope" : "scheduled defrost");
    }

    case CMD_STOP_DEFROST:
      act->cmd.type = CASE_CMD_STOP_DEFROST;
      return finish(g, d, VERDICT_ALLOW, CK_RULE_NONE, 0.0f, "stopping defrost is always safe");

    case CMD_SET_FANS: /* only "on" reaches here; "off" is dangerous */
      act->cmd.type = CASE_CMD_SET_FANS;
      act->cmd.fans = 1;
      return finish(g, d, VERDICT_ALLOW, CK_RULE_NONE, 1.0f, "fans on");

    default:
      return finish(g, d, VERDICT_DENY, CK_RULE_SCHEMA, NAN, "not a plant command");
  }
}

static void auto_cmd(ck_gov_t *g, ck_cmd_t *cmd, const char *origin, ck_cmd_type_t type,
                     uint16_t case_id, float value, uint64_t now) {
  memset(cmd, 0, sizeof *cmd);
  cmd->id = g->next_auto_id++;
  cmd->recv_ns = now;
  ck_strlcpy(cmd->origin, origin, sizeof cmd->origin);
  ck_strlcpy(cmd->user, origin, sizeof cmd->user);
  cmd->type = type; cmd->case_id = case_id; cmd->value = value;
}

int ck_gov_tick(ck_gov_t *g, const ck_case_state_t *st_in, uint64_t now,
                ck_gov_action_t *acts, ck_decision_t *decs, int max) {
  int k = 0;
  char why[96];
  /* local copy: actions decided in this tick update it (G3 concurrency) */
  ck_case_state_t st[CK_MAX_CASES];
  memcpy(st, st_in, sizeof(ck_case_state_t) * g->n);
  for (uint16_t i = 0; i < g->n; i++) {
    ck_gov_case_t *c = &g->c[i];
    const ck_env_type_t *et = &g->env.t[g->types[i]];
    const float prod = st[i].product_c;
    const uint16_t id = (uint16_t)(i + 1);

    /* G4: product-temperature override */
    if (!c->g4_active && prod > et->warn_c) {
      c->g4_active = true;
      if (k < max) {
        ck_decision_t *d = &decs[k];
        memset(d, 0, sizeof *d);
        auto_cmd(g, &d->cmd, "governor", CMD_SET_SETPOINT, id, c->lkg_setpoint, now);
        acts[k] = (ck_gov_action_t){.case_id = id};
        acts[k].cmd.type = CASE_CMD_SET_SETPOINT;
        acts[k].cmd.setpoint_c = c->lkg_setpoint;
        c->setpoint = c->lkg_setpoint;
        snprintf(why, sizeof why, "product %.1f > warn %.1f: force cooling, restore LKG %.1f",
                 (double)prod, (double)et->warn_c, (double)c->lkg_setpoint);
        finish(g, d, VERDICT_ALLOW, CK_RULE_G4, c->lkg_setpoint, why);
        k++;
      }
    } else if (c->g4_active && prod < et->warn_c - g->env.g4_clear_c) {
      c->g4_active = false;
      c->lkg_candidate_since_ns = now;
    }
    if (c->g4_active && st[i].defrost_on && k < max) {
      ck_decision_t *d = &decs[k];
      memset(d, 0, sizeof *d);
      auto_cmd(g, &d->cmd, "governor", CMD_STOP_DEFROST, id, 0.0f, now);
      acts[k] = (ck_gov_action_t){.case_id = id};
      acts[k].cmd.type = CASE_CMD_STOP_DEFROST;
      finish(g, d, VERDICT_ALLOW, CK_RULE_G4, 0.0f, "product above warning: end defrost");
      k++;
    }

    /* last-known-good: a setpoint that held for 1 h with product comfortably in range */
    if (!c->g4_active && prod <= et->warn_c - 2.0f &&
        now - c->lkg_candidate_since_ns >= CK_S(3600) && c->lkg_setpoint != c->setpoint) {
      c->lkg_setpoint = c->setpoint;
    }

    /* defrost schedule (origin "schedule"), subject to the same G3/G4 checks */
    if (now >= c->next_sched_ns && k < max) {
      ck_cmd_t cmd;
      auto_cmd(g, &cmd, "schedule", CMD_START_DEFROST, id, (float)et->sched_defrost_s, now);
      if (ck_gov_decide(g, &cmd, st, now, &decs[k], &acts[k])) {
        c->next_sched_ns = now + CK_S(et->sched_interval_s);
        st[i].defrost_on = 1;
      } else {
        c->next_sched_ns = now + CK_S(300); /* retry in 5 min; denial is recorded */
        acts[k].cmd.type = 0;               /* decision only, no plant action */
      }
      k++;
    }
  }
  return k;
}

bool ck_gov_apply_approved(ck_gov_t *g, const ck_cmd_t *cmd, uint64_t approval_id,
                           ck_decision_t *d) {
  memset(d, 0, sizeof *d);
  d->cmd = *cmd;
  if (approval_id == 0)
    return finish(g, d, VERDICT_DENY, CK_RULE_G5, NAN, "no keyconsole approval"), false;
  char why[96];
  switch (cmd->type) {
    case CMD_CHANGE_ENVELOPE: {
      if (cmd->case_id < 1 || cmd->case_id > g->n || !isfinite(cmd->value))
        return finish(g, d, VERDICT_DENY, CK_RULE_SCHEMA, NAN, "invalid envelope change"), false;
      ck_env_type_t *et = &g->env.t[g->types[cmd->case_id - 1]];
      if (cmd->value < et->sp_min)
        return finish(g, d, VERDICT_DENY, CK_RULE_SCHEMA, NAN, "sp_max below sp_min"), false;
      et->sp_max = cmd->value;
      g->env.version++;
      g->key_approvals++;
      snprintf(why, sizeof why, "key #%llu: %s sp_max=%.1f (env v%u)",
               (unsigned long long)approval_id, ck_case_type_name(g->types[cmd->case_id - 1]),
               (double)cmd->value, g->env.version);
      finish(g, d, VERDICT_ALLOW, CK_RULE_G5, cmd->value, why);
      return true;
    }
    case CMD_DISABLE_ALARMS:
      for (uint16_t i = 0; i < g->n; i++)
        if (cmd->case_id == 0 || cmd->case_id == i + 1) g->c[i].alarms_enabled = false;
      g->key_approvals++;
      finish(g, d, VERDICT_ALLOW, CK_RULE_G5, 0.0f, "alarms disabled with store key");
      return true;
    case CMD_SET_DEFROST_SCHEDULE: {
      if (cmd->case_id < 1 || cmd->case_id > g->n || !(cmd->value >= 1.0f && cmd->value <= 24.0f))
        return finish(g, d, VERDICT_DENY, CK_RULE_SCHEMA, NAN, "invalid schedule"), false;
      g->env.t[g->types[cmd->case_id - 1]].sched_interval_s = (uint32_t)(cmd->value * 3600.0f);
      g->env.version++;
      g->key_approvals++;
      finish(g, d, VERDICT_ALLOW, CK_RULE_G5, cmd->value, "defrost schedule changed with store key");
      return true;
    }
    default:
      g->key_approvals++;
      finish(g, d, VERDICT_ALLOW, CK_RULE_G5, cmd->value, "approved; no plant action modelled");
      return true;
  }
}

int ck_gov_status_text(const ck_gov_t *g, char *buf, size_t cap) {
  size_t o = 0;
#define EMIT(...)                                                        \
  do {                                                                   \
    int w_ = snprintf(buf + o, o < cap ? cap - o : 0, __VA_ARGS__);      \
    if (w_ > 0) o += (size_t)w_;                                         \
  } while (0)
  EMIT("envelope v%u\n", g->env.version);
  for (int t = CASE_CHILLED; t <= CASE_COLDROOM; t++) {
    const ck_env_type_t *e = &g->env.t[t];
    EMIT("  %-8s sp %.1f..%.1f warn %.1f limit %.1f defrost<=%umin\n",
         ck_case_type_name((ck_case_type_t)t), (double)e->sp_min, (double)e->sp_max,
         (double)e->warn_c, (double)e->limit_c, e->defrost_max_s / 60);
  }
  EMIT("verdicts ALLOW=%llu CLAMP=%llu DENY=%llu NEEDS_KEY=%llu\n",
       (unsigned long long)g->verdicts[VERDICT_ALLOW], (unsigned long long)g->verdicts[VERDICT_CLAMP],
       (unsigned long long)g->verdicts[VERDICT_DENY], (unsigned long long)g->verdicts[VERDICT_NEEDS_KEY]);
  EMIT("last %u decisions:\n", g->recent_n);
  for (uint32_t k = 0; k < g->recent_n; k++) {
    uint32_t idx = (g->recent_head + CK_GOV_RECENT - g->recent_n + k) % CK_GOV_RECENT;
    const ck_decision_t *d = &g->recent[idx];
    EMIT("  %s case=%u %s %s %s\n", ck_cmd_type_name(d->cmd.type), d->cmd.case_id,
         ck_verdict_name(d->verdict), ck_rule_name(d->rule_id), d->reason);
  }
#undef EMIT
  return (int)o;
}
