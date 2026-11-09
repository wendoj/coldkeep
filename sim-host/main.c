/* SPDX-License-Identifier: Apache-2.0
 *
 * ck-sim — COLDKEEP host simulator. Runs one scenario on a legacy store, a
 * protected store, or both in lockstep (same seed, same events), driving the
 * attack campaigns A1–A4 through the in-process management interface.
 *
 * Output:
 *   stdout                         attack transcript + summary (human)
 *   <out>/<scenario>.<mode>.jsonl  meta, telemetry, decisions, attacks, summary
 *
 * Everything here is a HOST SIMULATION. A3 errnos come from a model of the
 * target's access control, not from QNX; they are labelled "source":"host-model".
 */
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "coldkeep.h"
#include "scenario.h"

/* ------------------------------------------------------------------ */
/* JSON helpers                                                        */
/* ------------------------------------------------------------------ */

static void jstr(FILE *f, const char *s) {
  fputc('"', f);
  for (; *s; s++) {
    unsigned char c = (unsigned char)*s;
    if (c == '"' || c == '\\') { fputc('\\', f); fputc(c, f); }
    else if (c < 0x20) fprintf(f, "\\u%04x", c);
    else fputc(c, f);
  }
  fputc('"', f);
}

static void jnum(FILE *f, double v) {
  if (isnan(v) || isinf(v)) fputs("null", f);
  else fprintf(f, "%.3f", v);
}

static double t_s(const ck_store_t *s) { return (double)s->now / 1e9; }

/* ------------------------------------------------------------------ */
/* One simulated store + its output                                    */
/* ------------------------------------------------------------------ */

typedef struct {
  ck_store_t s;
  FILE *out;
  const char *label;           /* "legacy" / "coldkeep" for the transcript */
  uint64_t flood_requests, flood_limited, flood_ok;
} sim_t;

static void write_decision(FILE *f, const ck_store_t *s, const ck_decision_t *d) {
  fprintf(f, "{\"type\":\"decision\",\"t\":"); jnum(f, t_s(s));
  fprintf(f, ",\"id\":%llu,\"origin\":", (unsigned long long)d->cmd.id); jstr(f, d->cmd.origin);
  fprintf(f, ",\"user\":"); jstr(f, d->cmd.user);
  fprintf(f, ",\"cmd\":\"%s\",\"case\":%u,\"value\":", ck_cmd_type_name(d->cmd.type), d->cmd.case_id);
  jnum(f, d->cmd.value);
  fprintf(f, ",\"verdict\":\"%s\",\"rule\":\"%s\",\"applied\":", ck_verdict_name(d->verdict), ck_rule_name(d->rule_id));
  jnum(f, d->applied_value);
  fprintf(f, ",\"reason\":"); jstr(f, d->reason);
  fputs("}\n", f);
}

static void sink(ck_store_t *s, ck_ev_kind_t kind, const void *p, void *ctx) {
  sim_t *sim = ctx;
  FILE *f = sim->out;
  if (!f) return;
  switch (kind) {
    case EV_DECISION:
      write_decision(f, s, p);
      break;
    case EV_ALARM: {
      const ck_ev_alarm_t *a = p;
      fprintf(f, "{\"type\":\"alarm\",\"t\":"); jnum(f, t_s(s));
      fprintf(f, ",\"case\":%u,\"product\":", a->case_id); jnum(f, a->product_c);
      fprintf(f, ",\"limit\":"); jnum(f, a->limit_c);
      fprintf(f, ",\"silenced\":%s}\n", a->silenced ? "true" : "false");
      break;
    }
    case EV_G6: {
      const ck_ev_g6_t *g = p;
      fprintf(f, "{\"type\":\"g6\",\"t\":"); jnum(f, t_s(s));
      fprintf(f, ",\"case\":%u,\"hold\":%s}\n", g->case_id, g->hold ? "true" : "false");
      break;
    }
    case EV_PROC: {
      const ck_ev_proc_t *e = p;
      fprintf(f, "{\"type\":\"proc\",\"t\":"); jnum(f, t_s(s));
      fprintf(f, ",\"proc\":"); jstr(f, e->proc);
      fprintf(f, ",\"what\":"); jstr(f, e->what); fputs("}\n", f);
      break;
    }
    case EV_NOTE:
      fprintf(f, "{\"type\":\"note\",\"t\":"); jnum(f, t_s(s));
      fprintf(f, ",\"text\":"); jstr(f, p); fputs("}\n", f);
      break;
    case EV_COMPLIANCE:
      fprintf(f, "{\"type\":\"compliance\",\"t\":"); jnum(f, t_s(s));
      fputs(",\"cases\":[", f);
      for (uint16_t i = 0; i < s->n; i++) {
        fprintf(f, "%s{\"id\":%u,\"air\":", i ? "," : "", i + 1); jnum(f, s->st[i].air_c);
        fputs(",\"prod\":", f); jnum(f, s->st[i].product_c); fputc('}', f);
      }
      fputs("]}\n", f);
      break;
    case EV_TELEMETRY:
      break;
  }
}

