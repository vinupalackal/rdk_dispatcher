# Tasks

## 1. Spec updates
- [x] 1.1 `toolset-lifecycle/spec.md`: add a Phase-1-scoped exception to
      the "dynamically-pushed toolsets run out-of-process" requirement
- [x] 1.2 `sandboxed-runtime/spec.md`: add a Phase-1-scoped exception to
      the uniform out-of-process/sandboxing requirement, explicitly
      bounded (Phase 1's command-executing toolset(s) only, not a
      general reopening of Decision B)
- [ ] 1.3 `dispatch-core/spec.md`: confirm (not add — already true)
      that the single ACL checkpoint requirement applies
      unconditionally, including to Phase 1's in-process exception
- [x] 1.4 `add-triage-skillset-mapping-phase1/proposal.md`: note that
      real command execution is now a separate, additional Phase 1
      deliverable (this change), not a rescoping of that change's own
      read-only triage-discovery scope

## 2. Rollback model for Phase 1
- [x] 2.1 Define the Phase-1-specific `toolset.push` rollback
      behavior: prior in-process version stays loaded and serving
      until the new version passes health check, then Plugin Manager
      swaps which version handles calls — closer to
      `define-synchronous-toolset-push`'s original design than
      `define-on-demand-toolset-execution`'s artifact-fallback model,
      since there's no separate process to spawn yet
- [ ] 2.2 Confirm this Phase-1 rollback model is retired (not merged
      with) once Phase 2's out-of-process/on-demand hardening lands —
      two different mechanisms for two different phases, not one
      evolving into the other silently

## 3. Verification against spec
- [ ] 3.1 Confirm scenario: a real command executed in Phase 1 still
      passes through Dispatch Core's single ACL checkpoint, identically
      to how Phase 2's out-of-process model will enforce it
- [ ] 3.2 Confirm scenario: `toolset.push` in Phase 1 still rejects an
      artifact carrying a `dlopen()`-able binary (RDM boundary, A6,
      unchanged)
- [ ] 3.3 Confirm scenario: a Phase 1 command request is still plain
      JSON-RPC with mandatory verification, unencrypted (A7 unchanged)

## 4. Follow-up now more urgent
- [ ] 4.1 A1 (SAT token format) needs confirming — real command
      execution requires real caller authorization, no longer
      deferrable behind Phase 1's read-only scope
- [ ] 4.2 C8 (`toolset.push`'s `params.artifact` field) needs
      resolving — Phase 1 will actually build against this, not just
      design it
- [ ] 4.3 `docs/20`'s ACL implementation design (B5/B6) needs actual
      implementation now, not just design — it's now Phase 1 required
      work
- [ ] 4.4 `reference-impl/dispatcher_command_path.c`'s step 3 ("ensure
      reachable") needs a Phase-1 no-op path documented explicitly —
      currently only implies spawn-on-demand behavior
