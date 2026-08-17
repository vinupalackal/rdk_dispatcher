# Proposal: Merge diag-server as the `diagnostics` Toolset

## Intent

Formalize `external/diag-server/` — an existing, working, standalone
service — as the RDK Dispatcher project's `diagnostics` toolset,
capturing in OpenSpec the design and code that
`docs/24_diag_server_merge_plan.md` has already worked out and
(mostly) built, rather than designing it fresh. This is a
retroactive-formalization change: nearly everything in scope below is
already implemented and verified in `external/diag-server/`; this
change's job is to state it as project-level requirements, cross-link
it against the specs it depends on or deviates from, and flag the
handful of places where the built state genuinely doesn't yet match
this project's general model — rather than silently asserting
compliance it doesn't have.

This is Phase E.1 (`docs/24_diag_server_merge_plan.md` §8 step 6,
§12 Phase E.2), gated on §9's open questions being resolved first —
they now are (§14 items 7-9, last one closed 2026-08-16).

## Scope

In scope:
- Registering `diagnostics` as a concrete instance of the Phase 1
  in-process command-execution exception
  (`add-phase1-command-execution-exception`), naming diag-server as
  the toolset that exception was written to cover in the abstract.
- The `static`/`dynamic` per-tool typing model (catalog `"type"`
  field) and its now-closed override policy: static tools never honor
  a caller-supplied command (the catalog's own command always runs);
  dynamic tools remain caller-command-driven, gated only by the
  blocklist.
- The multi-plane catalog model (`"plane"` field, one
  `diag-<plane>-catalog.json` file per plane, explicit `plane` request
  field) as it's actually implemented.
- The local-only IPC endpoint (`DIAG_LOCAL_RECV_URL`/
  `DIAG_LOCAL_SEND_URL`) diag-server now binds, and the **interim, deliberately
  tracked deviation** this creates from `dispatch-core/spec.md`'s
  single-ACL-checkpoint model — see design.md's "Deviations to
  reconcile" section. This is the one part of scope this proposal does
  not just formalize as-is; it names the gap and proposes how it gets
  closed later, in Phase 2.
- diag-server's own in-process ACL gate (`diag_acl_check()`) and its
  outbound capability-sync notification (`diag_notify_capability_sync()`),
  as the concrete way `diagnostics` satisfies (or, for ACL, partially
  satisfies pending Phase 2) the project's general ACL and
  capability-sync requirements.
- The catalog push/reload mechanism (F2 version field, F3 health-check-gated
  swap, restricted to the local transport only) as `diagnostics`'
  concrete `toolset.push`/rollback behavior under the Phase 1 in-process
  exception's health-check-gated model
  (`add-phase1-command-execution-exception`'s toolset-lifecycle delta).
- The legacy wire format (`{tool, command}` / `{tool, exit_code,
  stdout}` msgpack) as a permanent, first-class framing for this one
  toolset — not a transitional adapter — per
  `docs/24_diag_server_merge_plan.md` §5's resolution of open question 2.

Out of scope (unchanged, tracked elsewhere, not re-decided here):
- The ACL Policy Store's transport (how `acl_policy_store_query()` is
  actually reached) — Phase 2, `docs/24` §14 item 4.
- A1 / the SAT token / caller-identity format — Phase 2, `docs/24` §14
  item 6, blocks Phase D.2's ACL-denial integration test and full
  runtime linkage of `diag_acl_check()`.
- Sandboxing / out-of-process execution for `diagnostics` — already
  covered, in the abstract, by
  `add-phase1-command-execution-exception`'s `sandboxed-runtime` delta;
  this change doesn't reopen it.
- Payload encryption — Phase 2 across the whole project
  (`require-payload-encryption-and-message-routing`), unaffected here.
- F6 (RDM verified-install/rollback pipeline for diag-server itself as
  a deliverable) — Phase 2, `docs/24` §12 Phase E.3.
- The full toolset-manifest conversion (method schemas, `load_type`)
  — `docs/24` §8 step 2, partially done (the catalog carries `type`/
  `plane` today) but not finished; this change formalizes what exists,
  not the finished manifest shape.

## Why this shape, not a redesign

Every substantive decision here was already made, and in almost every
case already built and verified, across `docs/24_diag_server_merge_plan.md`
§§2, 5, 6, 8, 9, 10, 11, 13, 14, 15 and the code in
`external/diag-server/diag-server-nn.c`. This proposal does not
introduce new design; it's the write-up `docs/24` §8 step 6 and §12
Phase E.2 always said would happen once §9's open questions closed.
The one place this proposal adds something `docs/24` hadn't fully
resolved is the ACL-checkpoint deviation named above — `docs/24` §13
built `diag_acl_check()` inside diag-server itself, by direct
instruction, but never squared that against `dispatch-core/spec.md`'s
"exactly once, in Dispatch Core... no other component... SHALL make an
independent access-control decision" requirement. That reconciliation
belongs in an OpenSpec change, not buried in an implementation doc, so
it's surfaced here rather than left implicit.