static void write_telemetry(sim_t *sim) {
  FILE *f = sim->out;
  const ck_store_t *s = &sim->s;
  if (!f) return;
  fprintf(f, "{\"type\":\"telemetry\",\"t\":"); jnum(f, t_s(s));
  fputs(",\"cases\":[", f);
  for (uint16_t i = 0; i < s->n; i++) {
    const ck_case_state_t *c = &s->st[i];
    fprintf(f, "%s{\"id\":%u,\"air\":", i ? "," : "", i + 1); jnum(f, c->air_c);
    fputs(",\"prod\":", f); jnum(f, c->product_c);
    fputs(",\"evap\":", f); jnum(f, c->evap_c);
    fputs(",\"sp\":", f); jnum(f, c->setpoint_c);
    fprintf(f, ",\"comp\":%u,\"heat\":%u,\"defrost\":%u,\"fans\":%u,\"door\":%u,\"hold\":%u}",
            c->compressor_on, c->heater_on, c->defrost_on, c->fans_on, c->door_open,
            s->ctl[i].hold ? 1u : 0u);
  }
  fputs("]}\n", f);
}

/* ------------------------------------------------------------------ */
/* Campaigns -> timed actions                                          */
/* ------------------------------------------------------------------ */

typedef enum { ACT_LOGIN = 1, ACT_MGMT, ACT_MGMT_SP_PLUS, ACT_FLOOD_ON, ACT_FLOOD_OFF,
               ACT_COMPROMISE, ACT_KILL, ACT_NOTE } act_kind_t;

typedef struct {
  uint64_t t_ns; act_kind_t k; char campaign[8];
  uint32_t session; ck_cmd_type_t type; uint16_t case_id; float value;
  char what[48];
} action_t;

#define MAX_ACTIONS 512
static action_t acts[MAX_ACTIONS];
static int nacts;
static float g_restart_s; /* sim seconds until the supervisor restarts governord */

static action_t *add(uint64_t t_ns, act_kind_t k, const char *campaign) {
  if (nacts >= MAX_ACTIONS) { fprintf(stderr, "too many actions\n"); exit(2); }
  action_t *a = &acts[nacts++];
  memset(a, 0, sizeof *a);
  a->t_ns = t_ns; a->k = k;
  ck_strlcpy(a->campaign, campaign, sizeof a->campaign);
  return a;
}

/* All campaigns pace requests at 1/s so they are judged by the governor,
 * not just by the broker's rate limiter (A2 is the exception, on purpose). */
