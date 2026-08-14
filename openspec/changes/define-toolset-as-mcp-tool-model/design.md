# Design: Toolsets as MCP Tools

## Technical Approach

### Decision: MCP sits on top of FR-1's existing JSON-RPC 2.0 framing, not a new transport

MCP's own wire format is JSON-RPC 2.0. `initialize`, `tools/list`,
`tools/call`, and `notifications/tools/list_changed` are simply
JSON-RPC methods/notifications carried over the same WRP/XMiDT/Parodus
transport FR-1 already requires. No new cloud-facing transport is
introduced.

### Decision: `tools/list` aggregates across every currently loaded toolset — one MCP tool per toolset, not per method

**Revised 2026-08-13 by direct confirmation, replacing the original
per-method granularity below.** Each `tools/list` entry is one
toolset: `name` = `<toolset>` (e.g. `"wifi"`, not
`"wifi.setChannel"`). `description` still comes from the toolset's own
`schema()`. `inputSchema` becomes a discriminated union across the
toolset's methods — a JSON Schema `oneOf` array, one branch per
method, each branch requiring a `method` field with a `const` value
naming that method plus a `params` sub-schema matching that method's
own argument shape. An MCP client calling `wifi` sends
`{"method": "setChannel", "params": {"channel": 6}}` as `arguments`,
not a bare parameter object. This directly answers
`OPEN_QUESTIONS.md` C5 (now folded into this change rather than
tracked separately, since it's the same decision this section already
owned) — deliberately coarser than one tool per method, to avoid a
`tools/list` catalog that grows one flat entry per method as toolsets
add capabilities, closer to how some MCP servers keep their top-level
tool count bounded.

Sourcing is otherwise unchanged: Dispatch Core still asks Plugin
Manager for the list of loaded toolsets and each toolset's own process
for its schema; nothing is centrally duplicated — only the shape the
aggregated schema is projected into changed, from N flat tool entries
per toolset to one entry with an internally-structured schema.

**What does *not* change:** ACL Policy Store's granularity. Access
control was never keyed off the MCP tool *name* string — the query
API (`acl_policy_store_query(caller, toolset, method)`,
`reference-impl/dispatcher_command_path.c`) already takes toolset and
method as two separate values, extracted from wherever the request
put them. Under the old convention that was a split of the dotted
tool name; under this one it's `name` plus `arguments.method`. Either
way, permission scoping stays exactly as granular as it always was —
per method, not coarsened to per-toolset just because the *catalog
listing* got coarser. This is worth stating explicitly because it
would have been an easy, wrong assumption: coarsening the MCP surface
does not mean coarsening what a permission grant actually covers.

**Trade-off this introduces, named rather than left implicit:** a
smaller `tools/list` is friendlier to an agent's context budget and
avoids an ever-growing flat catalog, but it also means an MCP client
can no longer identify a specific capability (`wifi.setChannel`) by
tool name and description alone — it has to inspect the toolset's
`oneOf`-composed `inputSchema` to discover which methods exist and
what each one needs. This is a real cost for MCP clients that reason
about capabilities primarily from flat tool name/description pairs,
not a strict improvement over the original per-method model — flagged
in `tasks.md` as worth watching once real toolsets have enough
methods to test whether the discriminated-union schema stays legible.

**Original per-method granularity, superseded above:** the original
version of this decision made `name` = `"<toolset>.<method>"`, one
`tools/list` entry per method, `inputSchema` matching that single
method's own argument shape directly. Simpler to project and to
consume, but grows the catalog linearly with total method count across
every loaded toolset — the concern this revision addresses.

### Decision: `tools/call` is not a second command path

`tools/call` carries `(toolset, method, args)` through the identical
route §5.1 already specifies: Dispatch Core's single ACL checkpoint,
Plugin Manager resolution, Execution Framework decode/validate,
sandboxed toolset execution, Platform Adapter identity forwarding.
`toolset` comes from the MCP `name` field, `method` and `args` are
unpacked from the structured `arguments` object (per the coarser
per-toolset `tools/list` granularity below) — the routing layer past
that unpacking step never sees, or cares, which wire shape produced
the split. Framing a request as an MCP tool call changes its wire
shape, not its authorization or execution path. This is stated
explicitly because it's the kind of thing that's easy to accidentally
special-case during implementation — a new entry point is exactly
where a second, inconsistent ACL check tends to get introduced by
accident.

### Decision: process persistence — superseded

