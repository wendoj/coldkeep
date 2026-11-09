/*
 * COLDKEEP core — portable C11, no QNX headers.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Data model plus the APIs of every core module. Target processes
 * (target/) and the host simulator (sim-host/) are thin wrappers around this.
 *
 * Time: all *_ns values are SIM-time nanoseconds since scenario start unless
 * stated otherwise. One sim tick is CK_TICK_NS (100 ms sim).
 */
#ifndef COLDKEEP_H
#define COLDKEEP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CK_NS_PER_S   1000000000ULL
#define CK_S(x)       ((uint64_t)(x) * CK_NS_PER_S)
#define CK_TICK_NS    (CK_NS_PER_S / 10u)
#define CK_MAX_CASES  16
#define CK_NTYPES     4 /* index 1..3 used */

/* ------------------------------------------------------------------ */
/* Data model                                                          */
/* ------------------------------------------------------------------ */

typedef enum { CASE_CHILLED = 1, CASE_FROZEN = 2, CASE_COLDROOM = 3 } ck_case_type_t;

typedef struct {               /* live state per case */
  uint16_t id; ck_case_type_t type;
  float air_c, product_c, evap_c, ambient_c;
  float setpoint_c;
  uint8_t compressor_on, defrost_on, fans_on, door_open;
  uint64_t defrost_started_ns, last_defrost_end_ns;
  /* actuator detail and frost state */
  uint8_t heater_on;           /* defrost heater (defrost_on = whole cycle) */
  float ice_j;                 /* frost on the evaporator, as latent heat (J) */
} ck_case_state_t;

typedef enum {
  CMD_SET_SETPOINT = 1, CMD_START_DEFROST, CMD_STOP_DEFROST, CMD_SET_FANS,
  CMD_DISABLE_ALARMS, CMD_SET_DEFROST_SCHEDULE, CMD_CHANGE_ENVELOPE, CMD_RAW_WRITE,
  CMD_STATUS                   /* extension: read-only status request */
} ck_cmd_type_t;
#define CK_CMD_MAX CMD_STATUS

typedef enum { VERDICT_ALLOW = 1, VERDICT_CLAMP, VERDICT_DENY, VERDICT_NEEDS_KEY } ck_verdict_t;

typedef struct {
  uint64_t id; uint64_t recv_ns;
  char     origin[16];         /* "mgmt", "keyconsole", "schedule", "governor" */
  char     user[32];           /* as claimed by mgmtd (untrusted) */
  ck_cmd_type_t type; uint16_t case_id;   /* 1..n; 0 = store-wide */
  float    value;              /* setpoint °C, duration s, etc. */
} ck_cmd_t;

typedef struct {
  ck_cmd_t cmd; ck_verdict_t verdict; uint32_t rule_id;   /* G1..G6, or CK_RULE_* */
  float applied_value; char reason[96];
} ck_decision_t;

/* rule_id values. 1..6 are governor rules G1..G6. */
enum {
  CK_RULE_NONE = 0,
  CK_RULE_G1 = 1, CK_RULE_G2, CK_RULE_G3, CK_RULE_G4, CK_RULE_G5, CK_RULE_G6,
  CK_RULE_SCHEMA = 10,         /* broker: malformed / out-of-schema */
  CK_RULE_RATE = 11,           /* broker: per-session rate limit */
  CK_RULE_UNAVAILABLE = 12,    /* governor not reachable */
  CK_RULE_AUTH = 13,           /* not logged in */
  CK_RULE_LEGACY = 20          /* legacy mode: applied directly, no checks */
};

const char *ck_case_type_name(ck_case_type_t t);   /* "chilled" ... */
const char *ck_cmd_type_name(ck_cmd_type_t t);     /* "set_setpoint" ... */
const char *ck_verdict_name(ck_verdict_t v);       /* "ALLOW" ... */
const char *ck_rule_name(uint32_t rule_id);        /* "G3", "RATE" ... */
void ck_strlcpy(char *dst, const char *src, size_t cap);

/* ------------------------------------------------------------------ */
/* prng.c — seeded, platform-independent randomness                    */
/* ------------------------------------------------------------------ */

