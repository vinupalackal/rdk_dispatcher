# Review: Architecture, Specs, and Code — Findings and Gaps

This is a point-in-time review of everything in this project as of
Phase 1's `add-triage-skillset-mapping-phase1` change: docs 16–18, the
consolidated `RDK_Dispatcher_Architecture_and_Requirements.md`, the
architecture SVG, all six `openspec/specs/` domains, both
`openspec/changes/`, `CLAUDE.md`, `CLAUDE_CODE_WORKFLOW.md`, the
`.claude/` scaffold, and every file in `reference-impl/`.

## 1. What's solid

The core design chain — doc 16 (why `rpcd` can't port as-is) → doc 17
(dispatcher/plugin cross-check against `ubus`/RBUS/Thunder) → doc 18
(the standalone architecture) → `RDK_Dispatcher_Architecture_and_Requirements.md`
→ `rdk_dispatcher_architecture.svg` → the OpenSpec domains — is
internally consistent. The five decisions the SVG's "reviewed against
rpcd/ubus" box calls out (hybrid dispatch, single ACL checkpoint,
out-of-process + sandboxed toolsets, local-client parity, separated
command/capability-sync auth) all trace cleanly through the
requirements doc's §7 decisions log and into matching FR/NFR entries
and OpenSpec scenarios. This part doesn't need rework.

## 2. Where it breaks down: the RDK-B build-out doesn't match the core spec

### 2.1 Process model conflict (the most significant finding)

FR-13/FR-14 require every toolset plugin to run out-of-process and
sandboxed (namespaces, seccomp, cgroups, dropped capabilities).
`reference-impl/dispatcher_core.c` does the opposite: it `dlopen()`s
every plugin — regardless of plane — directly into its own process,
isolated only by `run_with_timeout()`. `CLAUDE_CODE_WORKFLOW.md`
already flags this as an open thread, but only for the triage plane
specifically; it's actually true of the whole reference-impl. Until
this is resolved, the RDK-B build-out and the main spec describe two
different plugin execution models under the same project name.

### 2.2 Plane vs. toolset — two undeclared, competing taxonomies

The main spec partitions plugins by toolset domain (common, network,
wifi, DOCSIS, vendor — routed by Plugin Manager, each its own
out-of-process unit). `CLAUDE.md` and `plugin_contract.h` partition by
plane (config-apply, management, control, triage — a lifecycle-phase
axis). Nothing states how these relate. Phase 1's design assumed
"triage" is its own toolset-like process to make forward progress —
that was a working assumption made to unblock the change, not a
ratified decision, and it's the direct cause of finding 2.1 also
showing up inside Phase 1's own design.

### 2.3 No ACL path in the reference-impl

FR-4 makes Dispatch Core the single mandatory ACL checkpoint for every
request. Nothing in `reference-impl/` or `CLAUDE.md` shows or mentions
an ACL check anywhere in the sysevent/Netlink handling path.

### 2.4 No cloud/JSON-RPC entry point sketch

`reference-impl/dispatcher_core.c` only shows local, kernel-sourced
event dispatch (a sysevent/Netlink name in, `handle()` out). Nothing
shows Dispatch Core receiving a cloud JSON-RPC request and resolving
it to a plugin via Plugin Manager — the actual §5.1 command flow the
rest of the architecture is built around. Phase 1's `design.md`
bridges this on paper for `triage.capabilities` specifically, but
there's no corresponding code sketch, and no code path for the general
case.

### 2.5 Portability claim under strain

NFR-1 requires Dispatch Core, Plugin Manager, Execution Framework, and
ACL Policy Store to be identical across RDK-B and RDK-V, with only the
adapter layer differing. `CLAUDE_CODE_WORKFLOW.md`'s embedded
C/CCSP/HAL/sysevent/Netlink/Yocto stack is deeply RDK-B-specific in
ways that don't obviously carry to RDK-V's IARM/Thunder world. Already
flagged as open in `USER_GUIDE.md`; this review confirms it's a live
tension, not a hypothetical one, and it's coupled to finding 2.1 —
resolving what a "plane" is also bears on whether this code is
"Dispatch Core" (claimed portable) or something narrower.

## 3. Smaller items, each worth a explicit decision

- **WRP addressing is illustrative.** Phase 1's `mac:.../rdk-dispatcher/triage`
  and `dns:skillset-mapper.xmidt.example.com/svc` were not checked
  against real Parodus service-registration conventions.
- **"Static and dynamic, merged, from day one" may be more than Phase 1
  needs.** `CLAUDE_CODE_WORKFLOW.md` §10 originally framed static vs.
  dynamic as a *sequential* maturity choice (start compiled-in, migrate
  later), not a same-response requirement. Phase 1's design requires
  both simultaneously — reasonable, but an assumption, not a confirmed
  requirement.
- **RDM Client's install/rollback unit is a whole toolset package**
  (FR-11/FR-12), but Phase 1 introduces plugin-level granularity
  (static vs. dynamic *within* one toolset). `toolset-lifecycle/spec.md`
  doesn't yet address sub-toolset install granularity.
- **`.claude/settings.json`'s hooks** reference `dispatcher_handlers.c`
  and `dispatcher_triage.c`, which don't exist anywhere in this repo —
  harmless now, but confirms nothing here is implemented yet, only
  designed.
- **Security review (NFR-3/NFR-4)** has still not happened. Nothing in
  this project should be treated as production-ready regardless of how
  complete the specs look until it does.

## 4. Disposition

**Updated 2026-08-13.** Findings 2.1 and 2.2 are resolved together in
`openspec/changes/define-plane-vs-toolset-model/` — they turned out to
be the same underlying question, and the answer (revised once, per
direct instruction) is simpler than either finding anticipated: keep
the toolset architecture — out-of-process, sandboxed — for all four
planes. Plane is a descriptive tag, not a separate execution-trust
tier. This means finding 2.1's `reference-impl/dispatcher_core.c`
in-process model is confirmed non-compliant and needs rework (tracked
in that change's `tasks.md` §3), and it means
`add-triage-skillset-mapping-phase1`'s *original* "Triage Toolset
process" design needed no correction after all — an earlier revision
attempt in this project's history briefly "corrected" it toward a
now-obsolete aggregation model before this simpler answer was settled;
that revision was itself rolled back, and Phase 1's original design
stands unmodified. Findings 2.3, 2.4, and 2.5, and the smaller items
in §3, remain open and are sequenced in `USER_GUIDE.md`'s next-steps
list and `OPEN_QUESTIONS.md`.
