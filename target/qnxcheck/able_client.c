/* SPDX-License-Identifier: Apache-2.0
 *
 * QNX feature checks 1 and 2: a client that optionally drops privileges, then
 * writes to /dev/ckcheck (served by able_server).
 *
 *   ./able_client              stay root, write
 *   ./able_client -u 2000      deny + lock all abilities, switch to uid 2000, write
 *   ./able_client -u 2000 -k   same, but keep the custom ability first, then write
 *   add -s 5                   sleep 5 s afterwards so you can inspect it with pidin
 *
 * Every call prints its return value, so the output shows how dropping and
 * locking abilities behaves for a non-root process.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/procmgr.h>

#define ABILITY_NAME "coldkeep.check.write"
#define PATH "/dev/ckcheck"

static void report(const char *what, int rc) {
  printf("[client] %-58s rc=%d errno=%d (%s)\n", what, rc, rc == -1 ? errno : 0,
         rc == -1 ? strerror(errno) : "ok");
}

int main(int argc, char **argv) {
  setvbuf(stdout, NULL, _IOLBF, 0);
  int uid = -1, keep = 0, sleep_s = 0, opt;
  while ((opt = getopt(argc, argv, "u:ks:")) != -1) {
    if (opt == 'u') uid = atoi(optarg);
    else if (opt == 'k') keep = 1;
    else if (opt == 's') sleep_s = atoi(optarg);
    else { fprintf(stderr, "usage: %s [-u uid] [-k] [-s seconds]\n", argv[0]); return 2; }
  }
  printf("[client] pid=%d start uid=%d euid=%d\n", (int)getpid(), (int)getuid(), (int)geteuid());

  if (uid >= 0) {
    int rc;
    if (keep) {
      errno = 0;
      int id = procmgr_ability_lookup(ABILITY_NAME); /* VERIFY signature */
      printf("[client] procmgr_ability_lookup(\"%s\") = %d (errno=%d)\n", ABILITY_NAME, id, errno);
      if (id < 0) return 1;
      /* keep the custom ability for the non-root domain, deny + lock everything else */
      rc = procmgr_ability(0,
                           PROCMGR_ADN_NONROOT | PROCMGR_AOP_ALLOW | PROCMGR_AOP_LOCK | (unsigned)id,
                           PROCMGR_ADN_NONROOT | PROCMGR_AOP_DENY | PROCMGR_AOP_LOCK | PROCMGR_AID_EOL);
      report("procmgr_ability(keep custom, deny+lock rest, NONROOT)", rc);
    } else {
      rc = procmgr_ability(0, PROCMGR_ADN_NONROOT | PROCMGR_AOP_DENY | PROCMGR_AOP_LOCK | PROCMGR_AID_EOL);
      report("procmgr_ability(deny+lock all, NONROOT)", rc);
    }
    report("setgid", setgid((gid_t)uid));
    report("setuid", setuid((uid_t)uid));
    printf("[client] now uid=%d euid=%d\n", (int)getuid(), (int)geteuid());
    /* also lock the root domain now that we are not root */
    rc = procmgr_ability(0, PROCMGR_ADN_ROOT | PROCMGR_AOP_DENY | PROCMGR_AOP_LOCK | PROCMGR_AID_EOL);
    report("procmgr_ability(deny+lock all, ROOT) as non-root", rc);
  }

  int fd = open(PATH, O_WRONLY);
  report("open " PATH " O_WRONLY", fd);
  if (fd >= 0) {
    ssize_t w = write(fd, "x", 1);
    report("write 1 byte", (int)w);
    close(fd);
  }
  if (sleep_s > 0) {
    printf("[client] sleeping %d s (pid %d) - inspect with pidin now\n", sleep_s, (int)getpid());
    sleep((unsigned)sleep_s);
  }
  return 0;
}
