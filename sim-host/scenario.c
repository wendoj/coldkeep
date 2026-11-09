/* SPDX-License-Identifier: Apache-2.0
 *
 * Parser for the YAML subset used by scenarios/NAME.yaml:
 *
 *   name: demo-main            # comments allowed
 *   seed: 7
 *   cases: [chilled x4, frozen x2, coldroom x2]
 *   events:
 *     - { t: 600, do: attack, campaign: A1 }
 *
 * Anything else is rejected with a line number rather than silently ignored.
 */
#include "scenario.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *trim(char *s) {
  while (isspace((unsigned char)*s)) s++;
  char *e = s + strlen(s);
  while (e > s && isspace((unsigned char)e[-1])) *--e = '\0';
  return s;
}

#define FAIL(...) do { snprintf(err, errcap, __VA_ARGS__); return -1; } while (0)

static int parse_number(const char *v, double *out) {
  char *end;
  *out = strtod(v, &end);
  return (end != v && *trim(end) == '\0') ? 0 : -1;
}

static int parse_cases(char *v, scenario_t *sc, int line, char *err, size_t errcap) {
  size_t n = strlen(v);
  if (n < 2 || v[0] != '[' || v[n - 1] != ']') FAIL("line %d: cases must be [type xN, ...]", line);
  v[n - 1] = '\0';
  for (char *item = strtok(v + 1, ","); item; item = strtok(NULL, ",")) {
    char *it = trim(item);
    char type[32] = {0};
    int count = 1;
    char *x = strstr(it, " x");
    if (x) {
      *x = '\0';
      count = atoi(x + 2);
      if (count < 1) FAIL("line %d: bad case count", line);
    }
    ck_strlcpy(type, trim(it), sizeof type);
    ck_case_type_t t;
    if (strcmp(type, "chilled") == 0) t = CASE_CHILLED;
    else if (strcmp(type, "frozen") == 0) t = CASE_FROZEN;
    else if (strcmp(type, "coldroom") == 0) t = CASE_COLDROOM;
    else FAIL("line %d: unknown case type '%s'", line, type);
    for (int k = 0; k < count; k++) {
      if (sc->ncases >= CK_MAX_CASES) FAIL("line %d: more than %d cases", line, CK_MAX_CASES);
      sc->types[sc->ncases++] = t;
    }
  }
  if (sc->ncases == 0) FAIL("line %d: no cases", line);
  return 0;
}

static int parse_event(char *v, scenario_t *sc, int line, char *err, size_t errcap) {
  size_t n = strlen(v);
  if (n < 2 || v[0] != '{' || v[n - 1] != '}') FAIL("line %d: event must be { k: v, ... }", line);
  if (sc->nevents >= SC_MAX_EVENTS) FAIL("line %d: too many events", line);
  v[n - 1] = '\0';
  sc_event_t e = {0};
  int have_t = 0;
  char what[24] = {0};
  for (char *kv = strtok(v + 1, ","); kv; kv = strtok(NULL, ",")) {
    char *colon = strchr(kv, ':');
    if (!colon) FAIL("line %d: expected key: value in event", line);
    *colon = '\0';
    char *k = trim(kv), *val = trim(colon + 1);
    if (strcmp(k, "t") == 0) {
      double d;
      if (parse_number(val, &d) != 0 || d < 0) FAIL("line %d: bad t", line);
      e.t = (float)d; have_t = 1;
    } else if (strcmp(k, "do") == 0) ck_strlcpy(what, val, sizeof what);
    else if (strcmp(k, "campaign") == 0) ck_strlcpy(e.campaign, val, sizeof e.campaign);
    else if (strcmp(k, "proc") == 0) ck_strlcpy(e.proc, val, sizeof e.proc);
    else FAIL("line %d: unknown event key '%s'", line, k);
  }
  if (!have_t) FAIL("line %d: event without t", line);
  if (strcmp(what, "baseline") == 0) e.kind = EVT_BASELINE;
  else if (strcmp(what, "attack") == 0) {
    e.kind = EVT_ATTACK;
    if (strcmp(e.campaign, "A1") && strcmp(e.campaign, "A2") && strcmp(e.campaign, "A3") &&
        strcmp(e.campaign, "A4"))
      FAIL("line %d: unknown campaign '%s' (A1..A4)", line, e.campaign);
  } else if (strcmp(what, "kill") == 0) {
    e.kind = EVT_KILL;
    if (strcmp(e.proc, "governord") != 0) FAIL("line %d: only 'governord' can be killed on host", line);
  } else if (strcmp(what, "end") == 0) e.kind = EVT_END;
  else FAIL("line %d: unknown event '%s'", line, what);
  if (sc->nevents > 0 && e.t < sc->ev[sc->nevents - 1].t) FAIL("line %d: events must be in time order", line);
  sc->ev[sc->nevents++] = e;
  return 0;
}

