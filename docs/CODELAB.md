# Codelab: zero trust inside one box on QNX 8.0

Suppose the process that talks to the network gets taken over. On most systems that's game over for anything the process could reach, because the attacker inherits its rights, and if it ran as root that's everything. QNX 8.0 lets you do better: a driver can demand a **custom ability** that only one process holds, and a process can give up its abilities so thoroughly that it can't take them back.

You'll build two small C programs and watch QNX enforce that:

- `valved`, a resource manager that owns `/dev/valve` and only takes writes from a process holding the custom ability `demo.valve.write`;
- `valvectl`, a client that writes `open 40` to the valve as root, as the trusted controller, and as a compromised process that has dropped everything.

By the end, QNX will have refused root, accepted a non-root process holding the ability, and refused a process that dropped it and then tried to take it back.

| | |
|---|---|
| **Time** | About 35 minutes |
| **You need** | QNX SDP 8.0 on your computer (the free non-commercial licence works), and a QNX 8.0 target you can reach over SSH as root: QEMU, a Raspberry Pi 4 or real hardware. No target yet? See [Appendix A](#appendix-a-a-qemu-target-in-a-few-minutes). |
| **You'll know** | C and a shell. No QNX experience needed. |
| **Code** | About 150 lines of C in two files, all shown below. The finished files are also in [docs/codelab/](codelab/). |

Every output block below was copied from a QNX 8.0 x86_64 target running in QEMU. Your pids and the ability id will be different.

---

## Step 1: Set up (5 min)

On your computer, load the SDP environment and make a working directory:

```sh
source ~/qnx800/qnxsdp-env.sh
mkdir valve && cd valve
```

Create this `Makefile`:

```make
# Build with the QNX SDP environment sourced:  source ~/qnx800/qnxsdp-env.sh
#   make                  x86_64 (QEMU, most PCs)
#   make ARCH=aarch64le   Raspberry Pi 4
ARCH   ?= x86_64
CC     := qcc -Vgcc_nto$(ARCH)
CFLAGS := -Wall -O2

all: valved valvectl

%: %.c
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -f valved valvectl

.PHONY: all clean
```

`qcc` is the QNX compiler driver, and `-Vgcc_ntox86_64` picks the x86_64 target; a Pi needs `make ARCH=aarch64le`. If you use the QNX VS Code extension instead, the same files build there.

Then check that you can reach the target and make a directory on it. Any writable directory will do; this codelab uses `/data/home/valve`. For a QEMU target with SSH forwarded to port 2222, add `-p 2222` to `ssh` and `-P 2222` to `scp`.

```sh
TARGET=root@192.168.1.50          # your target's address
ssh $TARGET 'uname -a; mkdir -p /data/home/valve'
```

```text
QNX store-a 8.0.0 2026/06/05-16:26:03EDT x86pc x86_64
```

## Step 2: A resource manager that accepts anything (6 min)

On QNX, a driver is an ordinary process called a **resource manager**. It registers a path, and the kernel delivers every `open()`, `write()` and `read()` on that path to it as a message. Start with one that says yes to everyone. Save this as `valved.c`:

```c
/* valved, step 2 of the codelab: a resource manager for /dev/valve that accepts every write. */
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/dispatch.h>
#include <sys/iofunc.h>
#include <sys/stat.h>

static int valve_write(resmgr_context_t *ctp, io_write_t *msg, RESMGR_OCB_T *ocb) {
  int status = iofunc_write_verify(ctp, msg, ocb, NULL);
  if (status != EOK) return status;
  if ((msg->i.xtype & _IO_XTYPE_MASK) != _IO_XTYPE_NONE) return ENOSYS;

  char cmd[64] = "";
  size_t n = msg->i.nbytes < sizeof cmd - 1 ? msg->i.nbytes : sizeof cmd - 1;
  resmgr_msgread(ctp, cmd, n, sizeof msg->i);
  cmd[strcspn(cmd, "\n")] = '\0';

  printf("valved: accepted '%s'\n", cmd);
  _IO_SET_WRITE_NBYTES(ctp, msg->i.nbytes);
  return _RESMGR_NPARTS(0);
}

int main(void) {
  static resmgr_connect_funcs_t connect_funcs;
  static resmgr_io_funcs_t io_funcs;
  static iofunc_attr_t attr;
  setvbuf(stdout, NULL, _IOLBF, 0);

  dispatch_t *dpp = dispatch_create();
  iofunc_func_init(_RESMGR_CONNECT_NFUNCS, &connect_funcs, _RESMGR_IO_NFUNCS, &io_funcs);
  io_funcs.write = valve_write;
  iofunc_attr_init(&attr, S_IFNAM | 0666, NULL, NULL);
  if (resmgr_attach(dpp, NULL, "/dev/valve", _FTYPE_ANY, 0,
                    &connect_funcs, &io_funcs, &attr) == -1) {
    perror("valved: resmgr_attach");
    return 1;
  }
  printf("valved: serving /dev/valve\n");

  dispatch_context_t *ctp = dispatch_context_alloc(dpp);
  for (;;) {
    if ((ctp = dispatch_block(ctp)) == NULL) return 1;
    dispatch_handler(ctp);
  }
}
```

`iofunc_func_init` fills in default handlers for everything, so you only replace `write`. `resmgr_msgread` copies the written bytes out of the client's message. The `0666` mode means any user may open the file, and that's deliberate: in a minute you'll see a check that file permissions can't give you.

Build it and copy it over. Then open **two SSH sessions** to the target: the server runs in the first one, so you can watch its log, and you type commands in the second.

```sh
make valved
scp valved $TARGET:/data/home/valve/
```

Server session:

```sh
ssh $TARGET
cd /data/home/valve && ./valved
```

Command session: write to the valve as root, then as uid 1105, an unprivileged user standing in for the web server.

```sh
ssh $TARGET
cd /data/home/valve
ls -l /dev/valve
echo "open 40" > /dev/valve
on -u 1105 sh -c 'echo "open 90" > /dev/valve'
```

```text
nrw-rw-rw-  1 root root 0 2026-11-10 22:54 /dev/valve
```

Server session:

```text
valved: serving /dev/valve
valved: accepted 'open 40'
valved: accepted 'open 90'
```

Both writes went through. `on -u` is QNX's way of running a command as another user. Press Ctrl-C in the server session to stop `valved` before the next step.

## Step 3: Create a custom ability and check it on every write (6 min)

An **ability** in QNX is a named permission the process manager tracks for each process, and you can make up your own. Add these lines to `valved.c`, below the includes:

```c
#include <sys/procmgr.h>

#define ABILITY "demo.valve.write"

static int valve_id;

/* Ask the kernel who sent this message and whether it holds our ability. */
static int holds_ability(resmgr_context_t *ctp, uid_t *uid) {
  struct _client_info *info = NULL;
  struct _client_able able = { .ability = (unsigned)valve_id };
  *uid = (uid_t)-1;
  if (iofunc_client_info_able(ctp, 0, &info, 0, &able, 1) != EOK) return 0;
  *uid = info->cred.euid;
  iofunc_client_info_ext_free(&info);
  return able.flags != 0;
}
```

`iofunc_client_info_able` asks the kernel about the process that sent this particular message. The answer comes from the kernel, not from anything the client wrote, so a client can't lie about who it is. `able.flags` comes back non-zero when the client holds the ability.

In `valve_write`, replace the `printf("valved: accepted ...")` line with the check:

```c
  uid_t uid;
  if (!holds_ability(ctp, &uid)) {
    printf("valved: refused '%s' from uid %d\n", cmd, (int)uid);
    return EPERM;
  }
  printf("valved: accepted '%s' from uid %d\n", cmd, (int)uid);
```

In `main`, right after `setvbuf`, create the ability:

```c
  valve_id = procmgr_ability_create(ABILITY, 0);
  if (valve_id < 0) valve_id = procmgr_ability_lookup(ABILITY); /* made by an earlier run */
  if (valve_id < 0) { printf("valved: no ability id (%d)\n", valve_id); return 1; }
  printf("valved: %s is ability %d\n", ABILITY, valve_id);
```

An ability lasts until the target reboots, so the second time you start `valved`, `procmgr_ability_create` fails and the lookup finds the existing id.

Rebuild, copy it over, start `./valved` again in the server session, and repeat the root write in the command session:

```sh
echo "open 40" > /dev/valve; echo $?
```

```text
1
```

Server session:

```text
valved: demo.valve.write is ability 91
valved: serving /dev/valve
valved: refused 'open 40' from uid 0
```

**Root was refused.** On QNX 8.0, a new custom ability isn't held by anyone by default, root included, so the process that needs it has to be given it explicitly. `EPERM` from the handler becomes the `write()` error in the client, which is why the shell's exit status is 1.

## Step 4: The client, and the one process that may write (5 min)

Now the client. Save this as `valvectl.c`. The `--late-drop` mode is for the optional Step 6, so you can skip over it for now:

```c
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
```

`procmgr_ability` takes a list of operations, ended by an entry that includes `PROCMGR_AID_EOL`. Every ability has two halves, one used while the process is root and one used while it isn't (`PROCMGR_ADN_ROOT` and `PROCMGR_ADN_NONROOT`). `--keep` does three things, in this order:

1. allows the non-root half of `demo.valve.write` and locks it;
2. denies and locks every other non-root ability (`DENY_REST`; `PROCMGR_AID_EOL` with an operation means "and all the rest");
3. switches to the given uid, which it can still do because it's root until that moment.

This is the process you trust to move the valve: not root, and holding exactly one permission. Run `make`, copy both programs to the target (stop `valved` first, or `scp` can't replace the running file), restart `./valved` in the server session, and in the command session:

```sh
./valvectl --keep 2000 30 &
```

```text
valvectl: uid 2000: write accepted
valvectl: pid 1962006, sleeping 30 s
```

While it sleeps, look at its abilities, using the pid it printed:

```sh
pidin -p 1962006 abilities | grep -e valve -e setuid
```

```text
 1962006 ./valvectl
           spawn_setuid  R-L-
                 setuid  R-L-
       demo.valve.write  -NL-
```

Server session:

```text
valved: accepted 'open 40' from uid 2000
```

`pidin ... abilities` shows four flags per ability:

| Flag | Meaning |
|---|---|
| `R` | held while the effective uid is 0 |
| `N` | held while the effective uid isn't 0 |
| `L` | locked: it can't be changed any more |
| `I` | passed on to children across spawn and exec |

`demo.valve.write -NL-` is the ability, held as uid 2000 and locked. `setuid R-L-` means the process could only change uid while it was root; now that it's uid 2000, it can never get back to root.

## Step 5: The compromised process (5 min)

Now play the attacker. The web server has been taken over, but it started up the way a well-behaved service should: it gave up every ability, locked them, and switched to its own uid. Then the attacker tries to use it to open the valve:

```sh
./valvectl --drop 1105 30 &
```

```text
valvectl: uid 1105: write: Operation not permitted
valvectl: ask for demo.valve.write back: returned EOK
valvectl: uid 1105: write again: Operation not permitted
valvectl: pid 2007063, sleeping 30 s
```

```sh
pidin -p 2007063 abilities | grep -e valve -e setuid
```

```text
 2007063 ./valvectl
           spawn_setuid  R-L-
                 setuid  R-L-
       demo.valve.write  --L-
```

Server session:

```text
valved: refused 'open 40' from uid 1105
valved: refused 'open 40' from uid 1105
```

The write fails with `EPERM`, and QNX produced that error, not anything the attacker can patch out of the client. Look at the second attempt carefully: `procmgr_ability` returned `EOK` when asked to allow `demo.valve.write` again, but nothing changed. The ability still reads `--L-` (held under no uid, locked), and the next write is refused just like the first. So don't treat a return code as proof that you've dropped a privilege; check it with `pidin` the way you just did, or by trying the operation.

That's the whole idea of zero trust inside one box: `valved` doesn't care who you are or whether you're root, only whether the kernel says you hold `demo.valve.write`, and exactly one process does.

## Step 6 (optional): check every message, not only at open (6 min)

Where you put the check matters. A tempting shortcut is to check once in `open()` and trust every write on that file descriptor afterwards. Here's why that's a mistake.

Add an "open only" mode to `valved.c`. Add a flag next to `valve_id`:

```c
static int open_only;
```

Add an open handler above `valve_write`:

```c
static int valve_open(resmgr_context_t *ctp, io_open_t *msg,
                      RESMGR_HANDLE_T *handle, void *extra) {
  uid_t uid;
  if (open_only && !holds_ability(ctp, &uid)) {
    printf("valved: refused open from uid %d\n", (int)uid);
    return EPERM;
  }
  return iofunc_open_default(ctp, msg, handle, extra);
}
```

Make the write check skip itself in that mode:

```c
  uid_t uid;
  int ok = holds_ability(ctp, &uid);
  if (!ok && !open_only) {
    printf("valved: refused '%s' from uid %d\n", cmd, (int)uid);
    return EPERM;
  }
  printf("valved: accepted '%s' from uid %d\n", cmd, (int)uid);
```

In `main`, change the signature to `int main(int argc, char **argv)`, and add these lines:

```c
  open_only = argc > 1 && strcmp(argv[1], "-o") == 0;              /* first line of main */
  connect_funcs.open = valve_open;                                  /* after iofunc_func_init */
  printf("valved: serving /dev/valve%s\n", open_only ? " (checking at open only)" : "");
```

The last line replaces the old "serving" message. `valvectl --late-drop` opens the valve while it still holds the ability, then drops it and writes. Rebuild and copy `valved`, then try it against the open-only server first. Start `./valved -o` in the server session, and in the command session:

```sh
./valvectl --late-drop 2000
./valvectl --drop 1105
```

```text
valvectl: opened /dev/valve, then dropped demo.valve.write
valvectl: uid 2000: write accepted
valvectl: open: Operation not permitted
```

Server session:

```text
valved: demo.valve.write is ability 91
valved: serving /dev/valve (checking at open only)
valved: accepted 'open 40' from uid 2000
valved: refused open from uid 1105
```

Now press Ctrl-C, start plain `./valved`, and repeat the late drop:

```sh
./valvectl --late-drop 2000
```

```text
valvectl: opened /dev/valve, then dropped demo.valve.write
valvectl: uid 2000: write: Operation not permitted
```

Server session:

```text
valved: demo.valve.write is ability 91
valved: serving /dev/valve
valved: refused 'open 40' from uid 2000
```

With the open-only check, a process that no longer holds the ability still moved the valve through a descriptor it opened earlier. Checking on every message closes that gap. It matters whenever a descriptor can outlive the permission behind it: a process that drops privileges after start-up, or a descriptor inherited from a more trusted parent.

## What you proved

- QNX 8.0 refused root a custom ability that nobody had been given.
- A non-root process holding exactly `demo.valve.write` could write, and `pidin` showed it as `-NL-`.
- A process that denied and locked its abilities got `EPERM` from QNX, and couldn't get the ability back even though the call to do so returned `EOK`.
- A check at open alone let a descriptor outlive its permission, and a check on every write didn't.

## Exercises

1. **Guard reads too.** Create a second ability, `demo.valve.read`, add an `io_read` handler that reports the valve position, and give a monitoring client only the read ability.
2. **Drop the server's own privileges.** `valved` runs as root the whole time. After `resmgr_attach`, have it keep only what it still needs and switch to its own uid, then check it with `pidin`.
3. **Put a broker in front.** Write a third process that takes requests over `MsgSend`, checks them against limits (say, never more than 80 percent open) and is the only process holding `demo.valve.write`. Then the web process doesn't talk to the valve at all.
4. **Add a security policy.** QNX 8.0 can also assign security types at spawn through a policy (`secpolcompile`, `mkqnximage --secpol`). Read the SDP's security policy guide and give `valved` its own type.

## Troubleshooting

| What you see | Why | Fix |
|---|---|---|
| `valvectl: open: No such file or directory` | `valved` isn't running, so nothing has registered `/dev/valve` | Start `./valved` in the server session first |
| `valved: resmgr_attach: Operation not permitted` | `valved` was started as a non-root user and can't register a path | Run it as root |
| Root's write is refused | That's expected on QNX 8.0: root doesn't hold a new custom ability | Use `valvectl --keep UID` |
| `Memory fault (core dumped)` from the client | A `procmgr_ability` call without a `PROCMGR_AID_EOL` entry, so it reads past the end of its list | End every call's list with an entry that includes `PROCMGR_AID_EOL` |
| `scp: dest open ... Failure` when copying `valved` | The old `valved` is still running from that file | Stop it with Ctrl-C (or `slay valved`), then copy |
| `exec format error` on the target | Built for the wrong CPU | `make clean && make ARCH=aarch64le` for a Pi, plain `make` for x86_64 |

---

## See it in a full system: COLDKEEP

This codelab is COLDKEEP's security boundary, cut down to two files. COLDKEEP is a refrigeration supervisor in which the network-facing process is assumed to be compromised; the same pieces you just built keep it away from the plant.

| In this codelab | In COLDKEEP | Where |
|---|---|---|
| `valved` checks `demo.valve.write` on every write | `casectl` (one per refrigerated case) checks `coldkeep.plant.write` | `target/casectl/main.c` |
| `valvectl --keep` | `governord`, the safety governor, holds `coldkeep.plant.write` as uid 1103 (`-NL-`) | `target/governord/main.c` |
| `valvectl --drop` | `mgmtd`, the web API, runs as uid 1105 with every ability denied and locked | `ck_drop_to` in `target/common/ck_target.c` |
| Step 6's check on every message | `governord` checks `coldkeep.envelope.write` with `ConnectClientInfoAble` on every key approval | `target/governord/main.c` |
| Exercise 3's broker | `brokerd` sits between `mgmtd` and the governor | `target/brokerd/main.c` |

One difference: in COLDKEEP the governor creates the ability and the controllers check it, while here `valved` does both.

## Appendix A: a QEMU target in a few minutes

If you don't have a QNX 8.0 target, `mkqnximage` from the SDP builds a small QEMU image on a Linux host:

```sh
source ~/qnx800/qnxsdp-env.sh
mkdir qnx-vm && cd qnx-vm
mkqnximage --type=qemu --arch=x86_64 --hostname=valve --ssh-ident=$HOME/.ssh/id_ed25519.pub --build
```

On macOS the SDP's host tools run in Docker. COLDKEEP's `scripts/qemu-image.sh` and `scripts/qemu-run.sh` wrap the same command, add a fix for the stock image's `sshd`, and boot it headless with SSH on port 2222. The target in this codelab was built that way.

## Appendix B: build and attack COLDKEEP yourself

The full COLDKEEP walkthrough takes 60–90 minutes, mostly building images and booting. You need the SDP at `~/qnx800`, Docker Desktop, Homebrew `qemu` and Node 20+.

**Host only, no QNX.** `git clone https://github.com/wendoj/coldkeep.git && cd coldkeep && make test && make demo` runs the governor's unit and property tests, then the host simulator with the same attacks sent to a legacy store and a protected one.

**Two stores in QEMU.**

```sh
bash docker/setup.sh
scripts/qemu-image.sh ~/coldkeep-qemu/store-a store-a
scripts/qemu-image.sh ~/coldkeep-qemu/store-b store-b
scripts/qemu-run.sh ~/coldkeep-qemu/store-a --slot 0 --headless   # protected, ssh -p 2222
scripts/qemu-run.sh ~/coldkeep-qemu/store-b --slot 1 --headless   # legacy, ssh -p 2223
scripts/qnx-build.sh target
scripts/deploy.sh 0 protected && scripts/deploy.sh 1 legacy
```

**The attack from inside the web process (A3).** On the protected store, as `mgmtd`'s uid:

```sh
on -u 1105 sh -c 'echo setpoint 30 | dd of=/dev/coldkeep/case/1 2>&1'
on -u 1105 sh -c 'cat /dev/coldkeep/gov 2>&1'
on -u 1105 sh -c ': >> /data/coldkeep/audit.bin 2>&1'
```

```text
dd: /dev/coldkeep/case/1: write error: Operation not permitted
cat: /dev/coldkeep/gov: Permission denied
sh: cannot create /data/coldkeep/audit.bin: Permission denied
```

On the legacy store, where `mgmtd` is root, the controller write and the journal write go through.

**The dashboard and the key console.** Start the dashboard and a relay with `--mgmt` (commands in the [README](../README.md#dashboard)), then open **Control**. Setpoint +25 °C is applied on the legacy store and denied (G1) on COLDKEEP. A 24-hour defrost is clamped to 45 minutes (G3). Alarms off becomes a key request (G5), and the dashboard has no button to approve it. Approve it on the protected store with `cat /dev/coldkeep/key` and `echo "approve <id>" > /dev/coldkeep/key`; as uid 1105 the same write gets `Permission denied`.

**Crash and freeze the governor (A4).** `slay -s KILL governord`, then `grep 'governord restarted' /data/coldkeep/log/sup.log`: the supervisor restarted it in 29.6 ms on average over the recorded runs, and it still remembers recent defrosts. `slay -s STOP governord; sleep 5; slay -s CONT governord` puts all 8 cases into a safe hold (G6) and releases them afterwards.

**Record your own comparison.** `scripts/run-compare.py --label mylab` then `scripts/summarize-runs.py recordings/mylab`, and compare with [RESULTS.md](RESULTS.md). QEMU timing follows the host clock, so expect close numbers, not identical ones.
