/* SPDX-License-Identifier: Apache-2.0 — scenario files (scenarios/NAME.yaml, a small YAML subset) */
#ifndef CK_SCENARIO_H
#define CK_SCENARIO_H
#include "coldkeep.h"

#define SC_MAX_EVENTS 64

typedef enum { EVT_BASELINE = 1, EVT_ATTACK, EVT_KILL, EVT_END } sc_event_kind_t;

typedef struct {
  float t;                 /* sim seconds */
  sc_event_kind_t kind;
  char campaign[8];        /* attack: A1..A4 */
  char proc[24];           /* kill: governord */
} sc_event_t;

typedef struct {
  char name[64];
  uint64_t seed;
  float speed;             /* informational on host (target maps sim to wall time) */
  float ambient_c;
  float start_hour;
  float governor_restart_s;/* sim seconds until the supervisor restarts governord */
  int expect_legacy_exceeds;
  uint16_t ncases;
  ck_case_type_t types[CK_MAX_CASES];
  int nevents;
  sc_event_t ev[SC_MAX_EVENTS];
} scenario_t;

/* Returns 0 on success; on error writes a message (with line number) into err. */
int scenario_load(const char *path, scenario_t *sc, char *err, size_t errcap);
int scenario_parse(const char *text, scenario_t *sc, char *err, size_t errcap);
float scenario_end_s(const scenario_t *sc);

#endif
