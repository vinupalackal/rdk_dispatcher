# ACL Implementation: rpcd Technical Review and Dispatch Core Design

Resolves `OPEN_QUESTIONS.md` B5/B6. Two findings looked separate but
are the same gap: `reference-impl/` has no ACL check anywhere (B5),
and `reference-impl/` has no code path at all for an externally
initiated command (B6). There's no missing check because there's
nowhere to put one yet — `dispatcher_core.c` only sketches the
internal sysevent/Netlink event-dispatch path, which correctly needs
no ACL check. The real gap is that the cloud-command path (where an
ACL check actually belongs) was never sketched.

## 1. Why the existing sketch has no ACL check, and why that's correct

`dispatcher_dispatch_event()` in `reference-impl/dispatcher_core.c`:

```c
void dispatcher_dispatch_event(const char *name, const void *data, size_t len) {
    for_each_registered_plugin(name, plugin) {
        run_with_timeout(plugin->handle_fn, name, data, len, plugin->desc->timeout_ms);
    }
}
```

This fires when the device's own sysevent/Netlink bus raises an
event the device itself generated — a WiFi radio state change, a
DOCSIS event, a link-state transition. There is no external caller
here to authorize; the identity involved is the device acting on its
own telemetry. rpcd has the identical asymmetry: a ubus event
listener reacting to a local broadcast never calls `session.access`
either — only a request arriving from outside the trusted local
process boundary (uhttpd's HTTP bridge, in rpcd's case) does. Adding
an ACL check to `dispatcher_dispatch_event()` would be checking
against nobody; it's not the gap.

## 2. What rpcd actually does when a check is needed

From `study-docs/09_acl_management_detailed.md` (live-verified
against real `ubusd`+`rpcd`, not just read from source):

**Where policy lives.** `/usr/share/rpcd/acl.d/*.json` — named
groups, each with `read`/`write` blocks of `{object: [methods...]}`.
Reloaded by `glob()` on every `session.login`, not cached at
startup, so a new ACL file takes effect on the next login without a
restart.

**Identity → groups.** `uci` `config login` sections list which ACL
group names a login matches, using `fnmatch()` (shell-glob syntax,
so `list read 'wifi-*'` matches every group starting with `wifi-`).
Negative entries (`! group-name`) are checked first and win
unconditionally over any positive match — deny-first.

**Write implies read.** If no `read` rule matches a group but a
`write` rule does, the read grant is given anyway. You don't need
two separate rules to let someone both see and change something.

**Two-step check at call time.** `session.login` (identity →
session ID + merged ACL set, done once) is separate from
`session.access` (session ID + object + function → allow/deny,
checked per call). The second call is what actually gates a request
— having a session doesn't imply any specific permission until this
check runs.

**Enforcement is opt-in, not automatic.** Assembling the merged ACL
set onto a session doesn't restrict anything by itself at the ubus
transport level — only a caller that explicitly asks
`session.access` before proceeding is actually gated. A bare `ubus
call` from a local shell bypasses it entirely. This matters directly
for point 4 below.

## 3. Mapping onto Dispatch Core's ACL Policy Store

The ACL Policy Store (`openspec/specs/acl-policy-store/spec.md`,
already written) is the reimplementation of this pattern, with one
structural change: rpcd's `session.login`/`session.access` split
assumes a stateful session the device tracks; Dispatch Core's model
is a stateless SAT/JWT token (`OPEN_QUESTIONS.md` A1) carrying its
permission groups at issuance, so there's no device-side session
store to look up per call — the token itself is what
`session.access`'s session ID stood in for.

| rpcd mechanic | Dispatch Core equivalent |
|---|---|
| `acl.d/*.json` named groups, `read`/`write` blocks | ACL Policy Store's "named permission groups" requirement — declarative, scoped by `read`/`write` over `{toolset: [methods...]}` |
| `uci login` sections, `fnmatch()` group matching | Identity-to-group mapping at token issuance, not matched per request |
| `!`-prefixed deny, checked first | "Deny rules evaluated first" requirement — same ordering, same reasoning |
| write implies read | "Write-implies-read fallback" requirement — same rule, carried over directly |
| `glob()` re-read on every login → hot policy updates | "Hot-reloadable policy" requirement — updates apply without a Dispatch Core restart |
| `session.access` explicit query, opt-in per caller | "Single ACL checkpoint" (dispatch-core `spec.md`) — the difference: not opt-in. Every request through Dispatch Core's one entry point is checked, not just callers that remember to ask |

