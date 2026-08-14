# Using OpenSpec on rdk-dispatcher, and What to Do Next

This guide covers two things: how to actually run this project's
development through [OpenSpec](https://github.com/Fission-AI/OpenSpec)
day to day, and the prioritized next steps to get from "designed" to
"implemented and reviewed."

## 1. Install and initialize (one-time, in your terminal)

```sh
npm install -g @fission-ai/openspec@latest
cd rdk-dispatcher
openspec init
```

`openspec init` expects to manage an `openspec/` directory at your
project root — this folder already has one, seeded from
`RDK_Dispatcher_Architecture_and_Requirements.md`. Point `openspec
init` at this existing `openspec/` rather than letting it scaffold an
empty one; consult OpenSpec's own
[Using OpenSpec in an Existing Project](https://github.com/Fission-AI/OpenSpec/blob/main/docs/existing-projects.md)
guide for the exact adoption flow, since it's designed for exactly
this brownfield-with-pre-existing-specs situation.

## 2. The day-to-day loop (in your AI assistant's chat, not the terminal)

```
/opsx:explore                    (optional — think through an unclear idea first)
/opsx:propose <change-name>      (AI drafts proposal + delta specs + design + tasks)
/opsx:apply                      (AI implements the tasks)
/opsx:verify                     (checks completeness/correctness/coherence against the spec)
/opsx:archive                    (merges deltas into openspec/specs/, files the change away)
```

`openspec/changes/define-sat-token-format/` in this folder is a
complete worked example of this loop already drafted — it resolves
one of this project's own open questions (the SAT token format) as a
real change: read its `proposal.md`, `design.md`, `tasks.md`, and
`specs/dispatch-core/spec.md` delta to see the shape before you run
`/opsx:apply` and `/opsx:archive` on it yourself.

## 3. How our six domains map to OpenSpec's model

| `openspec/specs/<domain>/` | Covers |
|---|---|
| `dispatch-core` | Transport, single ACL checkpoint, stateless sessions, local-client parity |
| `acl-policy-store` | Groups, deny rules, write-implies-read, hot reload, audit |
| `sandboxed-runtime` | Namespaces, seccomp, cgroups, capability dropping per plugin |
| `platform-adapters` | RBUS / IARM-Thunder / CLI separation, identity propagation |
| `capability-sync` | Device→cloud reporting, event-triggered, device-identity auth |
| `toolset-lifecycle` | Plugin Manager's coarse registry, self-described schemas, Toolset Store/RDM Client |

Every requirement in each `spec.md` traces back to a specific FR/NFR
in `RDK_Dispatcher_Architecture_and_Requirements.md` §6, and every
decision behind it is recorded in that document's §7 decisions log —
if a requirement's rationale isn't obvious from its scenario, that's
where to look.

**Use OpenSpec's "Full spec" mode, not the default Lite mode, for any
change touching `acl-policy-store` or `sandboxed-runtime`.** These map
directly to NFR-3 (least privilege) and NFR-4 (no confused deputy) —
the two requirements our own design work flagged as needing
independent security verification, not just a design pass. Lite mode
is fine for everything else.

## 4. Next steps for development, in priority order

**-1. Apply `define-plane-vs-toolset-model` first.** A full review
(`docs/19_architecture_and_code_review_findings.md`) found that
`add-triage-skillset-mapping-phase1`'s "Triage Toolset process" design
was built on an unstated assumption — that triage is a toolset — which
conflicts with `CLAUDE.md`'s plane model. `define-plane-vs-toolset-model`
resolves this (triage is a plane, not a toolset; planes are dispatcher-core-internal
and not FR-13/FR-14-governed, toolsets are). Its own `tasks.md` §3
lists the exact edits `add-triage-skillset-mapping-phase1` needs as a
result — do those before item 0 below, not after.

**0. Then run `/opsx:apply` on `add-triage-skillset-mapping-phase1`.**
Once corrected per the item above, this change scopes the first
buildable slice: a cloud-initiated `triage.capabilities`
request/response carried in WRP with a JSON payload, aggregating
triage-plane capabilities across Dispatch Core's own plane plugins and
every loaded toolset's own reported planes. It deliberately excludes
triage evidence capture/execution — see its `proposal.md` "Out of
scope."

**1. Bootstrap the rest of the spec history properly.** The `specs/*.md` files here
were hand-authored directly from the architecture doc, not produced
through an actual `/opsx:propose` change — there's no proposal or
audit trail behind the starting point. Before building anything else,
run this as its own change (e.g. `add-initial-dispatcher-specs`,
all-`ADDED` deltas matching what's already in `specs/`) so even the
baseline has a recorded rationale, consistent with everything that
follows it.

**2. Decide the implementation language and runtime for Dispatch
Core.** `CLAUDE_CODE_WORKFLOW.md` now has a concrete answer for the
RDK-B side — embedded C99, CCSP/HAL, sysevent/Netlink, Yocto/bitbake —
with a working Claude Code scaffold (`.claude/`) and an illustrative
plugin-loader sketch (`reference-impl/`) already in this folder. Two
things this doesn't yet settle: whether the same choice applies to
RDK-V, and whether the sketch's timeout-based plugin isolation is
meant to satisfy this project's own sandboxing requirements
(FR-13/FR-14, NFR-2/NFR-3) or is a deliberate, lighter-weight
divergence for the embedded RDK-B target specifically — see
`CLAUDE_CODE_WORKFLOW.md`'s "Open threads" section. Resolve that as
its own `/opsx:propose` change before treating the sketch as anything
more than a starting point.

**3. Turn the remaining open questions into changes.** From
`RDK_Dispatcher_Architecture_and_Requirements.md` §8, four are still
unresolved (the SAT token question is already handled in the worked
example):
   - RBUS and Thunder per-toolset translation logic (which TR-181
     parameters or Thunder plugins each toolset actually maps to)
   - Sandbox profile authoring workflow (who writes/reviews a
     toolset's seccomp/capability manifest, and how a profile changes
     post-install)
   - Footprint budget, sized against real target hardware alongside
     RDK's existing CCSP/Thunder process load
   - The security review itself

**4. Schedule the security review specifically against NFR-3 and
NFR-4.** This is the one item on this list that isn't just "write
another OpenSpec change" — it requires an actual security team
independently verifying least-privilege sandboxing and the
no-confused-deputy adapter requirement, not a design-level check.
Nothing in this project should be considered production-ready until
this happens, regardless of how complete the specs and implementation
otherwise look.

**5. Settle the module's public name.** Everything here still refers
to the component generically as "RDK Dispatcher." If a name like *RDK
Switchboard* gets adopted, doing that rename pass sooner rather than
later avoids it spreading through more specs, code, and documentation
first.
