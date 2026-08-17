# Design: `diagnostics` Toolset (diag-server)

## Context

`external/diag-server/diag-server-nn.c` predates this project and ran
as an independent service before any merge work started. Across
`docs/24_diag_server_merge_plan.md`'s many sessions, it was hardened
(shell removed, timeouts enforced, command-safety checks added,
init-time validation) and then given three new capabilities so it can
sit inside this project's model instead of beside it: multi-plane
catalogs, an in-process ACL gate, and outbound capability-sync. This
document is the as-built architecture, not a proposal for new
architecture.

## End state, as built

One process, `diag-server-nn.c`, unchanged in its core execution
engine (`load_catalog()`, `is_command_safe()`, `tokenize_argv()`/
`free_argv()`, `run_command()`, `validate_static_commands()`,
`handle_request()`'s catalog-lookup/skip-check/execution decisions —
`docs/24` §13's exact "core logic" list). It answers five message
types over nanomsg PUSH/PULL sockets: EXEC (a diagnostic tool
invocation), DESCRIBE (self-description), HEALTH, PUSH (catalog
update), and it emits CHANGED (local) and `capability_sync.updated`
(cloud-facing) notifications on its own initiative.

It currently binds two socket pairs:
- The original public pair, `tcp://127.0.0.1:6666`/`6669`, talking to
  Parodus — but as of `docs/24` §15 B.4 part 2 (2026-08-16),
  `REGISTER_WITH_PARODUS` is compiled to `0`, so diag-server no longer
  sends its WRP type-9 registration. Parodus has no route to it. The
  sockets are still bound (so `capability_sync.updated` can still go
  out over them), but nothing external can address diag-server through
  this pair today.
- A new local pair, `DIAG_LOCAL_RECV_URL`/`DIAG_LOCAL_SEND_URL`
  (`ipc:///run/dispatcher/diagnostics-{in,out}.sock`), bound
  best-effort alongside the public pair. This is meant for a future
  Dispatch Core process to reach diag-server over — but **no such
  process exists yet**. In the sandbox this project has been built and
  tested in, `/run/dispatcher` isn't provisioned, so even this bind is
  currently a no-op in practice (diag-server logs a warning and falls
  back, same as always).

Put plainly: as of this change, diag-server is not reachable by
anything outside its own process's test harnesses. That's an accurate,
if unusual, snapshot of a Phase 1 component built ahead of the
component that's supposed to front it.

## Deviations to reconcile (Phase 2)

This section exists because the project's standing rule is to report
findings, not bury them in a design document's optimistic framing.

### 1. The ACL checkpoint isn't in Dispatch Core, because Dispatch Core doesn't front this path yet

`dispatch-core/spec.md`'s "Single ACL checkpoint" requirement is
explicit: *"The system SHALL enforce access control exactly once, in
Dispatch Core, for every request. No other component (toolset plugin,
adapter, Execution Framework) SHALL make an independent access-control
decision."*

`diag_acl_check()` (`docs/24` §13.4) runs inside diag-server itself,
as a guard clause in `handle_request()`, calling the same
`acl_policy_store_query()` extern every other toolset's checkpoint
uses — by direct instruction, because Phase C's separate Dispatch Core
process was retired (`docs/24` §13) before this component was built.
There is currently no Dispatch Core process in this path at all, so
strictly, diag-server is both "a toolset plugin" and "the only
component making an access-control decision for this traffic" — which
reads as exactly the case `dispatch-core/spec.md` says shouldn't
happen.

Two ways to read this, and this change picks the second, explicitly:
1. **Violation.** diag-server shouldn't have its own ACL check at all;
   it should wait for a real Dispatch Core process and front-load the
   check there, the same as every other toolset. Rejected for now —
   it would mean diag-server has *zero* access control until Dispatch
   Core's diagnostics-path integration is built, which is worse than
   an early, tracked, single-toolset exception.
2. **Scoped, temporary exception (adopted).** For as long as diag-server
   has no real Dispatch Core process fronting it, `diagnostics` is
   allowed to be its own checkpoint — mirroring exactly the pattern
   `add-phase1-command-execution-exception`'s `sandboxed-runtime` delta
   already established for containment (a narrow, tracked,
   time-bounded carve-out, not a general reopening of the rule). See
   the `dispatch-core` spec delta below, which states this exception
   the same way that one states its containment exception, and names
   its own retirement condition: once a real Dispatch Core process
   forwards `diagnostics` traffic over the local endpoint,
   `diag_acl_check()` becomes redundant defense-in-depth at most, and
   this exception is retired.

This is explicitly not resolved by writing the exception down — it's
resolved by Dispatch Core actually being built and taking over the
checkpoint role. Until then, the exception documents reality rather
than letting the spec and the code silently disagree.

### 2. `acl_policy_store_query()` has no implementation anywhere yet

Independent of point 1: even the *if-it-ran* semantics of
`diag_acl_check()` can't be exercised end-to-end today, because
`acl_policy_store_query()` is declared `extern` and never defined
(`docs/24` §14 item 4, Phase 2 — the transport underneath it isn't
chosen). `-fsyntax-only` compiles clean; a real linked binary would
not, until Phase 2 resolves this. This is a pre-existing, project-wide
gap (every toolset's ACL check shares the same unimplemented extern),
not something specific to diag-server's merge — named here because
it's the second half of why `diagnostics`' ACL enforcement isn't fully
real yet, distinct from point 1's process-topology issue.

### 3. The wire format is permanently non-generic for this one toolset

`docs/24` §5/§9 open question 2 already resolved this (not new to this
change): diag-server keeps its native msgpack `{tool, command}` /
`{tool, exit_code, stdout}` shape as a **permanent** wire format for
`diagnostics`, not a transitional shim pending conversion to generic
MCP `tools/call`/JSON-RPC framing. This is recorded here because it's
a real, intentional divergence from `define-toolset-as-mcp-tool-model`'s
generic framing expectation for every other toolset — flagged, not
silently different.

## The static/dynamic override model, as closed

Every catalog tool declares `"type": "static"` (default) or
`"type": "dynamic"`.

- **Static**: the catalog's own `command` always runs. A
  caller-supplied `command` field in the request is discarded
  unconditionally — not validated, not partially honored — regardless
  of whether the override would itself have been "safe." This closed
  2026-08-16 (`docs/24` §9 Q3, §14 item 7), replacing an intermediate
  state (2026-08-14 to 2026-08-16) where a static override was allowed
  through if its program (`argv[0]`) matched the catalog's declared
  program, which left a "same program, different arguments" gap open
  (e.g. redirecting `device_uptime`'s `cat /proc/uptime` to
  `cat /etc/shadow`).
- **Dynamic**: there is no catalog-declared command to fall back to —
  the tool is inherently caller-command-driven (see `adhoc_diagnostic`
  in `diag-triage-catalog.json`). The blocklist (`is_blocked()`, now
  including a basename check as well as a first-token check — `docs/24`
  D.1 Finding 2) still applies unconditionally; there is no
  program-pinning for dynamic tools, by design.

This is not a general "toolsets may not accept caller-supplied
commands" rule — it's specific to how `diagnostics` chose to close its
own known gap. A future toolset with different requirements isn't
bound by this precedent.

## Multi-plane catalogs

One `diag-<plane>-catalog.json` file per plane (`config-apply`,
`management`, `control`, `triage`), each loaded into its own `cJSON`
object — planes are not merged into one in-memory catalog. A request
may include an optional `"plane"` field; if present, lookup is scoped
to that plane only (a wrong-plane or unrecognized-plane request misses
cleanly, not falls through to another plane). If absent, lookup
searches all loaded planes and rejects (`LOG_ERR`, "ambiguous") if the
same tool name resolves in more than one. This is additive to the
original single-catalog `diag-triage-catalog.json` shape — every
currently-shipped tool is still `"plane": "triage"`.

## Push / reload

`PUSH` carries a diff, not a full catalog replacement: `base_version`/
`target_version`/`added`/`removed`/`modified` against the target
plane's catalog. `base_version` must exact-match the live catalog's
`_catalog_version` (compare-and-swap semantics, not a
newer-than-ordering comparison). The candidate catalog is validated in
isolation (`validate_static_commands()` re-run against it) before
promotion; if any tool in the diff's `added`/`modified` set ends up
marked `_skipped`, the push fails and the prior catalog keeps serving
— this is `diagnostics`' concrete instance of
`add-phase1-command-execution-exception`'s health-check-gated,
in-process swap rollback model (not the artifact-fallback model that
assumes out-of-process spawning).

**Transport-restricted, as of `docs/24` §15 B.4**: `PUSH` is only
accepted when it arrives via the local endpoint
(`req->from_local == true`) — rejected with `PUSH_ERR_FORBIDDEN_TRANSPORT`
otherwise. This exists because, during the window where both the
public and local endpoints are live with no ACL check yet gating PUSH
specifically, an unrestricted PUSH would let any WRP-addressable
public caller push a new catalog with zero authorization. DESCRIBE and
HEALTH are read-only/side-effect-free and stay reachable on both
sockets.

## Capability-sync

After a successful PUSH promotion, in addition to the existing local
`CHANGED` notification, diag-server calls
`diag_notify_capability_sync()` — a JSON-RPC 2.0 notification
(`capability_sync.updated`, `{toolset, version, capabilities}`) sent
over the public/Parodus push socket, per
`openspec/specs/capability-sync/spec.md`'s "same transport used for
commands, authenticated by device identity" requirement. No delta to
`capability-sync/spec.md` is needed — this is a new caller of an
already-generic mechanism, not new capability-sync behavior. (Note:
per the "End state, as built" section above, this notification
currently has nowhere real to go, since Parodus registration is
disabled — it's wired correctly but inert until B.4 part 2 is
reverted or a real Dispatch Core path exists.)

## Alternatives considered

- **Wait for a real Dispatch Core process before adding any ACL check
  to diag-server.** Rejected: leaves `diagnostics` with zero access
  control for an unbounded period, worse than a tracked exception.
- **Give diag-server its own, parallel ACL-query contract instead of
  reusing `acl_policy_store_query()`.** Rejected, already decided in
  `docs/24` §13.4: reusing the existing extern keeps exactly one query
  interface project-wide, avoiding a second contract that would need
  its own reconciliation later.
- **Restrict overrides to a catalog-declared argument allowlist instead
  of dropping them entirely for static tools.** Considered in `docs/24`
  §9 Q3 as the other original alternative; rejected 2026-08-15 because
  the static/dynamic split already gives callers who genuinely need
  flexibility a secured path (mark the tool `dynamic`, gated behind
  Phase 2's ACL/encryption/framing requirements for that framing —
  `docs/24` §14 item 3), so a static tool's override no longer serves a
  real use case.