int scenario_parse(const char *text, scenario_t *sc, char *err, size_t errcap) {
  memset(sc, 0, sizeof *sc);
  sc->seed = 1;
  sc->speed = 30.0f;
  sc->ambient_c = 20.0f;
  sc->start_hour = 9.0f;
  sc->governor_restart_s = -1.0f;
  ck_strlcpy(sc->name, "unnamed", sizeof sc->name);

  char *buf = strdup(text);
  if (!buf) FAIL("out of memory");
  int line = 0, in_events = 0, rc = 0;
  char *save = NULL;
  for (char *raw = strtok_r(buf, "\n", &save); raw; raw = strtok_r(NULL, "\n", &save)) {
    line++;
    char *hash = strchr(raw, '#');
    if (hash) *hash = '\0';
    const int indented = isspace((unsigned char)raw[0]);
    char *s = trim(raw);
    if (!*s) continue;
    if (s[0] == '-') {
      if (!in_events) { snprintf(err, errcap, "line %d: list item outside events", line); rc = -1; break; }
      if ((rc = parse_event(trim(s + 1), sc, line, err, errcap)) != 0) break;
      continue;
    }
    if (indented) { snprintf(err, errcap, "line %d: unexpected indentation", line); rc = -1; break; }
    in_events = 0;
    char *colon = strchr(s, ':');
    if (!colon) { snprintf(err, errcap, "line %d: expected key: value", line); rc = -1; break; }
    *colon = '\0';
    char *k = trim(s), *v = trim(colon + 1);
    double d;
    if (strcmp(k, "name") == 0) ck_strlcpy(sc->name, v, sizeof sc->name);
    else if (strcmp(k, "cases") == 0) { if ((rc = parse_cases(v, sc, line, err, errcap)) != 0) break; }
    else if (strcmp(k, "events") == 0) { if (*v) { snprintf(err, errcap, "line %d: events: takes a list", line); rc = -1; break; } in_events = 1; }
    else if (parse_number(v, &d) != 0) { snprintf(err, errcap, "line %d: '%s' needs a number", line, k); rc = -1; break; }
    else if (strcmp(k, "seed") == 0) sc->seed = (uint64_t)d;
    else if (strcmp(k, "speed") == 0) sc->speed = (float)d;
    else if (strcmp(k, "ambient_c") == 0) sc->ambient_c = (float)d;
    else if (strcmp(k, "start_hour") == 0) sc->start_hour = (float)d;
    else if (strcmp(k, "governor_restart_s") == 0) sc->governor_restart_s = (float)d;
    else if (strcmp(k, "expect_legacy_exceeds") == 0) sc->expect_legacy_exceeds = d != 0;
    else { snprintf(err, errcap, "line %d: unknown key '%s'", line, k); rc = -1; break; }
  }
  free(buf);
  if (rc) return rc;
  if (sc->ncases == 0) FAIL("no cases defined");
  if (sc->nevents == 0 || sc->ev[sc->nevents - 1].kind != EVT_END) FAIL("last event must be { do: end }");
  /* default: supervisor restarts within ~1 s wall time, i.e. `speed` sim seconds */
  if (sc->governor_restart_s < 0) sc->governor_restart_s = sc->speed * 1.0f;
  return 0;
}

int scenario_load(const char *path, scenario_t *sc, char *err, size_t errcap) {
  FILE *f = fopen(path, "rb");
  if (!f) FAIL("cannot open %s", path);
  static char text[65536];
  size_t n = fread(text, 1, sizeof text - 1, f);
  fclose(f);
  text[n] = '\0';
  return scenario_parse(text, sc, err, errcap);
}

float scenario_end_s(const scenario_t *sc) { return sc->ev[sc->nevents - 1].t; }
