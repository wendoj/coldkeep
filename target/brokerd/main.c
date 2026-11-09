/* SPDX-License-Identifier: Apache-2.0
 *
 * brokerd — command broker (core/broker.c): schema checks, per-session rate limit,
 * classification. Dangerous commands are queued for local store-key approval and
 * never reach the governor; the rest are forwarded to governord.
 *
 * /dev/coldkeep/broker   owner = mgmtd's uid, mode 0600; CK_MSG_CMD messages,
 *                        reply ck_decision_t. Other senders get EACCES.
 */
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/dispatch.h>
#include <sys/iofunc.h>
#include <sys/neutrino.h>
#include <sys/stat.h>
#include <unistd.h>

#include "ck_target.h"

#define PROC "brokerd"

static resmgr_connect_funcs_t connect_funcs;
static resmgr_io_funcs_t io_funcs;
static iofunc_attr_t attr;
static ck_envelope_t g_env;
static ck_broker_t g_br;
static ck_keyq_t g_keyq;
static ck_plant_shm_t *g_ps;
static int g_gov_fd = -1, g_audit_fd = -1;

static int forward(const ck_cmd_t *cmd, ck_decision_t *d) {
  ck_msg_cmd_t m;
  memset(&m, 0, sizeof m);
  m.hdr.type = CK_MSG_GOV;
  m.cmd = *cmd;
  for (int attempt = 0; attempt < 2; attempt++) {
    if (g_gov_fd < 0) g_gov_fd = open(CK_DEV_GOV, O_RDWR);
    if (g_gov_fd >= 0 && MsgSend(g_gov_fd, &m, sizeof m, d, sizeof *d) != -1) return 0;
    if (g_gov_fd >= 0) close(g_gov_fd); /* governor restarted or gone: reconnect once */
    g_gov_fd = -1;
  }
  return -1;
}

static int on_cmd(message_context_t *ctp, int type, unsigned flags, void *handle) {
  const int uid = ck_sender_uid(ctp->info.scoid);
  if (uid != CK_UID_MGMT && uid != 0) { MsgError(ctp->rcvid, EACCES); return 0; }
  if (ctp->info.msglen < (ssize_t)sizeof(ck_msg_cmd_t)) { MsgError(ctp->rcvid, EINVAL); return 0; }
  ck_msg_cmd_t m;
  memcpy(&m, ctp->msg, sizeof m);
  /* everything in the message is untrusted; origin is set here, user is kept as a label */
  ck_strlcpy(m.cmd.origin, "mgmt", sizeof m.cmd.origin);
  m.cmd.user[sizeof m.cmd.user - 1] = '\0';
  const uint64_t now = ck_sim_now(g_ps);
  m.cmd.recv_ns = now;
  const uint32_t session = (uint32_t)ctp->info.scoid * 1000u + (m.session % 1000u);

  ck_decision_t d;
  switch (ck_broker_check(&g_br, session, &m.cmd, now, &d)) {
    case BROKER_READ:
      d.verdict = VERDICT_ALLOW;
      ck_strlcpy(d.reason, "read-only", sizeof d.reason);
      break;
    case BROKER_REJECT:
      if (d.rule_id != CK_RULE_RATE) ck_audit_decision(g_audit_fd, &d, now); /* rate hits are counted */
      break;
    case BROKER_NEEDS_KEY: {
      const uint64_t req = ck_keyq_push(&g_keyq, &m.cmd);
      char why[96];
      if (req) snprintf(why, sizeof why, "dangerous: queued as key request #%llu", (unsigned long long)req);
      else snprintf(why, sizeof why, "dangerous: key queue full, dropped");
      ck_strlcpy(d.reason, why, sizeof d.reason);
      ck_audit_decision(g_audit_fd, &d, now);
      break;
    }
    case BROKER_FORWARD:
      if (forward(&m.cmd, &d) != 0) {
        memset(&d, 0, sizeof d);
        d.cmd = m.cmd;
        d.verdict = VERDICT_DENY;
        d.rule_id = CK_RULE_UNAVAILABLE;
        d.applied_value = NAN;
        ck_strlcpy(d.reason, "governor unavailable; plant holds safe config", sizeof d.reason);
        ck_audit_decision(g_audit_fd, &d, now);
      }
      break;
  }
  MsgReply(ctp->rcvid, EOK, &d, sizeof d);
  return 0;
}

int main(void) {
  g_ps = ck_map_plant(0, 30);
  if (!g_ps) { ck_log(PROC, "plant shared memory not available"); return 1; }
  ck_case_type_t types[CK_MAX_CASES];
  for (uint16_t i = 0; i < g_ps->ncases; i++) types[i] = (ck_case_type_t)g_ps->types[i];
  ck_envelope_default(&g_env);
  ck_broker_init(&g_br, &g_env, g_ps->ncases, types);
  ck_keyq_init(&g_keyq);
  g_gov_fd = ck_open_retry(CK_DEV_GOV, O_RDWR, 30);
  if (g_gov_fd < 0) ck_log(PROC, "warning: governor not reachable yet: %s", strerror(errno));
  g_audit_fd = ck_open_retry(CK_DEV_AUDIT, O_RDONLY, 30);

  dispatch_t *dpp = dispatch_create();
  iofunc_func_init(_RESMGR_CONNECT_NFUNCS, &connect_funcs, _RESMGR_IO_NFUNCS, &io_funcs);
  iofunc_attr_init(&attr, S_IFNAM | 0600, NULL, NULL);
  attr.uid = CK_UID_MGMT;
  attr.gid = CK_UID_MGMT;
  resmgr_attr_t rattr;
  memset(&rattr, 0, sizeof rattr);
  rattr.nparts_max = 1;
  rattr.msg_max_size = 4096;
  if (!dpp || resmgr_attach(dpp, &rattr, CK_DEV_BROKER, _FTYPE_ANY, 0, &connect_funcs, &io_funcs, &attr) == -1 ||
      message_attach(dpp, NULL, CK_MSG_CMD, CK_MSG_CMD, on_cmd, NULL) == -1) {
    ck_log(PROC, "attach: %s", strerror(errno));
    return 1;
  }
  if (ck_set_fifo_prio(CK_PRIO_BROKER) != EOK) ck_log(PROC, "warning: could not set priority");
  if (ck_drop_to(CK_UID_BROKER, NULL, 0, 0) != 0) { ck_log(PROC, "drop privileges: %s", strerror(errno)); return 1; }
  ck_log(PROC, "accepting commands on %s for %u cases, uid %d", CK_DEV_BROKER, g_ps->ncases, (int)getuid());

  dispatch_context_t *ctp = dispatch_context_alloc(dpp);
  for (;;) {
    if ((ctp = dispatch_block(ctp)) == NULL) { ck_log(PROC, "dispatch_block: %s", strerror(errno)); return 1; }
    dispatch_handler(ctp);
  }
}
