# Design: Synchronous Toolset Push

## Technical Approach

### Message shape

Request (build/release pipeline → device), WRP `msg_type: 3`,
`content_type: application/json`, JSON-RPC method `toolset.push`:

```json
{
  "jsonrpc": "2.0",
  "method": "toolset.push",
  "params": {
    "toolset": "wifi-triage-ext",
    "version": "1.3.0",
    "artifact": "<base64-encoded plugin bytes>",
    "signature": "<base64-encoded signature over artifact>",
    "signer": "release-authority-key-id",
    "nonce": "b6e2...",
    "manifest": {
      "plane": "triage",
      "capabilities": ["..."],
      "device_nodes": ["..."],
      "adapter": "none"
    }
  },
  "id": "..."
}
```

Response, same WRP `transaction_uuid`, synchronous:

```json
{
  "jsonrpc": "2.0",
  "id": "...",
  "result": {
    "status": "loaded",
    "toolset": "wifi-triage-ext",
    "version": "1.3.0"
  }
}
```

or, on rejection, a JSON-RPC `error` with a reason (signature
mismatch, replayed nonce, size ceiling exceeded, manifest requests a
capability outside policy, ACL denial).

### Decision: "synchronous" means the accept/reject decision, not full multi-cycle health confirmation

The response confirms signature verification and initial load
succeeded — it does not wait out Plugin Manager's full health-check
window before responding, since that would reintroduce the kind of
open-ended wait this change exists to avoid. A separate
`notifications/tools/list_changed` (from `define-toolset-as-mcp-tool-model`)
fires once the pushed toolset is actually live and its methods are
discoverable, giving the caller a second, async confirmation if it
wants to wait for full readiness rather than just initial acceptance.

### Decision: verification is the same policy RDM Client enforces, relocated

Dispatch Core's inline check on `toolset.push` — signature validity
against a trusted signer, manifest-declared capabilities against
policy — is the identical check RDM Client performs for its own
install path (FR-12), not a separate, weaker one. What differs is
orchestration (synchronous, inline, no download step) not the
standard applied.

### Decision: payload encryption — superseded, now required

**Superseded by `openspec/changes/require-payload-encryption-and-message-routing/`.**
This section originally argued transport TLS alone was sufficient and
payload-level encryption wasn't assumed necessary. Your supplied
architecture settled this directly: every WRP payload, including a
`toolset.push` artifact, is encrypted at the payload level in addition
to TLS/WSS transport, as defense in depth against infrastructure that
terminates or inspects the transport hop. Artifact signing (below)
still covers authenticity/integrity — encryption and signing are
answering two different questions (can anyone read this vs. did it
really come from a trusted signer) and this change now requires both.

### Decision: dedicated ACL scope, separate from any `tools/call` grant

`toolset.push` requires its own narrow permission, held only by a
build/release pipeline identity — never satisfied by a scope that
merely grants read/write on a toolset's own methods via `tools/call`.
This keeps "can invoke this toolset's capabilities" and "can replace
what this toolset's code actually is" as two different, independently
grantable levels of trust, still evaluated through Dispatch Core's one
ACL checkpoint (FR-4) like everything else — not a bypass of it.

### Decision: Phase 1 exception — original health-check-gated rollback applies again, but only there

**Added 2026-08-14.** `toolset.push` is now in scope for Phase 1
itself (`add-phase1-command-execution-exception`), not deferred to
Phase 2 — but Phase 1's command-executing toolset(s) run in-process,
per that change's explicit exception to `define-plane-vs-toolset-model`'s
Decision B. That means the amendment directly below (artifact
fallback, no resident process) doesn't fit Phase 1: there's no
separate process to spawn or fall back into yet. For Phase 1
specifically, rollback reverts to this section's *original* design,
un-amended: the prior in-process version stays loaded and serving
until the new version passes its health check, then Plugin Manager
switches which version handles calls. This is Phase-1-only and is
retired, not merged with, the artifact-fallback model below once
Phase 2's out-of-process hardening actually lands for that toolset —
two mechanisms for two phases, not one evolving into the other.

### Decision: rollback via health-check gate — amended for on-demand execution

**Amended 2026-08-13 by `define-on-demand-toolset-execution`.** This
section originally had the prior version "stay loaded and serving"
alongside the new one until a health check passed — that assumed
persistent, always-running toolset processes. Under the on-demand
execution model, neither version is necessarily running at any given
moment: Plugin Manager instead retains the *prior artifact* as a
fallback. The next demand spawns the new version first; if that spawn
or its first response fails health confirmation, Plugin Manager falls
back to spawning the prior artifact and reports the failure via the
same notification path a manual RDM rollback would use. Same safety
property (a bad push never leaves the toolset unusable), without
requiring both versions simultaneously resident.