static void expand(const scenario_t *sc) {
  const uint16_t n = sc->ncases;
  for (int e = 0; e < sc->nevents; e++) {
    const sc_event_t *ev = &sc->ev[e];
    uint64_t t = (uint64_t)(ev->t * 1000.0f) * 1000000ULL;
    const uint64_t sec = CK_NS_PER_S;
    if (ev->kind == EVT_KILL) { action_t *a = add(t, ACT_KILL, "A4"); ck_strlcpy(a->what, ev->proc, sizeof a->what); continue; }
    if (ev->kind != EVT_ATTACK) continue;
    const char *c = ev->campaign;
    if (strcmp(c, "A1") == 0) {
      /* credentialed takeover: admin/admin, 24 h defrost everywhere, setpoints +25, alarms off */
      action_t *a = add(t, ACT_LOGIN, c); a->session = 1;
      for (uint16_t i = 1; i <= n; i++) {
        a = add(t += sec, ACT_MGMT, c); a->session = 1; a->type = CMD_START_DEFROST; a->case_id = i; a->value = 86400;
      }
      for (uint16_t i = 1; i <= n; i++) {
        a = add(t += sec, ACT_MGMT_SP_PLUS, c); a->session = 1; a->type = CMD_SET_SETPOINT; a->case_id = i; a->value = 25;
      }
      a = add(t += sec, ACT_MGMT, c); a->session = 1; a->type = CMD_DISABLE_ALARMS; a->case_id = 0;
    } else if (strcmp(c, "A2") == 0) {
      /* API flood: 10k req/s for 5 sim-minutes; mid-flood, the store manager (own session) reads status */
      action_t *a = add(t, ACT_LOGIN, c); a->session = 2;
      a = add(t, ACT_LOGIN, c); a->session = 3;
      add(t, ACT_FLOOD_ON, c);
      a = add(t + 150 * sec, ACT_MGMT, c); a->session = 3; a->type = CMD_STATUS; a->case_id = 0;
      add(t + 300 * sec, ACT_FLOOD_OFF, c);
    } else if (strcmp(c, "A3") == 0) {
      /* attacker has code execution inside mgmtd (simulated switch, no real exploit) */
      static const char *steps[] = {"devctl_case", "connect_governor", "regain_abilities", "spawn", "tamper_audit"};
      for (int i = 0; i < 5; i++) {
        action_t *a = add(t + (uint64_t)i * sec, ACT_COMPROMISE, c);
        ck_strlcpy(a->what, steps[i], sizeof a->what);
      }
    } else if (strcmp(c, "A4") == 0) {
      action_t *a = add(t, ACT_KILL, c); ck_strlcpy(a->what, "governord", sizeof a->what);
      a = add(t, ACT_LOGIN, c); a->session = 1;
      a = add(t + 2 * sec, ACT_MGMT, c); a->session = 1; a->type = CMD_START_DEFROST; a->case_id = 1; a->value = 86400;
    }
  }
  /* stable sort by time (insertion sort keeps campaign order on ties) */
  for (int i = 1; i < nacts; i++) {
    action_t x = acts[i]; int j = i - 1;
    while (j >= 0 && acts[j].t_ns > x.t_ns) { acts[j + 1] = acts[j]; j--; }
    acts[j + 1] = x;
  }
}

static const char *errno_name(int e) {
  switch (e) {
    case 0: return "OK";
    case EPERM: return "EPERM";
    case EACCES: return "EACCES";
    case EINVAL: return "EINVAL";
    default: return "ERR";
  }
}

static void describe(char *buf, size_t cap, const sim_t *sim, const ck_decision_t *d) {
  if (d->verdict == VERDICT_ALLOW && d->rule_id == CK_RULE_LEGACY) snprintf(buf, cap, "%s: APPLIED", sim->label);
  else if (d->rule_id == CK_RULE_NONE) snprintf(buf, cap, "%s: %s", sim->label, ck_verdict_name(d->verdict));
  else snprintf(buf, cap, "%s: %s (%s %s)", sim->label, ck_verdict_name(d->verdict), ck_rule_name(d->rule_id), d->reason);
}

