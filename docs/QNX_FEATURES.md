# QNX features — verified on the target

Every answer below comes from a command run on QNX 8.0 (`QNX store-a 8.0.0 2026/06/05 x86pc x86_64`)
in QEMU on macOS, on 2026-11-09. Raw output: [`evidence/qnxcheck-store-a.txt`](evidence/qnxcheck-store-a.txt)
and [`evidence/qnxcheck-store-a-after-reboot.txt`](evidence/qnxcheck-store-a-after-reboot.txt).

## Answers

| # | Question | Answer | Evidence |
|---|---|---|---|
| 1 | Can a process create a custom ability, and can a resource manager reject a client lacking it? Exact 8.0 signatures? | **Yes.** `procmgr_ability_create("coldkeep.check.write", 0)` returned ability id 91. Another process found the same id with `procmgr_ability_lookup()`. In the resource manager, `iofunc_client_info_able(ctp, 0, &info, 0, &able, 1)` sets `able.flags` to `0x1` when the client holds the ability and `0x0` when it doesn't. Returning `EPERM` from `io_write` makes the client's `write()` fail with `EPERM`. **Root does not hold a new custom ability by default** (`flags=0x0` for a root client), so the process that needs it must grant it to itself explicitly. | Q1/Q2 tests 1–4; `qcc -Vgcc_ntox86_64 -Wall` builds `able_server.c` with no warnings |
| 2 | Does `procmgr_ability(... LOCK \| EOL)` behave as documented for a non-root process? | **Yes, for the calls we use.** As root: `procmgr_ability(0, NONROOT\|DENY\|LOCK\|EOL)` (optionally preceded by `NONROOT\|ALLOW\|LOCK\|<custom id>`) returns 0, then `setgid`/`setuid(2000)` succeed. After that, as non-root, `procmgr_ability(0, ROOT\|DENY\|LOCK\|EOL)` also returns 0. A process that keeps the custom ability can still use it after dropping root (test 3: accepted); one that doesn't keep it is refused (tests 2 and 5). `pidin -p <pid> abilities` lists every ability with a flags column (e.g. `R-L-`, `--L-` for the dropped custom ability). **Open:** what each flag letter means is not yet checked against the `pidin` docs. | Q1/Q2 tests 2, 3, 5 |
| 3 | Is security-policy enforcement available (procnto option, tools, assigning a type at spawn)? | **Available, but not enabled by default, and it needs a policy file.** The default image runs `procnto-smp-instr -mr -d 0777 -u 0777` with no policy. The SDK ships `secpolgenerate`, `secpolpush`, `secpollaunch`, `secpolmonitor` and `secpol` for the target, and `secpolcompile` on the host. `mkqnximage --secpol=develop\|open\|secure` adds a `secpolpush` step at boot that loads `/proc/boot/secpol.bin`; that file comes from `--policy=<file>`. Built with `--secpol=develop` and no `--policy`, the image fails at boot (`on: Unable to open policy file /proc/boot/secpol.bin`; `/boot`, `/system`, `/data` fail to mount; no shell). Start-up snippets launch services with `START(type)` / `STARTU(type, uid)` macros, which is how types are assigned at spawn. **Next:** write a COLDKEEP policy and build with `--secpol=develop --policy=<file>`. | Q3 section; second image `secpol-test` boot log |
| 4 | Is `ham` present? | **No.** Not on the target (`which ham hamctrl` finds nothing, no `ham` process), and no `ham`/`hamctrl`/`libham` in the installed SDP. Use the `ck-sup` supervisor. | Q4 section; search of `~/qnx800/target` |
| 5 | Writable persistent path for the journal | **`/data`** (qnx6 on `/dev/hd0t179`). Markers written to `/data`, `/data/var`, `/data/home`, `/var` and `/tmp` all survived a reboot. The default data partition is 61 MB; `mkqnximage --data-size=<MB>` changes it. | Q5 section, before and after reboot |
| 6 | Two instances at once on the Mac; port forwarding for 7070/8080 | **Yes.** `scripts/qemu-run.sh <image> --slot 0` and `--slot 1` ran two VMs at the same time with QEMU user-mode networking. Each slot gets its own forwards: SSH 2222/2223, management 18080/18081 → 8080, telemetry 17070/17071 → 7070, serial console 4440/4441. Each VM has 2 vCPUs and 1 GB, emulated (`-cpu max`; no hardware acceleration for x86 on Apple Silicon), and reaches "Startup complete" in about 6 s. | `lsof` listing of both instances' ports; SSH to `store-a` while slot 1 was running |
| 7 | Adaptive partitioning in free SDP 8.0? | **Not in this install.** No `aps` on the target and none in the installed SDP. Use real-time priorities only. | Q7 section; search of `~/qnx800/target` |

## Reproducing

1. Once: `bash docker/setup.sh` (creates the `qnx-build` container).
2. Build an image and boot it:
   `scripts/qemu-image.sh ~/coldkeep-qemu/store-a store-a` then `scripts/qemu-run.sh ~/coldkeep-qemu/store-a --headless`.
3. Build and copy the checks:
   `scripts/qnx-build.sh target/qnxcheck`, then
   `QNX_USER=root QNX_PORT=2222 QNX_SSH_OPTS="-o UserKnownHostsFile=$HOME/coldkeep-qemu/known_hosts" scripts/qnx-deploy.sh 127.0.0.1 target/qnxcheck able_server able_client run-checks.sh`.
4. Run them: `ssh -p 2222 root@127.0.0.1 'cd ckcheck && sh run-checks.sh'`; reboot the VM and run `sh run-checks.sh persist-check`.

## How A3 is demonstrated

The compromised-web-process scenario is shown from a QNX shell running with the
management service's uid (for example `on -u <mgmt-uid> sh -c 'echo x > /dev/coldkeep/case/1'`),
so every errno in the demo comes from QNX itself. Check 4 above already shows the
mechanism: `on -u 2000 sh -c 'echo x > /dev/ckcheck'` is refused with `EPERM`.
