/* SPDX-License-Identifier: Apache-2.0
 *
 * Shared definitions for the COLDKEEP QNX processes: paths, user ids, shared
 * memory layout, devctl codes, inter-process messages and small helpers.
 *
 * Trust boundary (enforced by QNX, not by this code):
 *   /dev/coldkeep/case/N   any client may open; devctl needs coldkeep.plant.write
 *   /dev/coldkeep/gov      owner ck_broker, mode 0600: only brokerd may connect
 *   /dev/coldkeep/broker   owner ck_mgmt,   mode 0600: mgmtd's only way in
 *   /dev/coldkeep/audit    readable by all; records accepted from trusted uids only
 *   /dev/coldkeep/status   read-only envelope + recent decisions
 */
#ifndef CK_TARGET_H
#define CK_TARGET_H

#include <devctl.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <sys/iomsg.h>
#include <sys/types.h>

#include "coldkeep.h"

/* ---- paths ---- */
#define CK_DEV_DIR      "/dev/coldkeep"
#define CK_DEV_CASE_FMT "/dev/coldkeep/case/%u"
#define CK_DEV_GOV      "/dev/coldkeep/gov"
#define CK_DEV_BROKER   "/dev/coldkeep/broker"
#define CK_DEV_AUDIT    "/dev/coldkeep/audit"
#define CK_DEV_STATUS   "/dev/coldkeep/status"
#define CK_SHM_PLANT    "/coldkeep_plant"
#define CK_SHM_ACT      "/coldkeep_act"
#define CK_DATA_DIR     "/data/coldkeep"
#define CK_JOURNAL      CK_DATA_DIR "/audit.bin"
#define CK_GOV_STATE    CK_DATA_DIR "/gov.state"

#define CK_ABILITY_PLANT_WRITE "coldkeep.plant.write"

/* ---- user ids (no passwd entries needed; uid == gid) ---- */
#define CK_UID_PLANT  1101
#define CK_UID_CASE   1102
#define CK_UID_GOV    1103
#define CK_UID_BROKER 1104
#define CK_UID_MGMT   1105
#define CK_UID_AUDIT  1106

/* ---- SCHED_FIFO priorities ---- */
#define CK_PRIO_CASE   40
#define CK_PRIO_GOV    35
#define CK_PRIO_PLANT  30
#define CK_PRIO_BROKER 25
#define CK_PRIO_AUDIT  20
#define CK_PRIO_MGMT   10

/* ---- shared memory: plantsim -> everyone (read-only) ---- */
#define CK_SHM_MAGIC 0x434B504Cu /* "CKPL" */
typedef struct {
  float air_c, product_c, evap_c;   /* probe readings (noisy) */
  float true_air_c, true_product_c; /* ground truth, telemetry only */
  uint8_t door_open, compressor_on, heater_on, fans_on, defrost_on;
  float setpoint_c;
} ck_shm_case_t;

typedef struct {
  uint32_t magic;
  uint16_t ncases;
  uint8_t types[CK_MAX_CASES];
  float speed;
  uint64_t seed;
  _Atomic uint32_t seq;             /* seqlock: odd while writing */
  uint64_t sim_ns;
  ck_shm_case_t c[CK_MAX_CASES];
} ck_plant_shm_t;

/* ---- shared memory: casectl N -> plantsim/governor (one writer per slot) ---- */
typedef struct {
  _Atomic uint32_t seq;
  uint8_t compressor_on, heater_on, fans_on, defrost_on, hold, alive;
  float setpoint_c;
  uint64_t ticks;
  uint32_t jitter_us_max, jitter_us_avg;  /* last wall-clock second */
} ck_act_slot_t;

typedef struct {
  uint32_t magic;
  ck_act_slot_t a[CK_MAX_CASES];
} ck_act_shm_t;

/* ---- devctl on /dev/coldkeep/case/N ---- */
#define DCMD_CK_CASE __DIOT(_DCMD_MISC, 0x10, ck_case_cmd_t)

/* ---- messages (sent with MsgSend on an fd opened on the server's path) ---- */
#define CK_MSG_BASE   (_IO_MAX + 0x100)
#define CK_MSG_CMD    (CK_MSG_BASE + 1)  /* mgmtd -> brokerd,  reply ck_decision_t */
#define CK_MSG_GOV    (CK_MSG_BASE + 2)  /* brokerd -> governord, reply ck_decision_t */
#define CK_MSG_AUDIT  (CK_MSG_BASE + 3)  /* trusted -> auditd, reply empty */
#define CK_MSG_LAST   CK_MSG_AUDIT

typedef struct { uint16_t type; uint16_t subtype; } ck_msg_hdr_t;
typedef struct { ck_msg_hdr_t hdr; uint32_t session; ck_cmd_t cmd; } ck_msg_cmd_t;
typedef struct { ck_msg_hdr_t hdr; ck_jrec_t rec; } ck_msg_audit_t;

/* ---- helpers (ck_target.c) ---- */
void ck_log(const char *proc, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
int  ck_set_fifo_prio(int prio);
/* Keep at most one ability for the non-root domain (allowed + locked), then switch
 * to uid. deny_rest: also deny + lock every other ability (untrusted processes). */
int  ck_drop_to(uid_t uid, const unsigned *keep, int nkeep, int deny_rest);
uint64_t ck_wall_ns(void);
/* Map the plant/act shared memory. writable: map for writing (owner only). */
ck_plant_shm_t *ck_map_plant(int writable, int wait_s);
ck_act_shm_t   *ck_map_act(int writable, int wait_s);
void ck_plant_read(const ck_plant_shm_t *s, ck_plant_shm_t *out); /* consistent snapshot */
uint64_t ck_sim_now(const ck_plant_shm_t *s);
/* Build the governor's view of every case (probe values + controller state). */
void ck_case_view(const ck_plant_shm_t *ps, const ck_act_shm_t *as, ck_case_state_t *v);
/* Open a path, retrying for up to wait_s seconds. */
int  ck_open_retry(const char *path, int flags, int wait_s);
/* Send a record to auditd (fd from ck_open_retry(CK_DEV_AUDIT)); -1 on failure. */
int  ck_audit_send(int fd, const ck_jrec_t *rec);
int  ck_audit_decision(int fd, const ck_decision_t *d, uint64_t sim_ns);
int  ck_audit_event(int fd, uint64_t sim_ns, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
/* uid of the process that sent the message identified by scoid; -1 on error */
int  ck_sender_uid(int scoid);

#endif
