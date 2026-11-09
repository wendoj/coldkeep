# QNX features — questions to verify on the target

Record each answer with the doc link or the exact command used and its output. Do not fill these in from memory.

| # | Question | Answer | Evidence |
|---|---|---|---|
| 1 | Can a process create a custom ability, and can a resource manager reject a client lacking it with `iofunc_client_info_able()`? Exact 8.0 signatures? | TBD | |
| 2 | Does `procmgr_ability(... LOCK \| EOL)` behave as documented for a non-root process? | TBD | |
| 3 | Is security-policy enforcement available in the QSTI (procnto option, `secpolgenerate`/`secpolpush`, assigning a type at spawn)? | TBD | |
| 4 | Is `ham` present in the QSTI? (Otherwise use `ck-sup`.) | TBD | |
| 5 | Writable persistent path for the journal | TBD | |
| 6 | Two QSTI instances at once on the Mac; port forwarding for 7070/8080 | TBD | |
| 7 | Adaptive partitioning in free SDP 8.0? | TBD | |