static void run_action(const action_t *a, sim_t *sims, int nsims, int quiet, int *flood) {
  char line[1024], part[256];
  size_t o = 0;
  line[0] = '\0';
#define APPEND(...) do { int w_ = snprintf(line + o, sizeof line - o, __VA_ARGS__); if (w_ > 0) o += (size_t)w_; if (o >= sizeof line) o = sizeof line - 1; } while (0)
  const double t = (double)a->t_ns / 1e9;
  switch (a->k) {
    case ACT_LOGIN:
      for (int i = 0; i < nsims; i++) ck_store_login(&sims[i].s, a->session, "admin", "admin");
      if (a->session == 1 || a->session == 2)
        APPEND("[%s] t=%.0f login admin/admin (session %u) -> OK on every store", a->campaign, t, a->session);
      break;
    case ACT_MGMT:
    case ACT_MGMT_SP_PLUS: {
      if (a->type == CMD_STATUS) {
        APPEND("[%s] t=%.0f status (store manager, session %u) ->", a->campaign, t, a->session);
        for (int i = 0; i < nsims; i++) {
          ck_decision_t d;
          ck_verdict_t v = ck_store_mgmt(&sims[i].s, a->session, CMD_STATUS, 0, 0, &d);
          APPEND("%s %s: %s", i ? " |" : "", sims[i].label, v == VERDICT_ALLOW ? "served" : ck_rule_name(d.rule_id));
        }
        break;
      }
      APPEND("[%s] t=%.0f %s case=%u", a->campaign, t, ck_cmd_type_name(a->type), a->case_id);
      if (a->k == ACT_MGMT_SP_PLUS) APPEND(" value=setpoint%+.0f", (double)a->value);
      else if (a->type == CMD_START_DEFROST) APPEND(" seconds=%.0f", (double)a->value);
      else if (a->type == CMD_SET_SETPOINT) APPEND(" value=%.1f", (double)a->value);
      APPEND(" ->");
      for (int i = 0; i < nsims; i++) {
        ck_store_t *s = &sims[i].s;
        float v = a->value;
        if (a->k == ACT_MGMT_SP_PLUS) v = s->ctl[a->case_id - 1].setpoint_c + a->value;
        ck_decision_t d;
        ck_store_mgmt(s, a->session, a->type, a->case_id, v, &d);
        describe(part, sizeof part, &sims[i], &d);
        APPEND("%s %s", i ? " |" : "", part);
      }
      break;
    }
    case ACT_FLOOD_ON:
      *flood = 1;
      APPEND("[%s] t=%.0f flood begins: 10000 requests/s for 300 s (session 2)", a->campaign, t);
      break;
    case ACT_FLOOD_OFF:
      *flood = 0;
      APPEND("[%s] t=%.0f flood ends ->", a->campaign, t);
      for (int i = 0; i < nsims; i++) {
        sim_t *sim = &sims[i];
        APPEND("%s %s: %llu requests, %llu rate-limited", i ? " |" : "", sim->label,
               (unsigned long long)sim->flood_requests, (unsigned long long)sim->flood_limited);
        if (sim->out) {
          fprintf(sim->out, "{\"type\":\"attack\",\"t\":"); jnum(sim->out, t);
          fprintf(sim->out, ",\"campaign\":\"A2\",\"action\":\"flood\",\"requests\":%llu,\"rate_limited\":%llu,"
                  "\"served\":%llu,\"jitter\":\"n/a on host (measure on QNX)\",\"source\":\"host-sim\"}\n",
                  (unsigned long long)sim->flood_requests, (unsigned long long)sim->flood_limited,
                  (unsigned long long)sim->flood_ok);
        }
      }
      break;
    case ACT_COMPROMISE:
      APPEND("[%s] t=%.0f (inside mgmtd) %s ->", a->campaign, t, a->what);
      for (int i = 0; i < nsims; i++) {
        sim_t *sim = &sims[i];
        int e = ck_store_compromised_attempt(&sim->s, a->what);
        APPEND("%s %s: %s", i ? " |" : "", sim->label, e == 0 ? "SUCCEEDED" : errno_name(e));
        if (sim->out) {
          fprintf(sim->out, "{\"type\":\"attack\",\"t\":"); jnum(sim->out, t);
          fprintf(sim->out, ",\"campaign\":\"A3\",\"action\":\"%s\",\"result\":\"%s\",\"source\":\"host-model\"}\n",
                  a->what, errno_name(e));
        }
      }
      break;
    case ACT_KILL:
      APPEND("[%s] t=%.0f kill %s ->", a->campaign, t, a->what);
      for (int i = 0; i < nsims; i++) {
        sim_t *sim = &sims[i];
        ck_store_kill_governor(&sim->s, g_restart_s);
        APPEND("%s %s: %s", i ? " |" : "", sim->label,
               sim->s.mode == MODE_LEGACY ? "n/a (no governor)" : "killed; G6 hold until restart");
      }
      break;
    case ACT_NOTE:
      break;
  }
  if (!quiet && o) puts(line);
#undef APPEND
}

