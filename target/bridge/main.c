/* SPDX-License-Identifier: Apache-2.0
 *
 * bridge — read-only telemetry feed to the host relay: JSON lines over TCP 7070.
 *
 *   bridge [-L] [-p port]     -L: tell clients this is the legacy store
 *
 * Every wall-clock second each connected client gets one `telemetry` line (all
 * cases, from the plant shared memory and the controllers' slots) and an `audit`
 * line for every new record in auditd's ring (/dev/coldkeep/audit). In protected
 * mode it also forwards the governor's /dev/coldkeep/status text every 5 s.
 *
 * The bridge never writes the plant: it opens no case controller, maps shared
 * memory read-only, and after binding its socket it switches to its own uid with
 * every ability denied and locked (like mgmtd). Input from clients is discarded.
 */
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "ck_target.h"

#define PROC "bridge"
#define MAX_CLIENTS 8

typedef struct {
  int fd;
  unsigned long last_seq; /* last audit record sent to this client */
} client_t;

static int g_legacy;
static ck_plant_shm_t *g_ps;
static ck_act_shm_t *g_as;
static client_t g_cl[MAX_CLIENTS];
static ck_envelope_t g_env;

/* Send a whole line or drop the client (a slow client must not stall the bridge). */
static void send_to(client_t *c, const char *s, size_t len) {
  if (c->fd < 0) return;
  const ssize_t n = send(c->fd, s, len, 0);
  if (n != (ssize_t)len) {
    close(c->fd);
    c->fd = -1;
  }
}

static void broadcast(const char *s, size_t len) {
  for (int i = 0; i < MAX_CLIENTS; i++) send_to(&g_cl[i], s, len);
}

static int meta_json(char *buf, size_t cap) {
  int o = snprintf(buf, cap,
                   "{\"type\":\"meta\",\"source\":\"qnx-bridge\",\"mode\":\"%s\",\"seed\":%llu,"
                   "\"speed\":%.1f,\"cases\":[",
                   g_legacy ? "legacy" : "protected", (unsigned long long)g_ps->seed, (double)g_ps->speed);
  for (uint16_t i = 0; i < g_ps->ncases && o < (int)cap - 80; i++) {
    const ck_case_type_t t = (ck_case_type_t)g_ps->types[i];
    o += snprintf(buf + o, cap - (size_t)o, "%s{\"id\":%u,\"type\":\"%s\",\"limit\":%.1f}", i ? "," : "",
                  i + 1, ck_case_type_name(t), (double)g_env.t[t].limit_c);
  }
  o += snprintf(buf + o, cap - (size_t)o, "]}\n");
  return o < (int)cap ? o : (int)cap - 1;
}

static int telemetry_json(char *buf, size_t cap) {
  ck_plant_shm_t snap;
  ck_plant_read(g_ps, &snap);
  int o = snprintf(buf, cap, "{\"type\":\"telemetry\",\"t\":%.1f,\"cases\":[", (double)snap.sim_ns / 1e9);
  for (uint16_t i = 0; i < snap.ncases && o < (int)cap - 320; i++) {
    const ck_shm_case_t *c = &snap.c[i];
    const ck_act_slot_t *a = &g_as->a[i];
    o += snprintf(buf + o, cap - (size_t)o,
                  "%s{\"id\":%u,\"type\":\"%s\",\"air\":%.2f,\"prod\":%.2f,\"air_true\":%.3f,"
                  "\"prod_true\":%.3f,\"sp\":%.1f,\"comp\":%u,\"heat\":%u,\"defrost\":%u,\"door\":%u,"
                  "\"hold\":%u,\"alive\":%u,\"jitter_us_max\":%u,\"jitter_us_avg\":%u}",
                  i ? "," : "", i + 1, ck_case_type_name((ck_case_type_t)snap.types[i]), (double)c->air_c,
                  (double)c->product_c, (double)c->true_air_c, (double)c->true_product_c, (double)a->setpoint_c,
                  a->compressor_on, a->heater_on, a->defrost_on, c->door_open, a->hold, a->alive,
                  a->jitter_us_max, a->jitter_us_avg);
  }
  o += snprintf(buf + o, cap - (size_t)o, "]}\n");
  return o < (int)cap ? o : (int)cap - 1;
}

/* Read a whole text file from a resource manager; returns length or -1. */
static int read_text(const char *path, char *buf, size_t cap) {
  const int fd = open(path, O_RDONLY);
  if (fd < 0) return -1;
  size_t o = 0;
  ssize_t n;
  while (o < cap - 1 && (n = read(fd, buf + o, cap - 1 - o)) > 0) o += (size_t)n;
  close(fd);
  buf[o] = '\0';
  return (int)o;
}

/* auditd ring lines look like "#<seq> t=<sim s> <text>". Send each client the
 * records it has not seen yet. */