typedef struct { uint64_t s; } ck_rng_t;
void     ck_rng_seed(ck_rng_t *r, uint64_t seed);
uint64_t ck_rng_next(ck_rng_t *r);                     /* splitmix64 */
float    ck_rng_uniform(ck_rng_t *r);                  /* [0,1), 24-bit exact */
float    ck_rng_range(ck_rng_t *r, float lo, float hi);
uint32_t ck_rng_below(ck_rng_t *r, uint32_t n);        /* [0,n) */
float    ck_rng_gauss(ck_rng_t *r);                    /* ~N(0,1), Irwin–Hall */
uint64_t ck_rng_derive(uint64_t seed, uint64_t stream); /* independent sub-seed */

/* ------------------------------------------------------------------ */
/* thermal.c — lumped, illustrative plant model                        */
/* ------------------------------------------------------------------ */

typedef struct {
  float c_air;        /* J/K   lumped air + fixtures */
  float ua_amb;       /* W/K   air <-> ambient */
  float ua_door;      /* W/K   extra while door open / shopper reach-in */
  float ua_ea_fan;    /* W/K   air <-> evaporator, fans on (clean coil) */
  float ua_ea_nat;    /* W/K   air <-> evaporator, fans off */
  float c_evap;       /* J/K   evaporator coil */
  float ua_ref;       /* W/K   coil <-> refrigerant when compressor on */
  float t_sat;        /* °C    refrigerant saturation temperature */
  float q_heater;     /* W     defrost heater */
  float k_prod;       /* 1/s   product follows air */
  float ice_grow;     /* J/s   frost growth while cooling below 0 °C */
  float ice_ref;      /* J     frost at which coil coupling halves */
  float ice_init;     /* J     frost at scenario start */
  float door_rate_h;  /* door/reach-in events per hour (store open) */
  float door_min_s, door_max_s;
  float hyst;         /* °C    thermostat half-band */
  float setpoint;     /* °C    default setpoint */
  float limit;        /* °C    food-safe product limit (metrics/invariants) */
  float food_kg;      /* illustrative product mass for "food at risk" */
} ck_thermal_params_t;

const ck_thermal_params_t *ck_thermal_defaults(ck_case_type_t t);
void  ck_thermal_init(ck_case_state_t *s, uint16_t id, ck_case_type_t t, float ambient_c);
/* Advance one step of dt seconds using the actuator fields in *s. */
void  ck_thermal_step(ck_case_state_t *s, const ck_thermal_params_t *p, float dt_s);
float ck_thermal_cool_eff(const ck_case_state_t *s, const ck_thermal_params_t *p);

/* ------------------------------------------------------------------ */
/* control.c — case controller logic (casectl)                         */
/* ------------------------------------------------------------------ */

typedef enum { DF_IDLE = 0, DF_PUMPDOWN, DF_HEAT, DF_DRIP, DF_RECOVER } ck_df_phase_t;

/* Command payload governord -> casectl (the devctl body on target). */
typedef enum {
  CASE_CMD_SET_SETPOINT = 1, CASE_CMD_START_DEFROST, CASE_CMD_STOP_DEFROST,
  CASE_CMD_SET_FANS, CASE_CMD_RAW_ACTUATORS, CASE_CMD_HEARTBEAT
} ck_case_cmd_type_t;

#define CK_TERM_NONE 1000.0f   /* term_c value meaning "forced: no termination" */

typedef struct {
  ck_case_cmd_type_t type;
  float    setpoint_c;
  uint32_t duration_s;         /* START_DEFROST: total cycle (pumpdown+heat+drip) */
  float    term_c;             /* START_DEFROST: coil termination temp, or CK_TERM_NONE */
  uint8_t  fans;               /* SET_FANS */
  uint8_t  raw_override, raw_compressor, raw_heater, raw_fans; /* RAW_ACTUATORS */
} ck_case_cmd_t;

typedef struct {
  uint16_t id; ck_case_type_t type;
  float setpoint_c, hyst;
  ck_df_phase_t phase;
  uint64_t phase_start_ns, defrost_start_ns, defrost_deadline_ns;
  float term_c;
  uint8_t fans_enabled;
  uint8_t raw_override, raw_compressor, raw_heater, raw_fans;
  /* G6 fail-safe */
  bool protected_mode;          /* expects governor heartbeats */
  uint64_t last_hb_ns;
  uint64_t hb_timeout_ns;       /* default CK_HB_TIMEOUT_NS; the target scales it so
                                   the timeout is 2 s of WALL time at any sim speed */
  bool hold;                    /* governor lost: hold last-known-good config */
  uint32_t defrosts_started;
} ck_ctl_t;