### Decision: `toolset.push` is a management method, not an MCP tool

It's deliberately not surfaced via `tools/list`/`tools/call` — those
are for AI-agent skill invocation against already-installed toolsets.
Mixing "invoke a capability" and "replace a toolset's code" into one
discoverable surface would make an already-loaded, narrowly-scoped
publish credential look, from the wire shape, like just another tool
call. Keeping them structurally separate makes the elevated nature of
`toolset.push` visible in the protocol, not just in ACL policy.

### Decision: RDM Client's exclusive scope — any `dlopen()`-able binary, regardless of size

**Resolved 2026-08-13, per direct confirmation, replacing the
size-based boundary below.** The rule distinguishing `toolset.push`
from RDM Client's path is artifact *type*, not size: RDM Client is
used for **any** toolset implementation library file or compiled
binary meant to be `dlopen()`'d at runtime — i.e., any `load_type:
dynamic` artifact — no matter how small. `toolset.push` SHALL NOT
carry a `dlopen()`-able binary under any circumstances; that always
goes through RDM Client's existing verified download/install/rollback
pipeline, because shipping new executable code onto the device is the
specific risk that pipeline exists to gate carefully, independent of
the artifact's size.

**Open consequence, not resolved here:** this narrows what
`toolset.push`'s own `params.artifact` field (§ Message shape, above)
is actually for. The original design showed it carrying "base64-encoded
plugin bytes" — under this corrected boundary, that can no longer mean
a `dlopen()`-able `.so`. What it *can* still reasonably carry —
toolset definition/manifest metadata referencing a binary already
delivered via RDM, a script-based (interpreted, non-`dlopen()`)
toolset implementation, or purely command arguments with no code
artifact at all — is a real open question, not assumed by this
document. Tracked in `tasks.md`.

### Boundary against RDM Client's path — superseded

The original version of this section proposed a size-based boundary
("plugin-scale artifacts" vs. "firmware/large-package installs"),
deliberately left unsized. That boundary is superseded by the
type-based rule above — size is no longer the deciding factor.

### Decision: RDM's install/rollback unit stays the whole toolset package, even with per-plugin static/dynamic sub-parts

**Added 2026-08-13, per direct confirmation, resolving
`OPEN_QUESTIONS.md` B9.** A toolset containing multiple internal
plugins (some `load_type: static`, some `load_type: dynamic`) is not
split into separate RDM install/rollback units per sub-plugin — FR-11/
FR-12's "whole toolset package" unit is unchanged. When a toolset
needs a genuinely new device-level implementation (new compiled/
`dlopen()`-able code, not just new arguments to an existing method),
that code is built and delivered as part of the toolset's package via
RDM Client, same as any other toolset install — never as a
standalone per-plugin artifact pushed independently. Once RDM has
installed (and verified) that package, the cloud can then call its
methods normally via `tools/call`, through the same resolution path
as any already-resident toolset.

This reconciles cleanly with `add-triage-skillset-mapping-phase1`'s
corrected model (`define-plane-vs-toolset-model`'s Consequence,
above): a toolset's internal static/dynamic sub-parts are merged
inside one toolset process at that process's own init time (per
`triage_core_static.c`/`plugins/triage_wifi.c`'s pattern), and RDM
only ever sees and installs the toolset as one package — it has no
visibility into, or need to separately version, the sub-parts inside
it.

**Timing, stated plainly:** RDM's install/rollback cycle is
asynchronous today (download, verify, install, health-confirm, with
no synchronous accept/reject the way `toolset.push` gives) — that's
unchanged by this decision. The aspiration is for more of this to
feel synchronous over time, but the only concrete synchronous path
that exists today is `toolset.push` itself, scoped to non-`dlopen()`-
able artifacts (per the RDM/`toolset.push` boundary above). Making
RDM's own install path synchronous is not designed or scheduled here
— it's a forward-looking direction, not a resolved decision, and
should not be assumed available until a real change proposes it.

## File/Component Changes

- Dispatch Core: implement `toolset.push` as a JSON-RPC method with
  inline verification, reusing RDM Client's existing signature/manifest
  policy check rather than duplicating it independently.
- Plugin Manager: extend its existing health-check/reload logic to
  support "load new, keep old warm, promote or revert based on health
  check" for pushes specifically.
- ACL Policy Store: a new, narrowly scoped permission for `toolset.push`,
  distinct from any toolset's own read/write method scopes.
- RDM Client: unchanged.
