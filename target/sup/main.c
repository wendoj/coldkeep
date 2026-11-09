/* SPDX-License-Identifier: Apache-2.0
 *
 * sup — supervisor that restarts COLDKEEP's trusted processes when they die
 * (QNX's High Availability Manager is not in the image; see docs/QNX_FEATURES.md).
 *
 *   sup [-l logdir] -c "<name> <ready-path|-> <program> [args...]" [-c ...]
 *
 * Children start in the order given; each one is started when the previous one's
 * ready path exists (its resource manager is attached). When a child exits, sup
 * restarts it at once and measures the wall time until its ready path is back.
 * Every death and restart goes to auditd ("event" records) and to sup's log.
 *
 * While the governor is down, casectl keeps its configuration and, after 2 s
 * without heartbeats, holds it (G6). governord restores its state from disk, so
 * a restart does not reset its rate limits or defrost history.
 */
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "ck_target.h"

#define PROC "sup"
#define MAX_CHILD 24
#define MAX_ARGS 12

extern char **environ;

typedef struct {
  char spec[256];
  char *name, *ready, *argv[MAX_ARGS + 1];
  pid_t pid;
  unsigned restarts;
  uint64_t recent[5]; /* wall times of the last restarts (crash-loop backoff) */
} child_t;

static child_t g_c[MAX_CHILD];
static int g_n;
static const char *g_logdir = "/data/coldkeep/log";
static int g_audit_fd = -1;
static ck_plant_shm_t *g_ps;
static volatile sig_atomic_t g_stop;

static void on_term(int sig) { (void)sig; g_stop = 1; }

static uint64_t sim_now(void) { return g_ps ? ck_sim_now(g_ps) : 0; }

static int parse(child_t *c, const char *spec) {
  ck_strlcpy(c->spec, spec, sizeof c->spec);
  int n = 0;
  char *save = NULL;
  for (char *tok = strtok_r(c->spec, " ", &save); tok && n < MAX_ARGS + 2; tok = strtok_r(NULL, " ", &save)) {
    if (n == 0) c->name = tok;
    else if (n == 1) c->ready = strcmp(tok, "-") ? tok : NULL;
    else c->argv[n - 2] = tok;
    n++;
  }
  if (n < 3) return -1;
  c->argv[n - 2] = NULL;
  return 0;
}

static pid_t start(child_t *c) {
  char log[256];
  snprintf(log, sizeof log, "%s/%s.log", g_logdir, c->name);
  posix_spawn_file_actions_t fa;
  posix_spawn_file_actions_init(&fa);
  posix_spawn_file_actions_addopen(&fa, 1, log, O_WRONLY | O_CREAT | O_APPEND, 0644);
  posix_spawn_file_actions_adddup2(&fa, 1, 2);
  pid_t pid = -1;
  const int rc = posix_spawn(&pid, c->argv[0], &fa, NULL, c->argv, environ);
  posix_spawn_file_actions_destroy(&fa);
  if (rc != 0) {
    ck_log(PROC, "spawn %s: %s", c->name, strerror(rc));
    return -1;
  }
  return pid;
}

/* Wait (up to 10 s) for a child's ready path; returns wall ns waited or -1. */
static int64_t wait_ready(const child_t *c, uint64_t since) {
  if (!c->ready) return (int64_t)(ck_wall_ns() - since);
  struct stat st;
  for (int i = 0; i < 10000; i++) {
    if (stat(c->ready, &st) == 0) return (int64_t)(ck_wall_ns() - since);
    const struct timespec ms = {0, 1000000};
    nanosleep(&ms, NULL);
  }
  return -1;
}

static void restart(child_t *c, int status) {
  const uint64_t died = ck_wall_ns();
  char why[64];
  if (WIFSIGNALED(status)) snprintf(why, sizeof why, "killed by signal %d", WTERMSIG(status));
  else snprintf(why, sizeof why, "exited with status %d", WEXITSTATUS(status));
  ck_log(PROC, "%s (pid %d) %s", c->name, c->pid, why);
  ck_audit_event(g_audit_fd, sim_now(), "sup: %s %s", c->name, why);

  /* crash loop (5 restarts within 10 s): wait a second before trying again */
  if (c->recent[0] && died - c->recent[0] < 10ull * CK_NS_PER_S) {
    ck_log(PROC, "%s is crash-looping; waiting 1 s", c->name);
    sleep(1);
  }
  memmove(c->recent, c->recent + 1, sizeof c->recent - sizeof c->recent[0]);
  c->recent[4] = died;

  c->pid = start(c);
  c->restarts++;
  const int64_t ready = c->pid > 0 ? wait_ready(c, died) : -1;
  if (ready >= 0) {
    ck_log(PROC, "%s restarted (pid %d) and ready in %.1f ms", c->name, c->pid, (double)ready / 1e6);
    ck_audit_event(g_audit_fd, sim_now(), "sup: %s restarted, ready in %.1f ms", c->name, (double)ready / 1e6);
  } else {
    ck_log(PROC, "%s restart failed or not ready after 10 s", c->name);
    ck_audit_event(g_audit_fd, sim_now(), "sup: %s restart failed", c->name);
  }
}

int main(int argc, char **argv) {
  int opt;
  while ((opt = getopt(argc, argv, "l:c:")) != -1) {
    if (opt == 'l') g_logdir = optarg;
    else if (opt == 'c' && g_n < MAX_CHILD) {
      if (parse(&g_c[g_n], optarg) != 0) { fprintf(stderr, "sup: bad -c \"%s\"\n", optarg); return 2; }
      g_n++;
    } else {
      fprintf(stderr, "usage: sup [-l logdir] -c \"name ready-path|- program [args]\" ...\n");
      return 2;
    }
  }
  if (!g_n) { fprintf(stderr, "sup: nothing to supervise\n"); return 2; }
  signal(SIGTERM, on_term);
  signal(SIGINT, on_term);
  g_ps = ck_map_plant(0, 30);
  g_audit_fd = ck_open_retry(CK_DEV_AUDIT, O_RDONLY, 30);
  /* above the processes it restarts, so a restart is not delayed by them */
  if (ck_set_fifo_prio(CK_PRIO_SUP) != EOK) ck_log(PROC, "warning: could not set priority");

  for (int i = 0; i < g_n; i++) {
    const uint64_t t0 = ck_wall_ns();
    g_c[i].pid = start(&g_c[i]);
    const int64_t ready = g_c[i].pid > 0 ? wait_ready(&g_c[i], t0) : -1;
    ck_log(PROC, "started %s (pid %d)%s", g_c[i].name, g_c[i].pid, ready < 0 ? " but it is not ready" : "");
  }
  ck_audit_event(g_audit_fd, sim_now(), "sup: supervising %d processes", g_n);

  while (!g_stop) {
    int status;
    const pid_t pid = waitpid(-1, &status, 0);
    if (pid < 0) {
      if (errno == EINTR) continue;
      if (errno == ECHILD) { sleep(1); continue; }
      break;
    }
    for (int i = 0; i < g_n; i++)
      if (g_c[i].pid == pid && !g_stop) restart(&g_c[i], status);
  }
  /* stopping the store: stop the children too, so nothing restarts them */
  for (int i = g_n - 1; i >= 0; i--)
    if (g_c[i].pid > 0) kill(g_c[i].pid, SIGTERM);
  ck_log(PROC, "stopped");
  return 0;
}
