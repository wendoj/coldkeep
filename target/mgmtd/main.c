/* SPDX-License-Identifier: Apache-2.0
 *
 * mgmtd — the internet-facing management API: JSON lines over TCP (core/proto.c).
 * Default credentials admin/admin, deliberately: they mirror the reported root
 * cause, and COLDKEEP's claim is that the store stays safe anyway.
 *
 *   mgmtd [-p port]       protected: drop + lock every ability, run as ck_mgmt,
 *                         talk only to brokerd (/dev/coldkeep/broker)
 *   mgmtd -L [-p port]    legacy flat architecture: stay root and write the case
 *                         controllers directly
 *
 * Requests and replies are one JSON object per line, e.g.
 *   {"op":"login","user":"admin","pass":"admin"}
 *   {"op":"start_defrost","case":1,"seconds":86400}
 */
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/neutrino.h>
#include <sys/socket.h>
#include <unistd.h>

#include "ck_target.h"

#define PROC "mgmtd"
#define MAX_CLIENTS 32

typedef struct {
  int fd;
  int authed;
  char user[32];
  size_t len;
  char buf[CK_PROTO_MAX_LINE + 2];
} client_t;

static int g_legacy;
static ck_plant_shm_t *g_ps;
static ck_act_shm_t *g_as;
static int g_broker_fd = -1, g_audit_fd = -1;
static int g_case_fd[CK_MAX_CASES];
static int g_alarms_enabled = 1; /* legacy only: protected alarms live in the governor */
static uint64_t g_next_id = 1;
static client_t g_cl[MAX_CLIENTS];

static void send_line(client_t *c, const char *s, int len) {
  char out[CK_PROTO_MAX_LINE + 512];
  if (len > (int)sizeof out - 2) len = (int)sizeof out - 2;
  memcpy(out, s, (size_t)len);
  out[len++] = '\n';
  (void)!write(c->fd, out, (size_t)len);
}

static int status_json(char *buf, size_t cap) {
  ck_plant_shm_t snap;
  ck_plant_read(g_ps, &snap);
  int o = snprintf(buf, cap, "{\"ok\":true,\"mode\":\"%s\",\"t\":%.1f,\"cases\":[",
                   g_legacy ? "legacy" : "protected", (double)snap.sim_ns / 1e9);
  for (uint16_t i = 0; i < snap.ncases && o < (int)cap - 300; i++) {
    const ck_shm_case_t *c = &snap.c[i];
    const ck_act_slot_t *a = &g_as->a[i];
    o += snprintf(buf + o, cap - (size_t)o,
                  "%s{\"id\":%u,\"type\":\"%s\",\"air\":%.2f,\"prod\":%.2f,\"air_true\":%.3f,"
                  "\"prod_true\":%.3f,\"sp\":%.1f,\"comp\":%u,\"heat\":%u,\"defrost\":%u,\"door\":%u,"
                  "\"hold\":%u,\"jitter_us_max\":%u}",
                  i ? "," : "", i + 1, ck_case_type_name((ck_case_type_t)snap.types[i]), (double)c->air_c,
                  (double)c->product_c, (double)c->true_air_c, (double)c->true_product_c, (double)a->setpoint_c,
                  a->compressor_on, a->heater_on, a->defrost_on, c->door_open, a->hold, a->jitter_us_max);
  }
  o += snprintf(buf + o, cap - (size_t)o, "]}");
  return o < (int)cap ? o : (int)cap - 1;
}