/* ------------------------------------------------------------------ */
/* Summary                                                             */
/* ------------------------------------------------------------------ */

static void write_summary(sim_t *sim, const scenario_t *sc) {
  const ck_store_t *s = &sim->s;
  const ck_metrics_t *m = &s->m;
  FILE *f = sim->out;
  if (!f) return;
  fprintf(f, "{\"type\":\"summary\",\"t\":"); jnum(f, t_s(s));
  fprintf(f, ",\"mode\":\"%s\",\"minutes_above_total\":", ck_mode_name(s->mode));
  jnum(f, ck_metrics_total_minutes_above(m));
  fputs(",\"food_at_risk_kg\":", f); jnum(f, ck_metrics_food_at_risk_kg(m));
  fprintf(f, ",\"applied\":%llu,\"blocked\":%llu", (unsigned long long)m->applied, (unsigned long long)m->blocked);
  fprintf(f, ",\"verdicts\":{\"ALLOW\":%llu,\"CLAMP\":%llu,\"DENY\":%llu,\"NEEDS_KEY\":%llu}",
          (unsigned long long)m->verdicts[VERDICT_ALLOW], (unsigned long long)m->verdicts[VERDICT_CLAMP],
          (unsigned long long)m->verdicts[VERDICT_DENY], (unsigned long long)m->verdicts[VERDICT_NEEDS_KEY]);
  fprintf(f, ",\"rate_limited\":%llu,\"key_pending\":%u,\"envelope_version\":%u,\"alarms_enabled\":%s",
          (unsigned long long)s->broker.rate_limited, ck_keyq_pending(&s->keyq),
          s->mode == MODE_PROTECTED ? s->gov.env.version : 0u,
          (s->mode == MODE_LEGACY ? s->alarms_enabled : s->gov.c[0].alarms_enabled) ? "true" : "false");
  fputs(",\"cases\":[", f);
  for (uint16_t i = 0; i < s->n; i++) {
    const ck_case_metrics_t *c = &m->c[i];
    fprintf(f, "%s{\"id\":%u,\"type\":\"%s\",\"limit\":", i ? "," : "", i + 1, ck_case_type_name(sc->types[i]));
    jnum(f, c->limit_c);
    fputs(",\"peak_product\":", f); jnum(f, c->peak_product_c);
    fputs(",\"peak_air\":", f); jnum(f, c->peak_air_c);
    fputs(",\"minutes_above\":", f); jnum(f, c->s_above / 60.0);
    fprintf(f, ",\"longest_defrost_s\":%llu}", (unsigned long long)c->defrost_s_max);
  }
  fputs("]}\n", f);
}

