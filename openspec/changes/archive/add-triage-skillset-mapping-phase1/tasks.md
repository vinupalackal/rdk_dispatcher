# Tasks

**Corrected 2026-08-16** — replaces the original task list, which
built around a bespoke `triage.capabilities` JSON-RPC method. Section
2 (below) is the part that actually changed; sections 1, 3 (partially),
4, and 5 carry over with only wording updates.

## 1. Triage Toolset process scaffold
- [ ] 1.1 Stand up the Triage Toolset as its own process, registered with Plugin Manager (coarse entry only, per `toolset-lifecycle/spec.md`)
- [ ] 1.2 Implement the static plugin registration table (compiled-in `describe()` calls, no `dlopen()`)
- [ ] 1.3 Port `dispatcher_load_plugins()`'s dynamic `.so` loader into the Triage Toolset process, scanning `/usr/libexec/dispatcher/triage/`
- [ ] 1.4 Extend `plugin_descriptor_t` with `load_type` and `version`; update `reference-impl/plugins/triage_wifi.c` to set them

## 2. Discovery via generic `tools/list`, not a dedicated method — corrected 2026-08-16
- [ ] 2.1 Implement `capabilities()`/`schema()` on the Triage Toolset merging static + dynamic plugin descriptors into one internal descriptor set (unchanged logic from the original 2.1)
- [x] ~~2.2 Wire `triage.capabilities` as a JSON-RPC 2.0 method resolvable by Dispatch Core → Plugin Manager → Triage Toolset~~ — **removed 2026-08-16, not built**: no such method exists under the corrected design; discovery goes through `tools/list` instead
- [ ] 2.3 Confirm Schema & Discovery's `tools/list` projection includes a `"triage"` entry sourced from 2.1's descriptor set, per `define-toolset-as-mcp-tool-model/spec.md` (no Triage-specific code needed beyond exposing `capabilities()`/`schema()` correctly — this is the generic mechanism doing its job)
- [x] ~~2.4 **Open design question**: decide how per-plugin `load_type`/`version`/`events`/`timeout_ms` metadata is carried inside `tools/list`'s `inputSchema.oneOf` shape.~~ — **Confirmed 2026-08-16**: `openspec/changes/resolve-tools-list-metadata-and-acl-scoping/` (confirmed by direct instruction) settles this with a sibling `methods` array on the `tools/list` entry, not embedded in `inputSchema`. This change's own `design.md` example has been updated to match
- [x] ~~2.5 **Open question**: does `tools/list`'s catalog need to be filtered per caller's ACL scope~~ — **Confirmed 2026-08-16**: same change settles this with a two-tier model (every toolset named; per-toolset detail gated by the existing `acl_policy_store_query`). See `resolve-tools-list-metadata-and-acl-scoping/design.md` Decision 2 and `OPEN_QUESTIONS.md` A17. This change's `design.md` and `specs/triage/spec.md` have been updated to match (an unauthorized caller sees `{"name": "triage", "access_restricted": true}`)

## 3. WRP transport
- [ ] 3.1 Confirm Parodus Agent delivers a `tools/list` request addressed to Dispatch Core (not a per-toolset `dest`, corrected 2026-08-16) unmodified
- [ ] 3.2 Confirm the JSON-RPC response is wrapped back into a `msg_type: 3` WRP response with matching `transaction_uuid` and `status: 200`
- [ ] 3.3 Confirm a malformed/unauthorized request yields a WRP response with a non-200 `status` and a JSON-RPC `error` object, not a dropped message

## 4. ACL
- [x] ~~4.1 Confirm `triage.capabilities` is reachable under the existing write-implies-read default for any identity with triage write access~~ — **removed 2026-08-16**: no longer applicable, no such method exists
- [x] ~~4.2 Add a read-only "discovery" ACL group scoped to `triage.capabilities` only, per `triage/spec.md`'s ACL scenario~~ — **removed 2026-08-16**: the ACL scenario it referenced was removed from `specs/triage/spec.md`; superseded by task 2.5's open question about `tools/list`-level ACL filtering, which isn't triage-specific
- [x] ~~4.3 Once task 2.5 is resolved project-wide, confirm the resolution applied consistently to the `"triage"` `tools/list` entry~~ — **done 2026-08-16**: `design.md` and `specs/triage/spec.md` both reflect the confirmed two-tier model for the `"triage"` entry specifically, no triage-specific exception

## 5. Verification against spec
- [ ] 5.1 Confirm the static+dynamic merge scenario (`specs/triage/spec.md`)
- [ ] 5.2 Confirm the `tools/list` inclusion scenario — a `"triage"` entry appears, `inputSchema` has one `oneOf` branch per internal triage sub-plugin method, `methods` carries per-method metadata, for an authorized caller (`specs/triage/spec.md`, updated 2026-08-16)
- [ ] 5.3 Confirm the "no bespoke discovery method" scenario — a `triage.capabilities` JSON-RPC call returns a method-not-found error, not a real response
- [ ] 5.4 Confirm the unauthorized-caller scenario — a caller with no grant on `triage` sees `{"name": "triage", "access_restricted": true}` in `tools/list`, not full detail and not a missing entry (`specs/triage/spec.md`, added 2026-08-16)
- [ ] 5.5 Confirm this phase does not alter `capability-sync/spec.md`'s existing push behavior (regression check)

## 6. Readiness for `/opsx:apply`
- [x] ~~6.1 Both corrections `define-toolset-as-mcp-tool-model/tasks.md` §4.2 required are now addressed~~ — **done**: plane-vs-toolset needed no change; bespoke discovery method → generic `tools/list` is done. **Also done 2026-08-16**: the two follow-on questions this correction itself surfaced (§2.4/§2.5) are now confirmed and reflected in this change's own docs. Nothing known-open remains blocking this change specifically — still needs a real review pass (proposal/design/tasks/spec read end to end) before `/opsx:apply`, not just a design-consistency check
- [ ] 6.2 Once applied, update `ROADMAP.md`'s status note for this change from "ready for review" to "applied"
