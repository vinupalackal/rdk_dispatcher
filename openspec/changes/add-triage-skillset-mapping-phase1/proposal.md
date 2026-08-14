# Proposal: Phase 1 — Triage Skillset Mapping over WRP

## Intent

Narrow the first implementation phase of the RDK Dispatcher to one
concrete, shippable slice: the device answers a cloud-initiated
"what triage capabilities do you have" request, sourced from both
statically compiled-in and dynamically (`dlopen()`) loaded triage
plugins, carried end-to-end in WRP with a JSON payload. This closes
part of the "Triage as a fifth OpenSpec domain" open thread noted in
`CLAUDE_CODE_WORKFLOW.md`, scoped down to exactly what's needed for
this phase rather than the full triage plane (evidence capture,
async enqueue, MQTT/telemetry export are explicitly deferred — see
Scope below).

## Scope

In scope:
- A new `triage` OpenSpec domain, seeded only with the requirements
  this phase needs (not a full port of doc 18's triage discussion).
- The WRP request/response envelope shape for a skillset-mapping
  query (§ design.md).
- The rule that the response merges capabilities from both static
  (compiled-in) and dynamic (`.so`, `dlopen()`-loaded) triage
  plugins, tagging each entry with which kind it is.
- Reusing FR-1's existing JSON-RPC 2.0 framing (`dispatch-core/spec.md`)
  as the WRP payload's content, rather than inventing a second,
  parallel request format.

Out of scope (deferred to a later change):
- Triage evidence capture, `trace_id` correlation, async enqueue —
  the `run_with_timeout()` / `triage_enqueue_async()` behavior already
  sketched in `reference-impl/plugins/triage_wifi.c` for *handling* a
  triage event. This phase only covers *advertising what triage
  toolsets exist*, not executing them.
- Full command execution through Execution Framework / Sandboxed
  Toolset Plugin Runtime (FR-13/FR-14) for the triage plane — this
  phase's response is read-only self-description, not a command path.
  **Note, 2026-08-14:** real command execution and `toolset.push` are
  now separately in scope for Phase 1 overall, via
  `add-phase1-command-execution-exception` — but that's an additional
  Phase 1 deliverable alongside this one, not a rescoping of *this*
  change. `triage.capabilities` stays a read-only query; it doesn't
  gain a command-execution path itself.
- Any change to `capability-sync/spec.md`'s existing event-triggered,
  device-initiated push model — this phase adds a distinct,
  cloud-initiated **pull**, not a replacement (see design.md, "Relation
  to capability-sync").

## Why this is a good first phase

It exercises the full vertical slice — WRP transport, JSON-RPC framing,
Dispatch Core's single ACL checkpoint, Plugin Manager's coarse registry,
Schema & Discovery's `capabilities()`, and the static/dynamic plugin
loader already sketched in `reference-impl/dispatcher_core.c` — without
requiring the Sandboxed Toolset Plugin Runtime, ACL Policy Store hot
reload, or RDM Client install pipeline to exist first. It's read-only,
so a bug here can't mutate device state, which makes it a safe
first thing to build and demo end-to-end.