/* Legacy flat architecture: apply straight to the controller. */
static void legacy_apply(const ck_cmd_t *cmd, ck_decision_t *d) {
  memset(d, 0, sizeof *d);
  d->cmd = *cmd;
  d->rule_id = CK_RULE_LEGACY;
  int err = 0;
  ck_case_cmd_t cc;
  if (ck_ctl_legacy_map(cmd, &cc)) {
    if (cmd->case_id < 1 || cmd->case_id > g_ps->ncases) err = EINVAL;
    else err = devctl(g_case_fd[cmd->case_id - 1], DCMD_CK_CASE, &cc, sizeof cc, NULL);
  } else if (cmd->type == CMD_DISABLE_ALARMS) {
    g_alarms_enabled = 0;
  }
  d->verdict = err ? VERDICT_DENY : VERDICT_ALLOW;
  d->applied_value = err ? NAN : cmd->value;
  if (err) snprintf(d->reason, sizeof d->reason, "legacy: controller refused: %s", strerror(err));
  else ck_strlcpy(d->reason, "legacy: applied directly", sizeof d->reason);
  ck_audit_decision(g_audit_fd, d, ck_sim_now(g_ps));
}

static void handle_line(client_t *c, int idx, const char *line, size_t len) {
  char out[1024];
  ck_req_t req;
  if (ck_proto_parse(line, len, &req) != 0) {
    send_line(c, out, ck_proto_reply_error(out, sizeof out, req.error));
    return;
  }
  if (req.kind == REQ_LOGIN) {
    /* deliberately weak default credentials (see header) */
    c->authed = !strcmp(req.user, "admin") && !strcmp(req.pass, "admin");
    ck_strlcpy(c->user, req.user, sizeof c->user);
    send_line(c, out, c->authed ? ck_proto_reply_ok(out, sizeof out, "logged in")
                                : ck_proto_reply_error(out, sizeof out, "bad credentials"));
    return;
  }
  if (!c->authed) { send_line(c, out, ck_proto_reply_error(out, sizeof out, "not logged in")); return; }
  if (req.kind == REQ_STATUS) { static char s[4096]; send_line(c, s, status_json(s, sizeof s)); return; }
  if (req.kind == REQ_DEBUG_EXEC) {
    send_line(c, out, ck_proto_reply_error(out, sizeof out, "debug_exec is not available"));
    return;
  }

  ck_cmd_t cmd;
  memset(&cmd, 0, sizeof cmd);
  cmd.id = g_next_id++;
  ck_strlcpy(cmd.origin, "mgmt", sizeof cmd.origin);
  ck_strlcpy(cmd.user, c->user, sizeof cmd.user);
  cmd.type = req.cmd;
  cmd.case_id = req.case_id;
  cmd.value = req.value;
  ck_decision_t d;
  if (g_legacy) {
    legacy_apply(&cmd, &d);
  } else {
    ck_msg_cmd_t m;
    memset(&m, 0, sizeof m);
    m.hdr.type = CK_MSG_CMD;
    m.session = (uint32_t)idx;
    m.cmd = cmd;
    if (MsgSend(g_broker_fd, &m, sizeof m, &d, sizeof d) == -1) {
      char why[96];
      snprintf(why, sizeof why, "broker unavailable: %s", strerror(errno));
      send_line(c, out, ck_proto_reply_error(out, sizeof out, why));
      return;
    }
  }
  send_line(c, out, ck_proto_reply_decision(out, sizeof out, &d));
}

static void on_readable(client_t *c, int idx) {
  const ssize_t n = read(c->fd, c->buf + c->len, sizeof c->buf - 1 - c->len);
  if (n <= 0) { close(c->fd); c->fd = -1; return; }
  c->len += (size_t)n;
  for (;;) {
    char *nl = memchr(c->buf, '\n', c->len);
    if (!nl) break;
    const size_t line_len = (size_t)(nl - c->buf);
    handle_line(c, idx, c->buf, line_len);
    if (c->fd < 0) return;
    memmove(c->buf, nl + 1, c->len - line_len - 1);
    c->len -= line_len + 1;
  }
  if (c->len >= sizeof c->buf - 1) { /* over-long line: drop the connection */
    char out[128];
    send_line(c, out, ck_proto_reply_error(out, sizeof out, "line too long"));
    close(c->fd);
    c->fd = -1;
  }
}

