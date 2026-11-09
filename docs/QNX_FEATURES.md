# QNX features — questions to verify on the target

Record each answer with the doc link or the exact command used and its output. Do not fill these in from memory.

## How to answer them

1. Once: `bash docker/setup.sh` (creates the `qnx-build` container). Then build the checks: `scripts/qnx-build.sh target/qnxcheck`.
   If it does not compile, the compiler errors answer question 1's "exact signatures" part.
2. Copy them: `scripts/qnx-deploy.sh <target-ip> target/qnxcheck able_server able_client run-checks.sh`.
3. On the target, as root: `cd ~qnxuser/ckcheck && sh run-checks.sh > results.txt 2>&1`.
4. Reboot the target, then `sh run-checks.sh persist-check > results2.txt 2>&1` (question 5).
5. Question 6 is answered on the Mac: start a second QEMU instance from a copy of the image folder.

## How A3 is demonstrated

The compromised-web-process scenario is shown from a QNX shell running with the
management service's uid (for example `on -u <mgmt-uid> sh -c 'echo x > /dev/coldkeep/case/1'`),
so every errno in the demo comes from QNX itself.

| # | Question | Answer | Evidence |
|---|---|---|---|
| 1 | Can a process create a custom ability, and can a resource manager reject a client lacking it with `iofunc_client_info_able()`? Exact 8.0 signatures? | TBD | |
| 2 | Does `procmgr_ability(... LOCK \| EOL)` behave as documented for a non-root process? | TBD | |
| 3 | Is security-policy enforcement available in the QSTI (procnto option, `secpolgenerate`/`secpolpush`, assigning a type at spawn)? | TBD | |
| 4 | Is `ham` present in the QSTI? (Otherwise use `ck-sup`.) | TBD | |
| 5 | Writable persistent path for the journal | TBD | |
| 6 | Two QSTI instances at once on the Mac; port forwarding for 7070/8080 | TBD | |
| 7 | Adaptive partitioning in free SDP 8.0? | TBD | |