static void print_summary(const sim_t *sims, int nsims, const scenario_t *sc) {
  printf("\nSummary — scenario %s, seed %llu, %.0f sim-s (HOST SIMULATION, not a QNX measurement)\n",
         sc->name, (unsigned long long)sims[0].s.seed, (double)scenario_end_s(sc));
  printf("  %-10s %14s %16s %9s %9s %14s\n", "store", "min>limit", "peak product", "applied", "blocked", "food at risk");
  for (int i = 0; i < nsims; i++) {
    const ck_metrics_t *m = &sims[i].s.m;
    float peak = -1e9f; int pi = 0;
    for (uint16_t c = 0; c < m->n; c++) {
      float over = m->c[c].peak_product_c - m->c[c].limit_c;
      if (over > peak) { peak = over; pi = c; }
    }
    char pk[48];
    snprintf(pk, sizeof pk, "%.1f°C (case %d)", (double)m->c[pi].peak_product_c, pi + 1);
    printf("  %-10s %14.1f %17s %9llu %9llu %11.0f kg\n", sims[i].label, ck_metrics_total_minutes_above(m), pk,
           (unsigned long long)m->applied, (unsigned long long)m->blocked, (double)ck_metrics_food_at_risk_kg(m));
  }
}

static int check(const sim_t *sims, int nsims, const scenario_t *sc) {
  int bad = 0;
  for (int i = 0; i < nsims; i++) {
    const ck_store_t *s = &sims[i].s;
    const double mins = ck_metrics_total_minutes_above(&s->m);
    if (s->mode == MODE_PROTECTED) {
      if (mins > 0) { fprintf(stderr, "CHECK FAILED: protected store above limit for %.1f min\n", mins); bad = 1; }
      for (uint16_t c = 0; c < s->n; c++) {
        const uint32_t mx = s->env.t[s->st[c].type].defrost_max_s;
        if (s->m.c[c].defrost_s_max > mx) { fprintf(stderr, "CHECK FAILED: case %u defrost %llu s > %u s\n", c + 1,
                                                    (unsigned long long)s->m.c[c].defrost_s_max, mx); bad = 1; }
      }
      if (s->gov.env.version != 1) { fprintf(stderr, "CHECK FAILED: envelope changed without key\n"); bad = 1; }
    } else if (sc->expect_legacy_exceeds && mins <= 0) {
      fprintf(stderr, "CHECK FAILED: legacy store expected to exceed limits but did not\n"); bad = 1;
    }
  }
  if (!bad) printf("check: OK (protected never exceeded limits%s)\n",
                   sc->expect_legacy_exceeds ? "; legacy exceeded as expected" : "");
  return bad;
}

/* ------------------------------------------------------------------ */

static void mkdir_p(const char *path) {
  char tmp[512];
  ck_strlcpy(tmp, path, sizeof tmp);
  for (char *p = tmp + 1; *p; p++)
    if (*p == '/') { *p = '\0'; mkdir(tmp, 0755); *p = '/'; }
  mkdir(tmp, 0755);
}

static void usage(void) {
  fprintf(stderr,
          "usage: ck-sim --scenario FILE [--mode legacy|protected|both] [--seed N]\n"
          "              [--out DIR] [--telemetry-s S] [--check] [--quiet]\n");
}