#define CK_PUMPDOWN_S   30u
#define CK_DRIP_S       120u
#define CK_RECOVER_MAX_S 90u
#define CK_HB_TIMEOUT_NS (2u * CK_NS_PER_S)

typedef struct { float air_c, product_c, evap_c; } ck_probe_t;

void ck_ctl_init(ck_ctl_t *c, uint16_t id, ck_case_type_t t, bool protected_mode, uint64_t now);
/* Returns 0 or a positive errno-style code (EBUSY, EPERM, EINVAL). */
int  ck_ctl_command(ck_ctl_t *c, const ck_case_cmd_t *cmd, uint64_t now);
/* Run the control loop once; writes actuator outputs + setpoint/defrost fields into *out. */
void ck_ctl_tick(ck_ctl_t *c, const ck_probe_t *probe, uint64_t now, ck_case_state_t *out);
bool ck_ctl_in_defrost(const ck_ctl_t *c);
/* Legacy flat architecture: map a management command straight onto a case command
 * (no envelope; "manual defrost" is forced). Returns false if it is not a case command. */
bool ck_ctl_legacy_map(const ck_cmd_t *cmd, ck_case_cmd_t *out);

/* ------------------------------------------------------------------ */
/* governor.c — safety envelope G1–G5 (G6 lives in control.c)          */
/* ------------------------------------------------------------------ */

typedef struct {
  float sp_min, sp_max, sp_default;
  float warn_c;                 /* G4 product warning threshold */
  float limit_c;                /* food-safe limit (reporting) */
  uint32_t defrost_max_s;       /* G3 */
  uint32_t sched_defrost_s;     /* scheduled defrost duration */
  uint32_t sched_interval_s;    /* scheduled defrost interval */
  float term_c;                 /* defrost coil termination temperature */
} ck_env_type_t;

typedef struct {
  uint32_t version;
  ck_env_type_t t[CK_NTYPES];   /* indexed by ck_case_type_t */
  float    deny_margin_c;       /* G1: deny when > this outside band */
  float    sp_max_step_c;       /* G2 */
  uint32_t sp_changes_per_h;    /* G2 */
  uint32_t defrost_min_gap_s;   /* G3: between manual defrosts on a case */
  float    defrost_max_frac;    /* G3: max share of cases defrosting */
  float    g4_clear_c;          /* G4: clears when product < warn - this */
} ck_envelope_t;

void ck_envelope_default(ck_envelope_t *e);

typedef struct {
  float setpoint;               /* governor's view of the applied setpoint */
  float lkg_setpoint;           /* last-known-good */
  uint64_t lkg_candidate_since_ns;
  uint64_t sp_change_ns[8]; uint32_t sp_change_n;   /* recent changes */
  uint64_t last_manual_defrost_ns; bool has_manual_defrost;
  uint64_t next_sched_ns;
  bool g4_active;
  bool alarms_enabled;
} ck_gov_case_t;

#define CK_GOV_RECENT 20

typedef struct {
  ck_envelope_t env;
  uint16_t n;
  ck_case_type_t types[CK_MAX_CASES];
  ck_gov_case_t c[CK_MAX_CASES];
  uint64_t verdicts[5];         /* indexed by ck_verdict_t */
  uint64_t rule_hits[16];
  ck_decision_t recent[CK_GOV_RECENT]; uint32_t recent_head, recent_n;
  uint32_t key_approvals;       /* envelope changes applied via keyconsole */
  uint64_t next_auto_id;
} ck_gov_t;

/* Actions the governor wants applied to case controllers. */
typedef struct { uint16_t case_id; ck_case_cmd_t cmd; } ck_gov_action_t;

void ck_gov_init(ck_gov_t *g, const ck_envelope_t *env, uint16_t n,
                 const ck_case_type_t *types, uint64_t now);
/* Decide one command. On ALLOW/CLAMP, *act is filled and the function returns true. */
bool ck_gov_decide(ck_gov_t *g, const ck_cmd_t *cmd, const ck_case_state_t *states,
                   uint64_t now, ck_decision_t *dec, ck_gov_action_t *act);
