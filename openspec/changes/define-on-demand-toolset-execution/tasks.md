# Tasks

## 1. Spec updates
- [ ] 1.1 `toolset-lifecycle/spec.md`: replace "persistent, supervised unit" language with on-demand spawn + idle-timeout teardown
- [ ] 1.2 `toolset-lifecycle/spec.md`: revise health-check requirement to spawn-time confirmation, not periodic polling
- [ ] 1.3 Amend `define-toolset-as-mcp-tool-model/design.md`'s "persistent, supervised unit" decision with a superseding pointer here
- [ ] 1.4 Amend `define-synchronous-toolset-push/design.md`'s rollback decision: prior artifact retained as fallback, not prior process kept resident

## 2. Mechanism confirmation (not settled by this draft)
- [ ] 2.1 Confirm idle-timeout spawn as the mechanism (recommended) vs. pure per-call fork/exec (simpler, smaller footprint, higher per-call latency) — real tradeoff, needs your sign-off
- [ ] 2.2 Once 2.1 is confirmed, define the idle-timeout duration — a tuning parameter, likely needs real measurement against B11's latency data, not a guessed constant

## 3. Verification against spec
- [ ] 3.1 Confirm scenario: an idle toolset with no pending calls holds no resident process
- [ ] 3.2 Confirm scenario: a burst of related calls to the same toolset within the idle window reuses one spawned process rather than re-spawning per call
- [ ] 3.3 Confirm scenario: a failed on-demand health check on a newly pushed version falls back to spawning the prior artifact, without requiring both versions ever running simultaneously
- [ ] 3.4 Re-confirm B3's 300KB figure against a concurrently-active-only interpretation, now that "everything installed" no longer means "everything resident"