**Superseded 2026-08-13 by `openspec/changes/define-on-demand-toolset-execution/`.**
This section originally chose a persistent, always-running process
(the RBUS-provider shape) over per-call fork/exec, reasoning that
Plugin Manager's health-check model assumed a process that stays up.
That's reversed: toolsets now spawn on demand (recommended:
idle-timeout, not pure per-call), and Plugin Manager's health-check
model was revised to confirm health at spawn time instead of via
periodic polling — driven by the 300KB footprint figure
(`OPEN_QUESTIONS.md` B3), which isn't achievable with every toolset
resident simultaneously. What's unchanged: toolsets still run
out-of-process and sandboxed (`define-plane-vs-toolset-model`); only
*when* the process exists changed.

### Decision: RDM Client verification is not bypassed by the push mechanism's convenience

However lightweight "push a new plugin/script" is made to feel
operationally (mirroring rpcd's low-friction script-drop experience),
the toolset is not registered with Plugin Manager or surfaced via
`tools/list` until RDM Client has verified its signature and
manifest-declared capabilities against policy (FR-12), unchanged. The
convenience is in the push mechanism's simplicity, not in skipping
verification — a cloud-invocable, AI-agent-facing toolset is a higher-stakes
actor than rpcd's original trusted-local-admin script convention, and
should not inherit its lack of a verification gate.

### Decision: the device publishes its toolset list first — push is primary, `tools/list` is secondary

**Added 2026-08-13, per direct confirmation.** The device agent SHALL
proactively publish its full toolset list via `capability-sync`'s
existing push mechanism as soon as a session is established (in
addition to the existing load/unload/reload triggers) — before
relying on the cloud sending a `tools/list` request at all. `tools/list`
remains available for on-demand or synchronous confirmation (e.g., a
cloud client reconnecting mid-session, or explicitly re-checking
current state), but it is not the primary discovery path — publish is.
This resolves `OPEN_QUESTIONS.md` C4 in part: rather than two
permanently equal, independent mechanisms, push is now primary and
`tools/list`/`notifications/tools/list_changed` are secondary,
on-demand confirmations of the same underlying data.

### Decision: relation to `capability-sync`'s existing push mechanism — permanently separate deliveries, one shared trigger point

**Revised 2026-08-13, per direct confirmation — resolves the "worth
revisiting later" note this section originally carried.**
`capability-sync/spec.md` already requires an event-triggered,
device-identity-authenticated push to the cloud's Device Model
Mapping/Tool Catalog on toolset load/unload/reload.
`notifications/tools/list_changed` is a *different* delivery
mechanism for the same underlying event, aimed at a live,
already-connected MCP session rather than an offline catalog update.
Both fire from the same trigger (Plugin Manager load/unload/reload)
and read the same underlying `capabilities()`/`schema()` data.

**Confirmed: these stay two permanently separate deliveries, not
merged, and neither becomes the other's source.** The reasoning:
they serve consumers with genuinely different lifetimes (a durable,
always-relevant catalog record vs. a heads-up that's only meaningful
if a session happens to be connected right now), different
credentials (long-lived device identity vs. a short-lived per-session
SAT token, `define-sat-token-format`), and different delivery
guarantees (the catalog push needs retry/eventual-consistency
behavior appropriate to a system of record; the live notification
wants to be fast and is fine being best-effort, since a client that
misses it can always re-poll `tools/list`). Making one the literal
source feeding the other would couple these mismatched reliability
profiles together — a retry/backoff cycle on the durable catalog push
could delay or block the live notification, which has no reason to
wait on it.

**What *does* change: both deliveries SHALL be fanned out from one
shared emission point inside Plugin Manager, not two independently
registered "listen for reload" handlers.** The risk being closed
isn't coupling (deliberately avoided, above) — it's silent drift: two
separately-written listeners can be edited independently, and it's
easy for a future change to update one and simply forget the other
exists, since nothing forces both to be touched together. A single
internal function that Plugin Manager's load/unload/reload path calls
unconditionally — which then enqueues into two independent, isolated
downstream queues (one for the durable catalog push, one for live
session notification) — makes it structurally harder to add a new
reload-triggering code path without going through the one place both
consumers are wired to. The two downstream queues remain fully
independent past that point: no shared retry logic, no ordering
dependency, no path where one's failure or delay affects the other's
delivery.

See `reference-impl/plugin_contract.h`'s `describe()`/`registry_add()`
pattern for the existing precedent of "one call site feeds every
registered consumer" already used for plugin registration itself —
this decision applies the same shape to the reload-notification
fan-out, not a new pattern for this codebase.

### Decision: resources vs. tools — settled for the triage plane now, other planes deferred to Phase 2

**Added 2026-08-13, per direct confirmation, resolving
`OPEN_QUESTIONS.md` C1 (now A11).** The triage plane keeps its
current model unchanged: `triage.capabilities` and every other
triage method stay unified as MCP tools, not split into a separate
`resources` surface for the read-only ones. This isn't a rejection of
the resources/tools distinction in general — it's scoping the
decision to what's actually being built now (Phase 1 is triage-only).
Whether config/management/control-plane reads should be modeled as
MCP resources, distinct from their invocable-action counterparts, is
explicitly deferred to Phase 2, when those planes' toolsets are
actually designed in full rather than assumed. Not deciding this now
for planes that don't have a concrete design yet is deliberate, not
an oversight.

### Decision: `tools/call` and plain JSON-RPC 2.0 are two permanently coexisting command shapes

**Added 2026-08-13, per direct confirmation, resolving
`OPEN_QUESTIONS.md` C2 (now A12).** `tools/call` does not become the
exclusive command path. A non-MCP, plain JSON-RPC 2.0 path — already
required by FR-1 as this project's baseline transport — stays
supported indefinitely, for cloud/ops clients that have no reason to
speak MCP's specific framing (an automated test harness, a legacy
XMiDT-side operations script, a monitoring job). This is viable with
low duplication risk *because* of a decision already made above:
`tools/call` is not a second command path at the execution level —
both shapes decode down to the identical `(toolset.method, args)`
tuple and flow through the same single ACL checkpoint, Plugin
Manager resolution, and Execution Framework. What's being permanently
maintained is two **framings** in front of one path, not two
backends.