int main(int argc, char **argv) {
  int port = 8080, opt;
  while ((opt = getopt(argc, argv, "Lp:")) != -1) {
    if (opt == 'L') g_legacy = 1;
    else if (opt == 'p') port = atoi(optarg);
    else { fprintf(stderr, "usage: mgmtd [-L] [-p port]\n"); return 2; }
  }
  for (int i = 0; i < MAX_CLIENTS; i++) g_cl[i].fd = -1;

  /* everything that needs privileges happens before the drop */
  const int ls = socket(AF_INET, SOCK_STREAM, 0);
  const int one = 1;
  setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
  struct sockaddr_in sa;
  memset(&sa, 0, sizeof sa);
  sa.sin_family = AF_INET;
  sa.sin_port = htons((uint16_t)port);
  sa.sin_addr.s_addr = htonl(INADDR_ANY);
  if (ls < 0 || bind(ls, (struct sockaddr *)&sa, sizeof sa) != 0 || listen(ls, 16) != 0) {
    ck_log(PROC, "listen on %d: %s", port, strerror(errno));
    return 1;
  }
  g_ps = ck_map_plant(0, 30);
  g_as = ck_map_act(0, 30);
  if (!g_ps || !g_as) { ck_log(PROC, "plant shared memory not available"); return 1; }
  g_audit_fd = ck_open_retry(CK_DEV_AUDIT, O_RDONLY, 30);
  if (g_legacy) {
    for (uint16_t i = 0; i < g_ps->ncases; i++) {
      char path[64];
      snprintf(path, sizeof path, CK_DEV_CASE_FMT, i + 1);
      g_case_fd[i] = ck_open_retry(path, O_RDWR, 30);
      if (g_case_fd[i] < 0) { ck_log(PROC, "open %s: %s", path, strerror(errno)); return 1; }
    }
  } else {
    g_broker_fd = ck_open_retry(CK_DEV_BROKER, O_RDWR, 30);
    if (g_broker_fd < 0) { ck_log(PROC, "open %s: %s", CK_DEV_BROKER, strerror(errno)); return 1; }
  }
  if (ck_set_fifo_prio(CK_PRIO_MGMT) != EOK) ck_log(PROC, "warning: could not set priority");
  if (!g_legacy && ck_drop_to(CK_UID_MGMT, NULL, 0, 1) != 0) {
    ck_log(PROC, "drop privileges: %s", strerror(errno));
    return 1;
  }
  ck_log(PROC, "%s API on port %d, uid %d%s", g_legacy ? "LEGACY" : "protected", port, (int)getuid(),
         g_legacy ? " (root, all abilities)" : " (all abilities denied and locked)");

  for (;;) {
    struct pollfd pf[MAX_CLIENTS + 1];
    int map[MAX_CLIENTS + 1], n = 0;
    pf[n].fd = ls; pf[n].events = POLLIN; map[n++] = -1;
    for (int i = 0; i < MAX_CLIENTS; i++)
      if (g_cl[i].fd >= 0) { pf[n].fd = g_cl[i].fd; pf[n].events = POLLIN; map[n++] = i; }
    if (poll(pf, (nfds_t)n, -1) < 0) continue;
    for (int k = 0; k < n; k++) {
      if (!(pf[k].revents & (POLLIN | POLLHUP | POLLERR))) continue;
      if (map[k] < 0) {
        const int fd = accept(ls, NULL, NULL);
        if (fd < 0) continue;
        int slot = -1;
        for (int i = 0; i < MAX_CLIENTS && slot < 0; i++) if (g_cl[i].fd < 0) slot = i;
        if (slot < 0) { close(fd); continue; }
        memset(&g_cl[slot], 0, sizeof g_cl[slot]);
        g_cl[slot].fd = fd;
      } else {
        on_readable(&g_cl[map[k]], map[k]);
      }
    }
  }
}
