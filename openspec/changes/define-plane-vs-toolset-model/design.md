# Design: Unified Toolset Architecture Across All Planes

**Revised 2026-08-13** — see `proposal.md`. Sections below replace the
original "planes are exempt" decisions with a single unified model.

## Technical Approach

### Decision A: Plane is a descriptive tag, not an execution-model boundary

Config-apply, management, control, and triage describe *what a piece
of toolset logic does* — nothing more. The label carries no claim
about trust, distribution, or execution model. There is no first-party
exemption for any plane, and no plane's logic is entitled to run
in-process just because it ships alongside Dispatch Core.

### Decision B: Every toolset uses the same toolset architecture, regardless of plane

Config-apply, management, control, and triage logic are organized and
shipped as toolsets — out-of-process, sandboxed (mount/PID namespace,
per-plugin seccomp-bpf, cgroup limits, dropped capabilities per
FR-13/FR-14), installed via Toolset Store/RDM Client or `toolset.push`,
and supervised by Plugin Manager — identically to a domain toolset
like wifi or DOCSIS. A toolset may still implement logic spanning
multiple planes internally (unchanged from the original version of
this decision): a single "wifi" toolset can have its own config-apply,
control, and triage logic inside it, or a dedicated "triage" toolset
can exist on its own — either is architecturally valid, as long as
whichever toolset it lives in is out-of-process and sandboxed like any
other.

### Decision C: The cloud drives every plane the same way — toolset name plus arguments

Whatever plane a capability belongs to, the cloud invokes it
identically: name the toolset, pass arguments, via `tools/call` (per
`define-toolset-as-mcp-tool-model`) through Dispatch Core's one ACL
checkpoint and Execution Framework. There is no separate,
plane-specific invocation path — a triage toolset and a wifi toolset
are called through the exact same mechanism.

### Consequence: `reference-impl/` needs rework, not just relabeling

The original version of this change judged
`reference-impl/dispatcher_core.c`'s in-process `dlopen()` +
`run_with_timeout()` model acceptable *because* it was scoped as
exempt plane logic. That exemption is withdrawn. Bringing the RDK-B
embedded build into compliance means its config-apply/management/
control/triage logic needs to move to the same out-of-process,
sandboxed, toolset-supervised model as any other toolset. This is a
rewrite of that code, not of this document — tracked as a task, not
done in this pass.

### Consequence: the exception clause below has been invoked

**Added 2026-08-14.** The paragraph above ends with: "If out-of-process
overhead turns out to be unacceptable for the fastest control-plane
paths, that needs to surface as an explicit, reviewed exception
request against this decision — not a quiet reversion back to
in-process without saying so." That request has now been made and
recorded: `openspec/changes/add-phase1-command-execution-exception/`
grants Phase 1's real command execution and `toolset.push` an
explicit, narrowly-scoped, tracked exception to run in-process for
this phase specifically. Decision B itself is unchanged and still
applies uniformly to every other toolset and every later phase — see
that change for the full reasoning and its bounds.

### Consequence: a real engineering tension, stated plainly rather than hidden

Moving every plane operation out-of-process introduces IPC overhead —
process boundary crossing, serialization, sandbox setup — on a
latency-sensitive embedded event loop. `CLAUDE.md`'s own hard rules
exist specifically because blocking or slow operations on the sysevent
notification thread are a real, previously-hit failure mode ("caused a
double-free in the WAN dispatcher last quarter" is exactly the kind of
incident this class of change can make worse if done carelessly). This
decision does not resolve that tension — it makes it a concrete,
must-be-sized requirement, tied directly to the still-open footprint
budget question (`OPEN_QUESTIONS.md` B3). If out-of-process overhead
turns out to be unacceptable for the fastest control-plane paths, that
needs to surface as an explicit, reviewed exception request against
this decision — not a quiet reversion back to in-process without
saying so.

### Consequence: Phase 1's original "Triage Toolset" framing was right after all

The in-session correction attempted earlier this project (aggregating
triage capabilities across "Dispatch Core's own plane plugins" and
toolsets) was solving a problem that no longer exists under this
decision — there is no such thing as "Dispatch Core's own plane
plugins" distinct from toolsets. `add-triage-skillset-mapping-phase1`'s
original design (a dedicated triage toolset, discovered via
`tools/list`) already matches this corrected decision. No further
correction to that change is needed on this specific point.

### Decision D: portability across RDK-B/RDK-V via a per-platform event namespace, not per-platform code

**Added 2026-08-13, per direct confirmation, resolving `OPEN_QUESTIONS.md`
B7.** NFR-1 (identical implementation across RDK-B and RDK-V) is
confirmed as a hard requirement, not aspirational — and the plane
model above applies to RDK-V exactly the same way it applies to
RDK-B, unchanged: plane stays a descriptive tag, every toolset stays
out-of-process and sandboxed, the cloud still invokes everything by
toolset name plus arguments. The CCSP/sysevent/Netlink coupling that
raised the original tension is isolated to one boundary: the event
*names* a toolset's `plugin_descriptor_t.events` list registers
against are namespace-relative, resolved against a platform-supplied
namespace table at load time — not compiled directly into Dispatch
Core, Plugin Manager, or the toolset's own logic. RDK-B and RDK-V MAY
define an identical namespace for events that mean the same thing on
both platforms (e.g. both call it `wifi-radio-reset`) or MAY diverge
where the underlying event bus genuinely differs (e.g. an RDK-V-only
tuner event with no RDK-B counterpart) — that choice belongs to
whoever authors each platform's namespace mapping, not to this
decision. What stays fixed regardless of platform is Dispatch Core's
own code: routing, the single ACL checkpoint, the MCP surface, and
toolset lifecycle management never branch on which platform they're
running on. Only the namespace resolution step at plugin registration
does.

## File/Component Changes

- `sandboxed-runtime/spec.md`: remove the "toolsets only, not
  dispatcher-core planes" exemption language; state FR-13/FR-14
  applies uniformly to every toolset regardless of which plane(s) its
  internal logic touches.
- `toolset-lifecycle/spec.md`: keep "a toolset may implement multiple
  planes internally"; remove any implication that plane logic can live
  outside the toolset architecture.
- `CLAUDE_CODE_WORKFLOW.md`: the "Isolation trade-off" open thread is
  reopened as real, unresolved follow-up work, not marked resolved.
- `reference-impl/`: flagged for rework — moving plane logic
  out-of-process — as a tracked task, not changed in this pass.
- `plugin_contract.h`: `plugin_descriptor_t.events` needs a documented
  convention that entries are namespace-relative, not raw platform
  event names — tracked as a task, not changed in this pass.