static void forward_audit(void) {
  static char text[64 * 220 + 256];
  if (read_text(CK_DEV_AUDIT, text, sizeof text) < 0) return;
  char *save = NULL;
  for (char *line = strtok_r(text, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
    if (line[0] != '#') continue;
    char *end = NULL;
    const unsigned long seq = strtoul(line + 1, &end, 10);
    if (!end || *end != ' ') continue;
    char esc[512], out[640];
    ck_json_escape(esc, sizeof esc, end + 1);
    const int len = snprintf(out, sizeof out, "{\"type\":\"audit\",\"seq\":%lu,\"text\":\"%s\"}\n", seq, esc);
    if (len <= 0 || len >= (int)sizeof out) continue;
    for (int i = 0; i < MAX_CLIENTS; i++) {
      if (g_cl[i].fd >= 0 && seq > g_cl[i].last_seq) {
        send_to(&g_cl[i], out, (size_t)len);
        g_cl[i].last_seq = seq;
      }
    }
  }
}

static void forward_status(void) {
  static char text[4096], esc[8192], out[8300];
  if (read_text(CK_DEV_STATUS, text, sizeof text) < 0) return;
  ck_json_escape(esc, sizeof esc, text);
  const int len = snprintf(out, sizeof out, "{\"type\":\"gov_status\",\"t\":%.1f,\"text\":\"%s\"}\n",
                           (double)ck_sim_now(g_ps) / 1e9, esc);
  if (len > 0 && len < (int)sizeof out) broadcast(out, (size_t)len);
}

int main(int argc, char **argv) {
  int port = 7070, opt;
  while ((opt = getopt(argc, argv, "Lp:")) != -1) {
    if (opt == 'L') g_legacy = 1;
    else if (opt == 'p') port = atoi(optarg);
    else { fprintf(stderr, "usage: bridge [-L] [-p port]\n"); return 2; }
  }
  signal(SIGPIPE, SIG_IGN);
  for (int i = 0; i < MAX_CLIENTS; i++) g_cl[i].fd = -1;
  ck_envelope_default(&g_env);

  const int ls = socket(AF_INET, SOCK_STREAM, 0);
  const int one = 1;
  setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
  struct sockaddr_in sa;
  memset(&sa, 0, sizeof sa);
  sa.sin_family = AF_INET;
  sa.sin_port = htons((uint16_t)port);
  sa.sin_addr.s_addr = htonl(INADDR_ANY);
  if (ls < 0 || bind(ls, (struct sockaddr *)&sa, sizeof sa) != 0 || listen(ls, 8) != 0) {
    ck_log(PROC, "listen on %d: %s", port, strerror(errno));
    return 1;
  }
  g_ps = ck_map_plant(0, 30);
  g_as = ck_map_act(0, 30);
  if (!g_ps || !g_as) { ck_log(PROC, "plant shared memory not available"); return 1; }
  if (ck_set_fifo_prio(CK_PRIO_BRIDGE) != EOK) ck_log(PROC, "warning: could not set priority");
  if (ck_drop_to(CK_UID_BRIDGE, NULL, 0, 1) != 0) {
    ck_log(PROC, "drop privileges: %s", strerror(errno));
    return 1;
  }
  ck_log(PROC, "telemetry on port %d (%s store), uid %d (all abilities denied and locked)", port,
         g_legacy ? "legacy" : "protected", (int)getuid());

  uint64_t next_tick = ck_wall_ns(), next_status = next_tick;
  for (;;) {
    struct pollfd pf[MAX_CLIENTS + 1];
    int map[MAX_CLIENTS + 1], n = 0;
    pf[n].fd = ls; pf[n].events = POLLIN; map[n++] = -1;
    for (int i = 0; i < MAX_CLIENTS; i++)
      if (g_cl[i].fd >= 0) { pf[n].fd = g_cl[i].fd; pf[n].events = POLLIN; map[n++] = i; }
    const uint64_t now = ck_wall_ns();
    const int wait_ms = now >= next_tick ? 0 : (int)((next_tick - now) / 1000000u) + 1;
    if (poll(pf, (nfds_t)n, wait_ms) > 0) {
      for (int k = 0; k < n; k++) {
        if (!(pf[k].revents & (POLLIN | POLLHUP | POLLERR))) continue;
        if (map[k] < 0) {
          const int fd = accept(ls, NULL, NULL);
          if (fd < 0) continue;
          int slot = -1;
          for (int i = 0; i < MAX_CLIENTS && slot < 0; i++) if (g_cl[i].fd < 0) slot = i;
          if (slot < 0) { close(fd); continue; }
          g_cl[slot].fd = fd;
          g_cl[slot].last_seq = 0;
          char m[1024];
          send_to(&g_cl[slot], m, (size_t)meta_json(m, sizeof m));
        } else {
          char junk[256]; /* clients have nothing to say to the bridge */
          if (read(g_cl[map[k]].fd, junk, sizeof junk) <= 0) { close(g_cl[map[k]].fd); g_cl[map[k]].fd = -1; }
        }
      }
    }
    if (ck_wall_ns() < next_tick) continue;
    next_tick += 1000000000ull;
    if (ck_wall_ns() > next_tick) next_tick = ck_wall_ns() + 1000000000ull; /* fell behind: skip */
    static char tbuf[4096];
    broadcast(tbuf, (size_t)telemetry_json(tbuf, sizeof tbuf));
    forward_audit();
    if (!g_legacy && ck_wall_ns() >= next_status) {
      next_status = ck_wall_ns() + 5000000000ull;
      forward_status();
    }
  }
}
