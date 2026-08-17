# Tasks

## 1. Spec updates
- [ ] 1.1 `toolset-lifecycle/spec.md`: register `diagnostics` as a named, concrete instance of the Phase 1 in-process exception (`add-phase1-command-execution-exception`), with its static/dynamic tool-typing and permanent legacy-msgpack-framing specifics
- [ ] 1.2 `dispatch-core/spec.md`: add the scoped, tracked, time-bounded exception for diag-server's interim self-hosted ACL checkpoint, naming its own retirement condition (a real Dispatch Core process forwarding `diagnostics` traffic)
- [ ] 1.3 Cross-reference `capability-sync/spec.md` and `acl-policy-store/spec.md` from this change's spec deltas — no deltas needed there, diag-server is a new caller of already-generic mechanisms, not new behavior; confirm this holds by re-reading both specs against `diag_notify_capability_sync()`/`diag_acl_check()`'s actual call shape (done during drafting — see design.md)

## 2. Reconciliation work this change surfaces but does not itself complete
- [ ] 2.1 Once a real Dispatch Core process exists and forwards `diagnostics` traffic over the local endpoint, retire the `dispatch-core` exception added in 1.2 and remove `diag_acl_check()`'s independent enforcement role (keep the call as defense-in-depth or remove it — decide at that time, not here)
- [ ] 2.2 Resolve `acl_policy_store_query()`'s transport (`docs/24` §14 item 4, Phase 2) — blocks `diag_acl_check()` from linking into a runnable binary, project-wide, not diagnostics-specific
- [ ] 2.3 Resolve A1 / SAT token format (`docs/24` §14 item 6, Phase 2) — blocks `caller_identity_t`'s real shape and Phase D.2's ACL-denial integration test
- [ ] 2.4 Decide whether `DIAG_LOCAL_RECV_URL`/`DIAG_LOCAL_SEND_URL`'s addresses need to change once Dispatch Core's real side is built (flagged in `docs/24` §15 B.4 part 1's implementation notes as a "flag for Phase C" item — `reference-impl/diag_legacy_framing.c`'s illustrative single-address sketch is already known to be stale against the real two-address split)

## 3. Verification against spec
- [ ] 3.1 Confirm scenario: a static tool's caller-supplied `command` override is discarded regardless of content, and the catalog's own command runs (already verified: `test_init_validation.c`, 17/17, plus D.1's harness, 86/86 — re-run against this change's spec text once written, not just against the code)
- [ ] 3.2 Confirm scenario: a dynamic tool's caller-supplied command is still gated only by the blocklist, unaffected by the static-tool policy change
- [ ] 3.3 Confirm scenario: a PUSH arriving on the public (non-local) socket is rejected with `PUSH_ERR_FORBIDDEN_TRANSPORT` before any decode happens (already verified: B.4 harness, 9/9)
- [ ] 3.4 Confirm scenario: a denied ACL check short-circuits before catalog lookup/execution, returning `exit_code=126`/`stdout="access denied"` (already verified: §13.4 harness, 24/24) — re-confirm this is what the new `dispatch-core` exception delta's scenario text describes, once §1.2 is written
- [ ] 3.5 Confirm scenario: `capability_sync.updated` fires after a successful PUSH promotion, over the public/Parodus socket, with the shape `capability-sync/spec.md` expects (already verified: §13.4 harness's capability-sync checks; D.3 harness's capability-count check)
