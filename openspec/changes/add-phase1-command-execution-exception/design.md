# Design: Phase 1 Command Execution Exception

## Technical Approach

### Decision: what moves up, what doesn't — narrowly scoped, per direct confirmation

Only two things move from Phase 2 into Phase 1: real command
execution, and `toolset.push`. Everything else currently scheduled
for Phase 2 — RBUS/Thunder mapping, sandbox profile authoring
workflow, footprint sizing, RDK-V namespace mapping, the concluding
NFR-3/NFR-4 security review, and the on-demand spawn mechanism itself
— stays there, unchanged. This was a deliberate choice among options
(the alternative considered was merging Phase 1 and Phase 2 entirely)
— narrow scope was chosen specifically so this stays a bounded,
reviewable exception rather than an undoing of the whole phased plan.

### Decision: in-process execution for Phase 1, as a tracked exception — not sandboxing itself relaxed in general

`define-plane-vs-toolset-model`'s Decision B (every toolset
out-of-process and sandboxed, no exemption) is not reopened as a
general matter. What's granted here is a scoped, temporary exception
for exactly the toolset(s) Phase 1's command execution and
`toolset.push` touch: they run in-process for this phase. This is the
explicit-exception path that document's own text reserved, exercised
now rather than discovered as a silent gap later. The hardening this
exception defers — moving Phase 1's command-executing toolset(s) to
the same out-of-process, sandboxed, on-demand-spawned model every
other toolset already requires — is Phase 2 work, using the model
`define-on-demand-toolset-execution` already designed. Nothing about
that design changes; it simply doesn't apply to Phase 1's toolset(s)
yet.

### Decision: authorization is not part of what's deferred

Running in-process defers sandboxing (a containment control), not
authorization (a permission control) — the two are independent, and
conflating them would be a real security regression, not just a
convenient scope cut. Dispatch Core's single ACL checkpoint (FR-4,
`dispatch-core/spec.md`) applies to Phase 1's real commands exactly as
it will apply once Phase 2's out-of-process model lands — same
checkpoint, same ACL Policy Store query, same deny-first/write-implies-
read semantics. The technical review and reference-impl sketch already
written for this (`docs/20_acl_implementation_rpcd_technical_review_and_dispatch_core_design.md`,
`reference-impl/dispatcher_command_path.c`, `reference-impl/toolset_resolution.c`)
were written generically enough that they don't assume out-of-process
execution at the resolution/authorization layer — only step 3
("ensure reachable," spawn-on-demand) assumes a separate process to
spawn. For Phase 1, that step is a no-op: the toolset is already
loaded in-process, so "ensure reachable" trivially succeeds without
invoking `define-on-demand-toolset-execution`'s spawn logic at all.

### Decision: `toolset.push`'s RDM boundary is unchanged — only where the resulting code runs changes

`define-synchronous-toolset-push`'s type-based boundary (A6) still
holds without modification: RDM Client handles any `dlopen()`-able
binary regardless of size; `toolset.push` still never carries one.
Running Phase 1's toolsets in-process does not mean Dispatch Core
itself starts `dlopen()`-ing pushed binaries directly, and does not
change which delivery mechanism is responsible for which artifact
type. What changes is purely where already-verified, already-delivered
toolset code executes once loaded — in-process for Phase 1, a separate
sandboxed process once Phase 2 lands. `toolset.push`'s own open
question (C8 — what `params.artifact` actually carries) is unaffected
by this decision and remains open, now more urgent since Phase 1 will
actually implement against it rather than just design it.

### Decision: a Phase-1-specific rollback model for `toolset.push`

`define-on-demand-toolset-execution` amended `toolset.push`'s rollback
behavior to retain the prior *artifact* as a fallback and spawn it
fresh on the next demand — a model that assumes out-of-process, on-
demand execution. Phase 1 doesn't have that yet, so it needs its own
rollback behavior, closer to `define-synchronous-toolset-push`'s
*original* design: the prior in-process version stays loaded and
serving until the new version passes its health check, then Plugin
Manager swaps which version actually handles calls. This is Phase-1-
only — it is retired, not merged with, the artifact-fallback model
once Phase 2's out-of-process hardening lands. Two different
mechanisms for two different phases, tracked as separate, not one
silently evolving into the other.

### Decision: encryption is explicitly not pulled forward

Per direct confirmation, payload encryption stays exactly where A7
already put it — Phase 2. Phase 1's real commands and `toolset.push`
messages run as plain JSON-RPC with the `static`/`dynamic` type field
and mandatory signature/manifest verification, same as Phase 1's
original read-only discovery scope. This was a live option considered
(bringing encryption forward alongside command execution, since real
commands and pushed code are more sensitive than a read-only query)
and explicitly declined — confidentiality is deferred, authenticity
and authorization are not.

### Consequence: what becomes urgent that wasn't before

Three previously-Phase-2-paced items become Phase 1 blockers because
Phase 1 now does real, authorized execution rather than read-only
discovery:
- **A1 (SAT token format)** — real command execution needs real
  caller authorization; it can no longer stay "drafted, unarchived"
  indefinitely the way Phase 1's original read-only scope allowed.
- **C8 (`toolset.push`'s `params.artifact` field)** — Phase 1 will
  actually build against `toolset.push`, not just design it.
- **`docs/20`'s ACL design (B5/B6)** — needs actual implementation now,
  not just the technical review and illustrative sketch that already
  exist.

## File/Component Changes

- `toolset-lifecycle/spec.md`: Phase-1-scoped exception to the
  "dynamically-pushed toolsets run out-of-process" requirement.
- `sandboxed-runtime/spec.md`: Phase-1-scoped exception to the uniform
  out-of-process/sandboxing requirement, explicitly bounded to Phase
  1's command-executing toolset(s) — not a general reopening.
- `add-triage-skillset-mapping-phase1/proposal.md`: note that real
  command execution is now a separate, additional Phase 1 deliverable
  (this change), not a rescoping of that change's own read-only
  triage-discovery scope.
- `define-synchronous-toolset-push/design.md`: Phase-1-specific
  rollback note, cross-referenced from here.
- `ROADMAP.md`: Phase 1 gains real command execution and `toolset.push`;
  Phase 2's corresponding entries are reframed as "hardens Phase 1's
  exception," not removed.
