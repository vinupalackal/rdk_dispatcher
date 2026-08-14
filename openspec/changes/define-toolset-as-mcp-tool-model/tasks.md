# Tasks

## 1. Spec updates
- [ ] 1.1 `dispatch-core/spec.md`: add MCP method-surface requirement (`initialize`/`tools/list`/`tools/call`/`notifications/tools/list_changed` over existing JSON-RPC/WRP transport)
- [ ] 1.2 `dispatch-core/spec.md`: add requirement that `tools/call` uses the identical ACL/Execution Framework/sandboxing path as any other command
- [ ] 1.3 `toolset-lifecycle/spec.md`: add requirement mapping toolset `schema()` output to MCP tool definitions
- [ ] 1.4 `toolset-lifecycle/spec.md`: add requirement that dynamically-pushed toolsets run as an out-of-process, persistent, supervised unit — not `dlopen()`'d, not fork-per-call
- [x] 1.5 ~~`capability-sync/spec.md`: add a note distinguishing `notifications/tools/list_changed` (live MCP session) from the existing device-identity-authenticated push (Device Model Mapping/Tool Catalog) — same trigger, different consumers~~ — done, see that file's "Distinct from MCP's live tool-change notification" requirement, strengthened 2026-08-13 with the shared-emission-point clause below

## 2. Push/discovery mechanism
- [ ] 2.1 Define the toolset manifest fields needed for the persistent-process push model (entrypoint, supervision/restart policy, health-check contract)
- [ ] 2.2 Confirm RDM Client's existing verify-before-install step (FR-12) is the gate before Plugin Manager registration, for this mechanism specifically, not just in the abstract

## 1a. New, from the shared-trigger-point revision (2026-08-13)
- [ ] 1a.1 Implement Plugin Manager's single internal
      `toolset_changed` emission function that both the
      capability-sync push and `notifications/tools/list_changed`
      subscribe to — no other code path may trigger either delivery
      directly
- [ ] 1a.2 Confirm the two downstream queues (durable catalog push,
      live session notification) share no retry/backoff logic and no
      ordering dependency — a slow or failing catalog push must not
      delay or block the live notification, and vice versa
- [ ] 1a.3 Add a regression test: a reload event with the cloud
      catalog backend unreachable still delivers
      `notifications/tools/list_changed` to any connected session
      without waiting on the catalog push's retry cycle

## 2a. New, from the per-toolset `tools/list` granularity revision (2026-08-13)
- [ ] 2a.1 Confirm the `oneOf` discriminated-union schema stays legible
      once a real toolset accumulates enough methods to test it — the
      granularity change trades catalog size for per-tool schema
      complexity, not decided to be a strict improvement (see
      `design.md`'s trade-off note)
- [ ] 2a.2 Update `add-triage-skillset-mapping-phase1` (already
      pending correction per §4 below) to reflect the coarser
      granularity once applied — its `tools/list` filtering-by-plane
      approach needs to project one `triage` tool entry, not one per
      triage method

## 3. Verification against spec
- [ ] 3.1 Confirm scenario: `tools/call` denied by ACL exactly as a non-MCP JSON-RPC call to the same method would be
- [ ] 3.2 Confirm scenario: an unverified pushed toolset never appears in `tools/list`
- [ ] 3.3 Confirm scenario: `tools/list` reflects a toolset's current `schema()` immediately after reload, with no separately-maintained MCP-specific copy drifting out of sync

## 4. Required follow-up: second correction to Phase 1
- [ ] 4.1 `add-triage-skillset-mapping-phase1` already needs correction per `define-plane-vs-toolset-model/tasks.md` §3 (triage is a plane, not a toolset). Layer this correction on top: once corrected, triage-plane capability discovery should be exposed by filtering/tagging `tools/list` output by plane, not by a separate `triage.capabilities` method
- [ ] 4.2 Do not apply `add-triage-skillset-mapping-phase1` until both `define-plane-vs-toolset-model` §3 and this section are complete