int main(int argc, char **argv) {
  const char *scen_path = NULL, *mode = "both", *outdir = "out";
  int do_check = 0, quiet = 0, seed_override = 0;
  uint64_t seed = 0;
  double telem_s = 30.0;
  for (int i = 1; i < argc; i++) {
    const char *a = argv[i];
    const char *v = i + 1 < argc ? argv[i + 1] : NULL;
    if (!strcmp(a, "--scenario") && v) { scen_path = v; i++; }
    else if (!strcmp(a, "--mode") && v) { mode = v; i++; }
    else if (!strcmp(a, "--seed") && v) { seed = strtoull(v, NULL, 10); seed_override = 1; i++; }
    else if (!strcmp(a, "--out") && v) { outdir = v; i++; }
    else if (!strcmp(a, "--telemetry-s") && v) { telem_s = atof(v); i++; }
    else if (!strcmp(a, "--check")) do_check = 1;
    else if (!strcmp(a, "--quiet")) quiet = 1;
    else { usage(); return 2; }
  }
  if (!scen_path || telem_s <= 0) { usage(); return 2; }

  static scenario_t sc;
  char err[256];
  if (scenario_load(scen_path, &sc, err, sizeof err) != 0) { fprintf(stderr, "%s: %s\n", scen_path, err); return 2; }
  if (seed_override) sc.seed = seed;

  ck_mode_t modes[2]; int nsims = 0;
  if (!strcmp(mode, "legacy") || !strcmp(mode, "both")) modes[nsims++] = MODE_LEGACY;
  if (!strcmp(mode, "protected") || !strcmp(mode, "both")) modes[nsims++] = MODE_PROTECTED;
  if (!nsims) { usage(); return 2; }

  mkdir_p(outdir);
  static sim_t sims[2];
  for (int i = 0; i < nsims; i++) {
    sim_t *sim = &sims[i];
    memset(sim, 0, sizeof *sim);
    sim->label = modes[i] == MODE_LEGACY ? "legacy" : "coldkeep";
    char path[512];
    snprintf(path, sizeof path, "%s/%s.%s.jsonl", outdir, sc.name, ck_mode_name(modes[i]));
    sim->out = fopen(path, "w");
    if (!sim->out) { fprintf(stderr, "cannot write %s\n", path); return 2; }
    ck_store_init(&sim->s, modes[i], sc.seed, sc.ncases, sc.types, sc.ambient_c, 0.1f);
    sim->s.start_hour = sc.start_hour;
    ck_store_set_sink(&sim->s, sink, sim);
    FILE *f = sim->out;
    fprintf(f, "{\"type\":\"meta\",\"scenario\":"); jstr(f, sc.name);
    fprintf(f, ",\"mode\":\"%s\",\"seed\":%llu,\"speed\":", ck_mode_name(modes[i]), (unsigned long long)sc.seed);
    jnum(f, sc.speed);
    fputs(",\"dt\":0.100,\"source\":\"sim-host\",\"note\":\"host simulator run, not a QNX measurement\",\"cases\":[", f);
    for (uint16_t c = 0; c < sc.ncases; c++)
      fprintf(f, "%s{\"id\":%u,\"type\":\"%s\",\"limit\":%.1f}", c ? "," : "", c + 1,
              ck_case_type_name(sc.types[c]), (double)ck_thermal_defaults(sc.types[c])->limit);
    fputs("]}\n", f);
    if (!quiet) printf("%s store -> %s\n", sim->label, path);
  }

  g_restart_s = sc.governor_restart_s;
  expand(&sc);
  const uint64_t end_ns = (uint64_t)(scenario_end_s(&sc) * 1000.0f) * 1000000ULL;
  const uint64_t telem_ns = (uint64_t)(telem_s * 1000.0) * 1000000ULL;
  uint64_t next_telem = 0;
  int ai = 0, flood = 0;
  const uint32_t flood_per_step = 1000; /* 10k/s at 0.1 s steps */

  while (sims[0].s.now < end_ns) {
    const uint64_t now = sims[0].s.now;
    while (ai < nacts && acts[ai].t_ns <= now) {
      run_action(&acts[ai], sims, nsims, quiet, &flood);
      ai++;
    }
    if (flood) {
      for (int i = 0; i < nsims; i++) {
        sim_t *sim = &sims[i];
        for (uint32_t k = 0; k < flood_per_step; k++) {
          ck_decision_t d;
          ck_verdict_t v = ck_store_mgmt(&sim->s, 2, CMD_STATUS, 0, 0, &d);
          sim->flood_requests++;
          if (v == VERDICT_DENY && d.rule_id == CK_RULE_RATE) sim->flood_limited++;
          else sim->flood_ok++;
        }
      }
    }
    if (now >= next_telem) {
      for (int i = 0; i < nsims; i++) write_telemetry(&sims[i]);
      next_telem = now + telem_ns;
    }
    for (int i = 0; i < nsims; i++) ck_store_step(&sims[i].s);
  }
  for (int i = 0; i < nsims; i++) write_telemetry(&sims[i]);

  for (int i = 0; i < nsims; i++) { write_summary(&sims[i], &sc); fclose(sims[i].out); sims[i].out = NULL; }
  if (!quiet) print_summary(sims, nsims, &sc);
  return do_check ? check(sims, nsims, &sc) : 0;
}
