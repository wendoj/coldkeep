/* SPDX-License-Identifier: Apache-2.0
 *
 * plantsim — the simulated refrigeration plant. Runs core/plant.c in sim time
 * (10 Hz sim, paced to wall time x speed), takes actuator states from the case
 * controllers (act shared memory) and publishes probe readings (plant shared
 * memory, read-only for everyone else).
 *
 *   plantsim [-s seed] [-x speed] [-c chilled:4,frozen:2,coldroom:2] [-a ambient]
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/neutrino.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "ck_target.h"

#define PROC "plantsim"
#define PULSE_TICK (_PULSE_CODE_MINAVAIL + 1)

static int parse_cases(const char *spec, ck_case_type_t *types) {
  int n = 0;
  char buf[128];
  ck_strlcpy(buf, spec, sizeof buf);
  for (char *tok = strtok(buf, ","); tok; tok = strtok(NULL, ",")) {
    char *colon = strchr(tok, ':');
    int count = colon ? atoi(colon + 1) : 1;
    if (colon) *colon = '\0';
    ck_case_type_t t;
    if (!strcmp(tok, "chilled")) t = CASE_CHILLED;
    else if (!strcmp(tok, "frozen")) t = CASE_FROZEN;
    else if (!strcmp(tok, "coldroom")) t = CASE_COLDROOM;
    else return -1;
    for (int k = 0; k < count && n < CK_MAX_CASES; k++) types[n++] = t;
  }
  return n;
}

static void *create_shm(const char *name, size_t size, mode_t mode, uid_t uid, gid_t gid) {
  shm_unlink(name);
  int fd = shm_open(name, O_RDWR | O_CREAT | O_EXCL, mode);
  if (fd < 0) return NULL;
  if (ftruncate(fd, (off_t)size) != 0 || fchown(fd, uid, gid) != 0 || fchmod(fd, mode) != 0) {
    close(fd);
    return NULL;
  }
  void *p = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  close(fd);
  if (p == MAP_FAILED) return NULL;
  memset(p, 0, size);
  return p;
}

static void publish(ck_plant_shm_t *s, const ck_plant_t *pl, uint64_t sim_ns) {
  atomic_fetch_add_explicit(&s->seq, 1, memory_order_acq_rel);
  s->sim_ns = sim_ns;
  for (uint16_t i = 0; i < pl->n; i++) {
    ck_shm_case_t *c = &s->c[i];
    c->air_c = pl->probe[i].air_c;
    c->product_c = pl->probe[i].product_c;
    c->evap_c = pl->probe[i].evap_c;
    c->true_air_c = pl->st[i].air_c;
    c->true_product_c = pl->st[i].product_c;
    c->door_open = pl->st[i].door_open;
    c->compressor_on = pl->st[i].compressor_on;
    c->heater_on = pl->st[i].heater_on;
    c->fans_on = pl->st[i].fans_on;
    c->defrost_on = pl->st[i].defrost_on;
    c->setpoint_c = pl->st[i].setpoint_c;
  }
  atomic_fetch_add_explicit(&s->seq, 1, memory_order_acq_rel);
}

int main(int argc, char **argv) {
  uint64_t seed = 7;
  float speed = 30.0f, ambient = 20.0f;
  const char *spec = "chilled:4,frozen:2,coldroom:2";
  int opt;
  while ((opt = getopt(argc, argv, "s:x:c:a:")) != -1) {
    switch (opt) {
      case 's': seed = strtoull(optarg, NULL, 10); break;
      case 'x': speed = (float)atof(optarg); break;
      case 'c': spec = optarg; break;
      case 'a': ambient = (float)atof(optarg); break;
      default:
        fprintf(stderr, "usage: plantsim [-s seed] [-x speed] [-c spec] [-a ambient]\n");
        return 2;
    }
  }
  ck_case_type_t types[CK_MAX_CASES];
  int n = parse_cases(spec, types);
  if (n <= 0 || speed <= 0) { ck_log(PROC, "bad case spec or speed"); return 2; }

  ck_plant_shm_t *ps = create_shm(CK_SHM_PLANT, sizeof *ps, 0644, CK_UID_PLANT, CK_UID_PLANT);
  ck_act_shm_t *as = create_shm(CK_SHM_ACT, sizeof *as, 0664, CK_UID_PLANT, CK_UID_CASE);
  if (!ps || !as) { ck_log(PROC, "shared memory: %s", strerror(errno)); return 1; }

  static ck_plant_t pl;
  ck_plant_init(&pl, seed, (uint16_t)n, types, ambient, 0.1f);
  ps->ncases = (uint16_t)n;
  for (int i = 0; i < n; i++) ps->types[i] = (uint8_t)types[i];
  ps->speed = speed;
  ps->seed = seed;
  uint64_t sim_ns = 0;
  ck_plant_sense(&pl, sim_ns);
  publish(ps, &pl, sim_ns);
  as->magic = CK_SHM_MAGIC;
  ps->magic = CK_SHM_MAGIC;

  int chid = ChannelCreate(0);
  int coid = ConnectAttach(0, 0, chid, _NTO_SIDE_CHANNEL, 0);
  struct sigevent ev;
  SIGEV_PULSE_INIT(&ev, coid, CK_PRIO_PLANT, PULSE_TICK, 0);
  timer_t tid;
  if (chid < 0 || coid < 0 || timer_create(CLOCK_MONOTONIC, &ev, &tid) != 0) {
    ck_log(PROC, "timer: %s", strerror(errno));
    return 1;
  }
  struct itimerspec its = {.it_value = {0, 10000000}, .it_interval = {0, 10000000}}; /* 10 ms wall */
  timer_settime(tid, 0, &its, NULL);

  if (ck_set_fifo_prio(CK_PRIO_PLANT) != EOK) ck_log(PROC, "warning: could not set priority");
  if (ck_drop_to(CK_UID_PLANT, NULL, 0, 0) != 0) { ck_log(PROC, "drop privileges: %s", strerror(errno)); return 1; }
  ck_log(PROC, "running: %d cases, seed %llu, speed %.0fx, uid %d", n, (unsigned long long)seed,
         (double)speed, (int)getuid());

  const uint64_t dt = ck_plant_dt_ns(&pl);
  /* at most ~2 ticks' worth of steps per 10 ms pulse: if the CPU cannot keep up,
   * sim time falls behind (and we say so) rather than starving lower priorities */
  const int max_steps = 2 * (int)(speed / 10.0f + 1.0f);
  uint64_t start = ck_wall_ns(), last_report = start, lag_ns = 0;
  for (;;) {
    struct _pulse pulse;
    if (MsgReceivePulse(chid, &pulse, sizeof pulse, NULL) == -1) continue;
    if (pulse.code != PULSE_TICK) continue;
    const uint64_t wall = ck_wall_ns();
    const uint64_t target = (uint64_t)((double)(wall - start) * (double)speed) - lag_ns;
    if (target > sim_ns + (uint64_t)max_steps * dt * 5) { /* far behind: drop the backlog */
      lag_ns += target - sim_ns;
    }
    if (wall - last_report >= 10 * CK_NS_PER_S) {
      if (lag_ns) ck_log(PROC, "running behind real speed: sim lags by %.1f s", (double)lag_ns / 1e9);
      last_report = wall;
    }
    for (int steps = 0; sim_ns + dt <= target && steps < max_steps; steps++) {
      for (int i = 0; i < n; i++) {
        const ck_act_slot_t *a = &as->a[i];
        ck_case_state_t *st = &pl.st[i];
        if (a->alive) {
          st->compressor_on = a->compressor_on;
          st->heater_on = a->heater_on;
          st->fans_on = a->fans_on;
          st->defrost_on = a->defrost_on;
          st->setpoint_c = a->setpoint_c;
        } else { /* no controller: nothing drives the plant */
          st->compressor_on = st->heater_on = st->defrost_on = 0;
          st->fans_on = 0;
        }
      }
      ck_plant_sense(&pl, sim_ns);
      ck_plant_advance(&pl);
      sim_ns += dt;
      publish(ps, &pl, sim_ns);
    }
  }
}