**Advantages of permanent dual-path support:**
- Non-AI-agent callers (ops tooling, scripted cloud clients, legacy
  XMiDT/WRP infrastructure) never have to adopt MCP's
  handshake/discovery model just to keep calling a method that
  already worked before MCP was introduced — MCP is additive, not a
  forced migration.
- MCP adoption on the cloud side can be gradual and per-team; nothing
  about existing tooling breaks the day MCP support ships.
- Because both shapes converge on one execution path (the decision
  above), the *security-critical* machinery — ACL, resolution,
  sandboxing — is written and reviewed once, not duplicated per shape.
  The cost of the second shape is a parsing/framing layer, not a
  parallel implementation of anything that matters for correctness.

**Bottlenecks / costs, stated plainly because "permanent" means these
never retire:**
- Two request-shape parsers/framings to keep in sync forever — every
  new toolset method needs to work correctly through both entry
  points, indefinitely, with no future date where one gets
  deprecated and this burden goes away.
- Documentation and test coverage effectively double: every capability
  needs coverage through both shapes, not just the one a given team
  happens to exercise.
- This is structurally the exact shape where an accidental second,
  inconsistent authorization check tends to get introduced by
  mistake — already flagged above as a reason to be explicit that
  `tools/call` isn't a separate path. Permanence means every future
  change touching either entry point needs to keep re-verifying both
  still converge on the same checkpoint, not just verifying it once
  at launch. This is now explicitly in scope for B4's Phase-2
  concluding security review (`OPEN_QUESTIONS.md` B4), not assumed
  safe by construction forever.
- Discoverability is asymmetric: an MCP-aware client gets
  `tools/list`'s self-describing catalog; a plain JSON-RPC caller has
  no equivalent standardized discovery mechanism defined by this
  project. Such a caller needs out-of-band knowledge of method names
  and schemas, which is more brittle and easier to silently break
  than a schema an MCP client can query directly.
- Two "front doors" to version: if MCP's own protocol version moves,
  Dispatch Core tracks that upgrade while the plain JSON-RPC path
  stays independently stable — a growing compatibility matrix (MCP
  version × plain-path behavior × toolset schema version) that a
  single-path design wouldn't have.

## File/Component Changes

- Dispatch Core: implement `initialize`/`tools/list`/`tools/call`/
  `notifications/tools/list_changed` as JSON-RPC methods over the
  existing transport; no new transport component.
- Schema & Discovery: add an MCP-tool-shaped projection of each
  toolset's existing `schema()` output; the underlying schema itself
  is unchanged.
- Toolset push/discovery: out-of-process, persistent-process model,
  gated by RDM Client verification before Plugin Manager registration.
- `add-triage-skillset-mapping-phase1`: requires a second correction
  (tracked in `tasks.md` §4), on top of the one already required by
  `define-plane-vs-toolset-model`.
