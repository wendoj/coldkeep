/* valvectl: write "open 40" to /dev/valve, optionally after giving up privileges.
 *   ./valvectl                      as whoever runs it
 *   ./valvectl --keep UID [SECS]    keep only demo.valve.write, become UID
 *   ./valvectl --drop UID [SECS]    give up every ability, become UID
 *   ./valvectl --late-drop UID      open first, then give up the ability
 * SECS keeps the process alive afterwards so you can look at it with pidin. */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/procmgr.h>

#define ABILITY "demo.valve.write"
#define DENY_REST (PROCMGR_ADN_NONROOT | PROCMGR_AOP_DENY | PROCMGR_AOP_LOCK | PROCMGR_AID_EOL)

static void become(uid_t uid) {
  if (setgid(uid) == -1 || setuid(uid) == -1) { perror("valvectl: setuid"); exit(1); }
}

int main(int argc, char **argv) {
  const char *mode = argc > 2 ? argv[1] : "";
  uid_t uid = argc > 2 ? (uid_t)atoi(argv[2]) : 0;
  int secs = argc > 3 ? atoi(argv[3]) : 0;
  int id = procmgr_ability_lookup(ABILITY);
  int fd = -1;

  if (strcmp(mode, "--keep") == 0) {
    procmgr_ability(0, PROCMGR_ADN_NONROOT | PROCMGR_AOP_ALLOW | PROCMGR_AOP_LOCK | id, DENY_REST);
    become(uid);
  } else if (strcmp(mode, "--drop") == 0) {
    procmgr_ability(0, DENY_REST);
    become(uid);
  } else if (strcmp(mode, "--late-drop") == 0) {
    procmgr_ability(0, PROCMGR_ADN_NONROOT | PROCMGR_AOP_ALLOW | id, DENY_REST);
    become(uid);
    fd = open("/dev/valve", O_WRONLY);
    procmgr_ability(0, PROCMGR_ADN_NONROOT | PROCMGR_AOP_DENY | PROCMGR_AOP_LOCK | id, PROCMGR_AID_EOL);
    printf("valvectl: opened /dev/valve, then dropped %s\n", ABILITY);
  }

  if (fd == -1) fd = open("/dev/valve", O_WRONLY);
  if (fd == -1) { printf("valvectl: open: %s\n", strerror(errno)); return 1; }
  if (write(fd, "open 40\n", 8) == -1)
    printf("valvectl: uid %d: write: %s\n", (int)getuid(), strerror(errno));
  else
    printf("valvectl: uid %d: write accepted\n", (int)getuid());

  if (strcmp(mode, "--drop") == 0) {   /* try to take the ability back */
    int rc = procmgr_ability(0, PROCMGR_ADN_NONROOT | PROCMGR_AOP_ALLOW | id, PROCMGR_AID_EOL);
    printf("valvectl: ask for %s back: returned %s\n", ABILITY, rc == EOK ? "EOK" : strerror(rc));
    if (write(fd, "open 40\n", 8) == -1)
      printf("valvectl: uid %d: write again: %s\n", (int)getuid(), strerror(errno));
    else
      printf("valvectl: uid %d: write again accepted\n", (int)getuid());
  }
  if (secs > 0) {
    printf("valvectl: pid %d, sleeping %d s\n", (int)getpid(), secs);
    sleep((unsigned)secs);
  }
  return 0;
}