/* Periodic checks (G4 override, defrost schedule, LKG tracking). Returns the number
 * of actions written; for each, decs[i] is the matching audit decision. */
int  ck_gov_tick(ck_gov_t *g, const ck_case_state_t *states, uint64_t now,
                 ck_gov_action_t *acts, ck_decision_t *decs, int max);
/* Apply a dangerous command that a keyconsole approved. approval_id must be non-zero.
 * This is the ONLY function that changes the envelope. */
bool ck_gov_apply_approved(ck_gov_t *g, const ck_cmd_t *cmd, uint64_t approval_id,
                           ck_decision_t *dec);
const ck_env_type_t *ck_gov_env_for(const ck_gov_t *g, uint16_t case_id);
/* Writes "status" text (envelope, counters, last decisions) — /dev/coldkeep/status. */
int  ck_gov_status_text(const ck_gov_t *g, char *buf, size_t cap);

/* ------------------------------------------------------------------ */
/* broker.c — schema, auth, rate limit, classification                 */
/* ------------------------------------------------------------------ */

typedef enum { CLASS_READ = 1, CLASS_ROUTINE, CLASS_RISKY, CLASS_DANGEROUS, CLASS_INVALID } ck_class_t;
const char *ck_class_name(ck_class_t c);

typedef enum { BROKER_FORWARD = 1, BROKER_REJECT, BROKER_NEEDS_KEY, BROKER_READ } ck_broker_out_t;

#define CK_MAX_SESSIONS 8
typedef struct { uint32_t id; float tokens; uint64_t last_ns; bool used; } ck_bucket_t;

typedef struct {
  const ck_envelope_t *env;
  uint16_t n; ck_case_type_t types[CK_MAX_CASES];
  float rate_burst, rate_per_s;
  ck_bucket_t b[CK_MAX_SESSIONS];
  uint64_t rate_limited, rejected, forwarded, needs_key, reads;
} ck_broker_t;

void ck_broker_init(ck_broker_t *b, const ck_envelope_t *env, uint16_t n, const ck_case_type_t *types);
/* Pure schema check: returns 0 if valid, else writes a reason. */
int  ck_broker_validate(const ck_broker_t *b, const ck_cmd_t *cmd, char *why, size_t cap);
ck_class_t ck_broker_classify(const ck_broker_t *b, const ck_cmd_t *cmd);
/* Full pipeline step. On REJECT / NEEDS_KEY, *dec holds the decision for the audit log. */
ck_broker_out_t ck_broker_check(ck_broker_t *b, uint32_t session, const ck_cmd_t *cmd,
                                uint64_t now, ck_decision_t *dec);

/* Key request queue (keyconsole). Dangerous commands wait here for local approval. */
#define CK_KEYQ_MAX 32
typedef struct { uint64_t req_id; ck_cmd_t cmd; uint8_t state; /* 0 pending,1 approved,2 rejected */ } ck_keyreq_t;
typedef struct { ck_keyreq_t r[CK_KEYQ_MAX]; uint32_t n; uint64_t next_id; uint64_t dropped; } ck_keyq_t;
void      ck_keyq_init(ck_keyq_t *q);
uint64_t  ck_keyq_push(ck_keyq_t *q, const ck_cmd_t *cmd);       /* 0 if full */
ck_keyreq_t *ck_keyq_find(ck_keyq_t *q, uint64_t req_id);
uint32_t  ck_keyq_pending(const ck_keyq_t *q);

/* ------------------------------------------------------------------ */
/* journal.c — append-only audit journal with CRC32                    */
/* ------------------------------------------------------------------ */

typedef enum { JREC_DECISION = 1, JREC_EVENT = 2, JREC_COMPLIANCE = 3 } ck_jrec_kind_t;

#define CK_JTEXT 160
typedef struct {
  ck_jrec_kind_t kind;
  uint64_t t_ns;
  ck_decision_t d;               /* JREC_DECISION */
  char text[CK_JTEXT];           /* JREC_EVENT / JREC_COMPLIANCE */
} ck_jrec_t;

