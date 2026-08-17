# Tasks

## 1. Confirmation
- [x] 1.1 ~~Get explicit confirmation (or a redirect) on both decisions in `design.md`~~ — **confirmed 2026-08-16**, by direct instruction ("confirm the approach as-is"). Both decisions adopted as designed

## 2. Spec updates — drafted as this change's own deltas, not yet applied/archived
- [x] 2.1 `dispatch-core/spec.md`: two-tier `tools/list` visibility requirement drafted — see `specs/dispatch-core/spec.md` in this change. **Not yet merged into the base spec file** — that's `/opsx:apply`'s job, not done in this pass
- [x] 2.2 `toolset-lifecycle/spec.md`: optional `methods` sibling-field requirement drafted — see `specs/toolset-lifecycle/spec.md` in this change. **Not yet merged into the base spec file**, same as 2.1

## 3. Downstream updates — done 2026-08-16
- [x] 3.1 `add-triage-skillset-mapping-phase1/design.md`: replaced the `x-rdk-*`-in-`inputSchema` example with the `methods`-sibling-field shape; replaced both "not resolved" notes (metadata placement, ACL scoping) with confirmation pointers to this change
- [x] 3.2 `add-triage-skillset-mapping-phase1/specs/triage/spec.md`: updated — the "`tools/list` includes the triage entry" scenario unconditionally asserted full detail, which would be false for an unauthorized caller under the two-tier model; qualified it to an authorized caller and added a new scenario for the unauthorized case. Also updated the removed "Single ACL checkpoint" requirement's explanation, which still called this an open question
- [x] 3.3 `add-triage-skillset-mapping-phase1/tasks.md`: §2.4/§2.5 marked resolved, pointing here

## 4. Verification against spec
- [ ] 4.1 Confirm scenario: a `tools/list` caller with no grant on a toolset still sees that toolset's name, with `access_restricted: true` and no `inputSchema`/`methods` detail
- [ ] 4.2 Confirm scenario: a `tools/list` caller with write access to a toolset sees its full `inputSchema` and `methods` detail (write-implies-read)
- [ ] 4.3 Confirm scenario: a `tools/list` caller with a read-only "discovery" grant on a toolset sees full detail without gaining write/execute via `tools/call`
- [ ] 4.4 Confirm scenario: a generic MCP client that ignores unknown top-level fields still correctly parses a `tools/list` entry that includes `methods` — the field is additive, not schema-breaking
- [ ] 4.5 Size the O(loaded toolsets) ACL query cost per `tools/list` call once a real toolset count exists to test against (flagged in `design.md` as not yet a proven concern, not assumed safe by construction)