The one deliberate divergence from rpcd, already decided in
`dispatch-core/spec.md`'s "Single ACL checkpoint" requirement, is
closing rpcd's opt-in gap: rpcd's `session.access` only gates callers
that choose to call it, which is why a bare local `ubus call` bypasses
ACL entirely. Dispatch Core's single checkpoint sits on the one path
every external request must go through (cloud `tools/call`, local UDS
clients alike — see dispatch-core `spec.md`'s "same ACL check ...
local UDS clients" requirement), so there's no equivalent bypass by
construction, not by caller discipline.

## 4. The actual gap: no command-handling path exists to put the check in

`dispatcher_core.c` has exactly one entry point,
`dispatcher_dispatch_event()`, and it's internal-only. There is no
sketch anywhere in `reference-impl/` of Dispatch Core receiving an
external JSON-RPC/MCP `tools/call` request, resolving it to a
toolset+method via Plugin Manager, and executing it — which is
`OPEN_QUESTIONS.md` B6 exactly. So "no ACL check exists" (B5) isn't a
missing line in an otherwise-complete flow; it's that the flow it
would belong to was never written. Section 5 below sketches that
missing path with the check placed where it belongs — the ACL Policy
Store query sitting between request resolution and toolset
execution, mirroring rpcd's `session.access` call but mandatory
rather than opt-in.

## 5. Reference-impl addition

See `reference-impl/dispatcher_command_path.c`, new in this change.
It sketches `dispatcher_handle_command()`: parse the incoming
`tools/call` (or plain JSON-RPC) request, resolve `toolset.method`
against the Plugin Manager registry, query the ACL Policy Store with
the caller's identity/groups from their SAT token, deny with a
JSON-RPC error on a negative answer, otherwise dispatch to the
resolved toolset. This is illustrative, matching the rest of
`reference-impl/`'s style — not the reviewed production loader.

## 7. Toolset resolution: static manifest first, live registration fallback

Added 2026-08-13, per direct follow-up. Step 1 of §5's flow
("resolve `toolset.method` against the Plugin Manager registry") was
left as a single opaque call. Here's the actual two-tier design
behind it, and why one tier isn't enough on its own.

**Why not just query the live registry.** Under on-demand execution
(A8), most toolsets aren't running most of the time — including
right after boot, before anything has been spawned even once. A
lookup that only checks a live, in-memory registry populated by
already-running processes would report "not found" for every
installed-but-currently-idle toolset, which is wrong: the toolset
exists, it just isn't warm. Resolution ("does this exist, and where
would I reach it") has to be answerable independent of whether the
target process happens to be up at that instant; spawning it is a
separate, later step.

**Tier 1 — static manifest.** Every installed toolset has a
persisted, install-time-written descriptor — one JSON file per
toolset (mirroring rpcd's own `acl.d/*.json` multi-file convention,
already used as a model elsewhere in this project), e.g.
`/usr/share/dispatcher/toolsets.d/wifi-triage-ext.json`. Its content
— name, plane, methods, schemas, `load_type` — is always *derived*
from the toolset's own manifest block, written by RDM Client on
install (FR-11/FR-12) or by the `toolset.push` handler on a
synchronous push (`define-synchronous-toolset-push`), never
hand-authored. This is Plugin Manager's fast path: an index read, no
process needs to be running, works even for a toolset that's never
been spawned since boot.

**Tier 2 — live self-registration fallback.** If a lookup misses the
manifest, that isn't automatically "doesn't exist" — it can also
mean a toolset was just pushed or reloaded and its manifest file
write hasn't landed yet, or the manifest and the process's own
self-description have drifted apart. If the target toolset process
happens to already be live (still warm within its idle-timeout
window), Plugin Manager asks it directly for the same self-report a
toolset already gives at its own startup handshake — this isn't a
new mechanism, just reused here as a fallback. A Tier-2 hit triggers
an asynchronous manifest repair, so the next lookup for the same
method hits Tier 1 again. Only when both tiers miss is the request
a genuine "Method not found" (`-32601`).

**What resolution returns, and what it deliberately doesn't.** A
resolved lookup yields a *locator* — which toolset process to reach,
and how (`process_uds_path`), plus whether Plugin Manager currently
believes it's running. It does **not** yield a function pointer or
imply Plugin Manager dlopen()s anything itself. The static-vs-dynamic
merge (`load_type: static` compiled-in vs. `load_type: dynamic`
`dlopen()`'d) happens *inside* the target toolset's own process,
exactly as already shown for the Triage Toolset specifically
(`triage_core_static.c`, `plugins/triage_wifi.c`,
`triage_capabilities.c`'s `registry_all_of_plane()`) — generalized to
every plane's toolset process by `define-plane-vs-toolset-model`,
unchanged by this section. Conflating the two would mean Dispatch
Core itself loading toolset code into its own process, which is
exactly the pre-A2/A3 model `dispatcher_core.c`'s
`dispatcher_load_plugins()` still shows and which is already flagged
elsewhere (`CLAUDE_CODE_WORKFLOW.md`, "reopened, not resolved") as
non-compliant and in need of rework. This section's design does not
depend on, or extend, that stale sketch.

**Reference-impl addition.** `reference-impl/toolset_resolution.c`
implements `registry_resolve()` as described above.
`dispatcher_command_path.c` was updated to call it and to add an
explicit "ensure the toolset process is reachable, spawning on
demand if needed" step (§5's original sketch left spawn-on-demand as
an unlabeled aside inside the dispatch step; it's now its own
numbered step, since an unauthorized request should not pay
spawn cost — the ACL check runs before it, not after).

## 8. Still open

- The reference-impl sketch shows the SAT token supplying groups
  directly (per A1's "permission groups embedded in the token at
  issuance"), not a session lookup — this is consistent with A1 but
  A1 itself remains drafted/unarchived. If A1's token format changes
  before archiving, this sketch's identity-extraction step will need
  a matching update.
- Audit logging (ACL Policy Store's own requirement) isn't shown in
  the sketch — out of scope for a routing-path illustration, but a
  real implementation needs the denied/allowed decision actually
  logged, not just acted on.
- This document doesn't re-litigate whether Platform Adapters'
  separate RBUS/Thunder-native ACL check (NFR-4, no confused deputy)
  is correctly independent of this one — that's B4's job (the
  Phase 2 concluding security review), not this review.