#define CK_JREC_MAX_BYTES 512
uint32_t ck_crc32(const void *data, size_t len);
/* Encode one record; returns bytes written or 0 if cap too small. */
size_t   ck_journal_encode(const ck_jrec_t *r, uint8_t *buf, size_t cap);
typedef void (*ck_jrec_cb)(const ck_jrec_t *r, void *ctx);
/* Scan a byte stream; calls cb for every intact record; stops at the first torn or
 * corrupt record. Returns the length of the valid prefix. */
size_t   ck_journal_scan(const uint8_t *buf, size_t len, ck_jrec_cb cb, void *ctx, size_t *count);

typedef struct { int fd; size_t records; size_t bytes; size_t recovered_dropped; } ck_journal_t;
/* Open (create) a journal file, truncating any torn tail. Returns 0 or -errno. */
int  ck_journal_open(ck_journal_t *j, const char *path);
/* Append; fsync when sync is true (deny/clamp). Returns 0 or -errno. */
int  ck_journal_append(ck_journal_t *j, const ck_jrec_t *r, bool sync);
void ck_journal_close(ck_journal_t *j);

/* ------------------------------------------------------------------ */
/* metrics.c — minutes above limit, peaks, verdict counts              */
/* ------------------------------------------------------------------ */

typedef struct {
  float limit_c, food_kg;
  float peak_product_c, peak_air_c;
  double s_above;                /* sim seconds with product above limit */
  uint8_t above;                 /* currently above limit */
  uint32_t excursions;           /* times it crossed above */
  uint64_t defrost_s_max;        /* longest single defrost cycle (s) */
  uint64_t cur_defrost_start_ns; uint8_t in_defrost;
} ck_case_metrics_t;

typedef struct {
  uint16_t n;
  ck_case_metrics_t c[CK_MAX_CASES];
  uint64_t verdicts[5];
  uint64_t applied, blocked;     /* applied = ALLOW+CLAMP, blocked = DENY+NEEDS_KEY */
  uint64_t next_compliance_ns, compliance_period_ns;
} ck_metrics_t;

void   ck_metrics_init(ck_metrics_t *m, uint16_t n, const ck_case_state_t *states);
void   ck_metrics_sample(ck_metrics_t *m, const ck_case_state_t *states, uint64_t now, float dt_s);
void   ck_metrics_decision(ck_metrics_t *m, ck_verdict_t v);
double ck_metrics_minutes_above(const ck_metrics_t *m, uint16_t idx);
double ck_metrics_total_minutes_above(const ck_metrics_t *m);
float  ck_metrics_food_at_risk_kg(const ck_metrics_t *m);
bool   ck_metrics_compliance_due(ck_metrics_t *m, uint64_t now);

/* ------------------------------------------------------------------ */
/* proto.c — mgmtd JSON-lines protocol (untrusted input)               */
/* ------------------------------------------------------------------ */

typedef enum { REQ_INVALID = 0, REQ_LOGIN, REQ_STATUS, REQ_CMD, REQ_DEBUG_EXEC } ck_req_kind_t;

#define CK_PROTO_MAX_LINE 1024
typedef struct {
  ck_req_kind_t kind;
  ck_cmd_type_t cmd;           /* REQ_CMD */
  uint16_t case_id;
  float value;
  char user[32], pass[32];     /* REQ_LOGIN */
  char action[32];             /* REQ_DEBUG_EXEC */
  char error[64];              /* set when parsing fails */
} ck_req_t;

/* Parse one request line (a flat JSON object). Returns 0, or -1 with req->error set. */
int ck_proto_parse(const char *line, size_t len, ck_req_t *req);
/* Reply lines (JSON, no trailing newline). Return the length written (truncated to cap-1). */
int ck_proto_reply_decision(char *buf, size_t cap, const ck_decision_t *d);
int ck_proto_reply_error(char *buf, size_t cap, const char *msg);
int ck_proto_reply_ok(char *buf, size_t cap, const char *msg);
/* JSON string escaping into buf; returns bytes written (excluding NUL). */
size_t ck_json_escape(char *buf, size_t cap, const char *s);

/* ------------------------------------------------------------------ */
/* plant.c — all cases' physics + shopper load + probe noise (plantsim)  */
/* ------------------------------------------------------------------ */

