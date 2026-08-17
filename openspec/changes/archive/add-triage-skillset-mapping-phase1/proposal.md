# Proposal: Phase 1 — Triage Skillset Mapping over WRP

**APPLIED 2026-08-16.** This change's delta is merged into the new
`openspec/specs/triage/spec.md` domain file. Applied after
`define-toolset-as-mcp-tool-model` (a prerequisite — this change's
`tools/list` references depend on that one's merge). This directory is
retained as historical record of the design and correction reasoning;
`openspec/specs/triage/spec.md` is the current source of truth going
forward.

**Corrected 2026-08-16.** `define-toolset-as-mcp-tool-model/tasks.md`
§4 flagged this change as needing two corrections before it could be
applied. Re-checking both: the first (`define-plane-vs-toolset-model/tasks.md`
§3, summarized elsewhere as "triage is a plane, not a toolset") turned
out to already be satisfied — that change's own later addendum
confirms a dedicated Triage Toolset is architecturally valid, no
change needed on that point. The second — this change invented a
bespoke `triage.capabilities` JSON-RPC method for discovery, before
`define-toolset-as-mcp-tool-model` established `tools/list` as the
generic mechanism for the same question — did need fixing, and is
fixed here: discovery now goes through `tools/list`, not a
triage-specific method. See `design.md` for the full before/after and
`specs/triage/spec.md` for the corrected requirements.

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
- The Triage Toolset's internal static/dynamic plugin merge, and how
  that data reaches the cloud: via `tools/list`
  (`define-toolset-as-mcp-tool-model`'s generic discovery mechanism),
  **not** a bespoke `triage.capabilities` method — corrected
  2026-08-16, see above.
- The rule that the Triage Toolset's internal capability descriptor
  set merges both static (compiled-in) and dynamic (`.so`,
  `dlopen()`-loaded) triage plugins, tagging each entry with which
  kind it is.
- How that per-plugin descriptor detail (`load_type`, `version`,
  `events`, `timeout_ms`) is projected into `tools/list`'s coarse,
  per-toolset `inputSchema` shape — flagged in `design.md` as a real,
  not-fully-settled design point (vendor-extension `x-rdk-*` schema
  properties), not assumed solved by this correction.

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
  change. Triage's `tools/list` discovery entry (corrected 2026-08-16,
  no longer a `triage.capabilities` method) stays a read-only listing;
  it doesn't gain a command-execution path itself.
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