typedef struct {
  uint16_t n;
  float dt_s;                   /* plant step (0.1 s default) */
  float start_hour;             /* time of day at t=0, for shopper load */
  ck_case_state_t st[CK_MAX_CASES];      /* ground truth + actuator inputs */
  ck_thermal_params_t p[CK_MAX_CASES];
  ck_rng_t door_rng[CK_MAX_CASES], noise_rng[CK_MAX_CASES];
  uint64_t door_close_ns[CK_MAX_CASES];
  ck_probe_t probe[CK_MAX_CASES];        /* noisy sensor readings */
} ck_plant_t;

void ck_plant_init(ck_plant_t *pl, uint64_t seed, uint16_t n, const ck_case_type_t *types,
                   float ambient_c, float dt_s);
/* Shopper load + probe readings at time now. Consumes the same RNG draws every
 * call regardless of mode, so legacy and protected stores see identical loads. */
void ck_plant_sense(ck_plant_t *pl, uint64_t now);
/* Physics step of dt_s using the actuator fields in pl->st. */
void ck_plant_advance(ck_plant_t *pl);
uint64_t ck_plant_dt_ns(const ck_plant_t *pl);

/* ------------------------------------------------------------------ */
/* store.c — in-process wiring of one store (host sim + property tests) */
/* ------------------------------------------------------------------ */

typedef enum { MODE_LEGACY = 1, MODE_PROTECTED = 2 } ck_mode_t;
const char *ck_mode_name(ck_mode_t m);

/* Event sink: the store reports everything it does through this callback. */
typedef enum {
  EV_DECISION = 1, EV_TELEMETRY, EV_COMPLIANCE, EV_ALARM, EV_G6, EV_PROC, EV_NOTE
} ck_ev_kind_t;
typedef struct ck_store ck_store_t;
typedef void (*ck_ev_cb)(ck_store_t *s, ck_ev_kind_t kind, const void *payload, void *ctx);

typedef struct { uint16_t case_id; uint8_t hold; } ck_ev_g6_t;
typedef struct { uint16_t case_id; float product_c, limit_c; uint8_t silenced; } ck_ev_alarm_t;
typedef struct { const char *proc; const char *what; } ck_ev_proc_t;

struct ck_store {
  ck_mode_t mode;
  uint16_t n;
  uint64_t seed;
  uint64_t now;
  ck_plant_t plant;
  ck_ctl_t ctl[CK_MAX_CASES];
  ck_envelope_t env;
  ck_gov_t gov;
  ck_broker_t broker;
  ck_keyq_t keyq;
  ck_metrics_t m;
  bool gov_up; uint64_t gov_restart_ns;
  uint64_t next_gov_tick_ns;
  uint64_t next_cmd_id;
  bool alarms_enabled;          /* legacy store-wide alarm switch */
  uint8_t alarm_latched[CK_MAX_CASES];
  bool hold_reported[CK_MAX_CASES];
  bool sessions_authed[CK_MAX_SESSIONS];
  /* legacy-only scheduler state */
  uint64_t legacy_next_sched_ns[CK_MAX_CASES];
  ck_ev_cb cb; void *cb_ctx;
};

void ck_store_init(ck_store_t *s, ck_mode_t mode, uint64_t seed, uint16_t n,
                   const ck_case_type_t *types, float ambient_c, float dt_s);
void ck_store_set_sink(ck_store_t *s, ck_ev_cb cb, void *ctx);
/* Advance one plant step (dt_s). */
void ck_store_step(ck_store_t *s);
/* mgmt login: true when credentials match (default admin/admin). */
bool ck_store_login(ck_store_t *s, uint32_t session, const char *user, const char *pass);
/* Submit a management command through the mode's path. Fills *dec. */
ck_verdict_t ck_store_mgmt(ck_store_t *s, uint32_t session, ck_cmd_type_t type,
                           uint16_t case_id, float value, ck_decision_t *dec);
/* A3 helper: what a compromised mgmtd can do. Returns 0 if the privileged action
 * succeeded, else an errno value (host model of the target's access control). */
int  ck_store_compromised_attempt(ck_store_t *s, const char *action);
/* A4: kill governord; it restarts after restart_s sim seconds (protected mode). */
void ck_store_kill_governor(ck_store_t *s, float restart_s);
/* keyconsole approval (local only). */
bool ck_store_key_approve(ck_store_t *s, uint64_t req_id);

#ifdef __cplusplus
}
#endif
#endif /* COLDKEEP_H */
