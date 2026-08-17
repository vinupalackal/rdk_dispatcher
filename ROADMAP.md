# RDK Dispatcher — Phased Roadmap

Groups this project's work into build phases. `OPEN_QUESTIONS.md`
tracks individual decisions; this file tracks which phase each
decision (and each chunk of implementation work) belongs to. Add to a
phase's list as items get scoped — don't let a phase assignment live
only in chat.

## Phase 1: Triage Capability Discovery

**Status: applied 2026-08-16.** Spec now lives in
`openspec/specs/triage/spec.md`. A cloud-initiated, read-only
`tools/list` query surfaces the device's triage-plane capabilities
(plain, unencrypted JSON-RPC per Phase 1's A7 scoping — payload
encryption is Phase 2, not this phase), via the generic `tools/list`
mechanism `define-toolset-as-mcp-tool-model` established (also
applied — `openspec/specs/dispatch-core/spec.md`/
`openspec/specs/toolset-lifecycle/spec.md`), including the confirmed
`tools/list` metadata/ACL-scoping resolution
(`openspec/specs/dispatch-core/spec.md`'s two-tier visibility
requirement, `OPEN_QUESTIONS.md` A17). Full design history and the
two corrections this change went through are preserved in
`openspec/changes/archive/add-triage-skillset-mapping-phase1/`.

**Explicitly not required for Phase 1, by design:** out-of-process
execution and sandboxing, and payload encryption. Phase 1 is read-only
self-description — see its `proposal.md`'s "Why this is a good first
phase." All out-of-process/sandboxing work and encryption are
consolidated into Phase 2 below, per direct instruction — Phase 1
should not be mistaken for an FR-13/FR-14-compliant or fully secured
implementation; it's a scoped prototype of the discovery protocol
shape, with hardening deferred on purpose.

**Explicitly required in Phase 1, not deferred:** WRP payloads as
plain JSON-RPC, always carrying a `type` field (`static` for a
definition/no-commands message, `dynamic` for one carrying a command),
and signature/manifest verification for both types — confirmed per
`OPEN_QUESTIONS.md` A5/A7. Only confidentiality/integrity-via-encryption
is deferred; classification and verification are not.

**Also in Phase 1's scope, confirmed 2026-08-13:** implementing
`dispatcher_handlers.c` and `dispatcher_triage.c` — the two files
`.claude/settings.json`'s hooks already reference but which don't
exist in the repo yet. See `OPEN_QUESTIONS.md` B10.

**Also in Phase 1's scope, confirmed 2026-08-14 — real command
execution and `toolset.push`, as an explicit, reviewed exception:**
`openspec/changes/add-phase1-command-execution-exception/`. This
reverses the earlier "all out-of-process work belongs in Phase 2"
consolidation for exactly these two pieces, narrowly:
- Real command execution (not just read-only discovery) and
  `toolset.push` both move into Phase 1.
- The toolset(s) handling them run **in-process**, as a scoped,
  tracked exception to `define-plane-vs-toolset-model`'s uniform
  out-of-process/sandboxing rule — not a silent reversion of it.
  Hardening to out-of-process/sandboxed/on-demand execution stays
  Phase 2 work (below).
- Dispatch Core's single ACL checkpoint (FR-4) is unaffected — still
  mandatory from Phase 1. Deferring sandboxing does not defer
  authorization.
- Payload encryption stays Phase 2, unchanged (A7) — confirmed not
  pulled forward alongside this.
- `toolset.push`'s RDM boundary (A6) and verification requirements are
  unchanged; only its Phase-1 rollback mechanism differs (health-check-
  gated in-process swap, not the artifact-fallback model
  `define-on-demand-toolset-execution` designed for Phase 2).
- **Newly urgent because of this:** A1 (SAT token format — real
  execution needs real authorization), C8 (`toolset.push`'s artifact
  field — Phase 1 will build against it now), and `docs/20`'s ACL
  design (B5/B6 — needs actual implementation, not just design). See
  `OPEN_QUESTIONS.md` A15.

## Phase 2: hardening Phase 1's exception, plus everything else out-of-process/sandboxed

**What's left here after the Phase 1 exception above: everything
requiring out-of-process execution or sandboxing for toolsets *other*
than Phase 1's command-execution exception, plus hardening that
exception itself once this phase starts.**

- **SAT token format and revocation mechanism (NFR-10).**
  `openspec/changes/define-sat-token-format/` — JWT tokens, 5-minute
  expiry with refresh, no revocation list; permission groups embedded
  in the token at issuance. See `OPEN_QUESTIONS.md` A1 for status
  (drafted, unarchived) and the chat explanation above for the
  reasoning in plain terms. This is real authenticated command
  execution's foundation — Phase 1 deliberately avoided needing it by
  staying read-only; Phase 2 is where that changes.

- **How does the four-plane model relate to the toolset model?**
  `openspec/changes/define-plane-vs-toolset-model/` (revised
  2026-08-13, per direct instruction: keep toolset architecture for
  all planes) — resolved as: plane (config-apply, management, control,
  triage) is a descriptive tag, not a separate execution-trust tier.
  Every plane is implemented *as* one or more toolsets — uniformly
  out-of-process and sandboxed (FR-13/FR-14), no first-party
  exemption. The cloud manages every plane identically, by naming a
  toolset and passing arguments via `tools/call`. See
  `OPEN_QUESTIONS.md` A2/A3 for status and B11 for the new,
  must-be-sized engineering tension this creates: out-of-process
  overhead on the sysevent thread, which `CLAUDE.md`'s own hard rules
  say must never block. `reference-impl/` needs a rework pass to
  actually comply — tracked in `define-plane-vs-toolset-model/tasks.md`
  §3, not done yet.

- **Should toolsets be MCP-discoverable/invocable, and does dynamic
  push require out-of-process execution?** **Confirmed and applied
  2026-08-16** — `openspec/specs/dispatch-core/spec.md`,
  `openspec/specs/toolset-lifecycle/spec.md`, and
  `openspec/specs/capability-sync/spec.md` now carry the full MCP
  tool-method surface (`tools/list`/`tools/call`/`notifications/tools/list_changed`),
  the coarse one-MCP-tool-per-toolset `tools/list` shape with its
  sibling `methods` metadata field, and the two-tier ACL-scoped
  `tools/list` visibility model (`OPEN_QUESTIONS.md` A4/A11–A14/A17).
  The device agent publishes its toolset list first, on session
  establishment — push is the primary discovery path, `tools/list` is
  a secondary, on-demand confirmation (also resolves
  `OPEN_QUESTIONS.md` C4). Dynamic push's out-of-process sandboxing is
  required in general — **except** Phase 1's specific
  command-execution/`toolset.push` exception above, which runs
  in-process for now. This phase's job for that exception is to harden
  it to the out-of-process model, not introduce out-of-process
  execution for the first time. Design history for both merged changes
  is preserved in `openspec/changes/archive/`. Actual device-side
  *implementation* (code, not spec) is still Phase 2 work.

- **Payload-level encryption for WRP messages.**
  `openspec/changes/require-payload-encryption-and-message-routing/`
  — confirmed, phased (`OPEN_QUESTIONS.md` A7): Phase 1 runs plain
  JSON-RPC with the `static`/`dynamic` type field and mandatory
  verification only; this phase adds encryption on top of that same
  structure, unchanged otherwise. Not a redesign — additive.

- **RBUS and Thunder per-toolset translation logic.** No OpenSpec
  change drafted yet — this is real engineering/mapping work, not an
  architecture decision. Scope confirmed 2026-08-13: a **full**
  mapping covering every Thunder plugin and every TR-181 namespace
  value is needed, not a partial or per-toolset-as-needed mapping. To
  be concluded as part of this phase. See `OPEN_QUESTIONS.md` B1.
  Platform Adapters (RBUS Adapter, IARM/Thunder Adapter) are the
  components this mapping ultimately feeds — see
  `RDK_Dispatcher_Architecture_and_Requirements.md` §4.8.

- **Sandbox profile authoring workflow.** No OpenSpec change drafted
  yet — who writes and reviews a toolset's seccomp/capability manifest,
  and how that manifest changes post-install without re-negotiating
  per call (per the Sandboxed Toolset Plugin Runtime's existing
  "declared at install, enforced at every launch" requirement). See
  `OPEN_QUESTIONS.md` B2. Directly relevant once out-of-process
  sandboxing work (also Phase 2) actually starts.

- **Footprint budget.** Answered 2026-08-13: **under 300KB for now.**
  Eased significantly by the on-demand execution decision directly
  below (A8) — the budget now covers concurrently-active toolsets
  only, not everything installed. Still needs real sizing before
  treating it as met. See `OPEN_QUESTIONS.md` B3.

- **Should sandboxed toolset execution be persistent or on demand?**
  `openspec/changes/define-on-demand-toolset-execution/` — **confirmed:
  on demand, not always-running.** Toolsets spawn when a request
  actually targets them; recommended (not yet confirmed) mechanism is
  idle-timeout spawn over pure per-call fork/exec. Supersedes
  `define-toolset-as-mcp-tool-model`'s "persistent supervised process"
  decision and amends `define-synchronous-toolset-push`'s rollback
  logic to retain the prior artifact as a fallback rather than keeping
  the prior process resident. See `OPEN_QUESTIONS.md` A8; the exact
  spawn mechanism is the next thing to confirm. **Not relevant to
  Phase 1's command-execution exception until this phase actually
  hardens it** — there's no process to spawn on demand while that
  toolset still runs in-process.

- **Independent security review of NFR-3 (least privilege) and NFR-4
  (no confused deputy) — Phase 2's concluding gate, not just another
  item in the list.** Everything else in this phase is design and
  implementation work; this is verification of it. Least privilege
  (NFR-3) means a toolset only gets the syscalls, capabilities, and
  filesystem access its manifest actually declares needing — enforced
  by the sandboxing work above, not just stated as intent. No confused
  deputy (NFR-4) means a Platform Adapter always checks the *original
  caller's* real permissions against RBUS's/Thunder's own ACL, rather
  than trusting Dispatch Core's earlier check and acting under one
  fixed, elevated identity for everyone — the classic trap where a
  privileged middleman gets tricked into misusing its own access on
  someone else's behalf. Design review (this project's own reasoning
  on paper) is not a substitute for independent verification (a
  separate reviewer actively trying to break the sandbox or trigger
  confused-deputy behavior against the real, implemented code) —
  failure here means unauthorized access to real device functions, a
  different category of risk than anything else on this list. Nothing
  from this phase should be considered production-ready until this
  concludes. See `OPEN_QUESTIONS.md` B4.

- **Resources vs. tools for config/management/control-plane reads.**
  `openspec/changes/archive/define-toolset-as-mcp-tool-model/` (applied
  2026-08-16) — deferred 2026-08-13 (`OPEN_QUESTIONS.md` A11): the
  triage plane keeps its
  current unified-tools model unchanged; whether the other three
  planes' reads should be split into MCP resources, distinct from
  invocable tools, is decided here, once those toolsets have a real
  design rather than being assumed.

- **WRP payload size ceiling for `toolset.push`.**
  `openspec/changes/define-synchronous-toolset-push/` — deferred
  2026-08-13 (`OPEN_QUESTIONS.md` B12): the type-based RDM boundary
  (A6) is settled; the actual numeric size limit for what
  `toolset.push` itself may carry is not, and is worked out here.

- **RDK-V portability: per-platform event namespace mapping.**
  `openspec/changes/define-plane-vs-toolset-model/` (Decision D) —
  confirmed 2026-08-13: NFR-1 stays a hard requirement, the plane
  model applies identically to RDK-V, and the CCSP/sysevent/Netlink
  coupling is isolated behind a namespace table each platform
  supplies for its toolsets' `events` entries to resolve against.
  What's real engineering work, not yet done: where that table lives,
  how it's authored, and a first worked example of an RDK-B/RDK-V
  event pair (shared namespace entry vs. divergent). See
  `OPEN_QUESTIONS.md` A9 and that change's `tasks.md` §5.

- **ACL check missing from `reference-impl/`'s command path.**
  `docs/20_acl_implementation_rpcd_technical_review_and_dispatch_core_design.md`
  — a technical review of how `rpcd` actually implements ACL
  (`session.login`/`session.access`, `acl.d/*.json`, deny-first,
  `uci` login groups), mapped to Dispatch Core's already-spec'd ACL
  Policy Store, plus a reference-impl sketch of where the check
  belongs. Addressed together with B6 (no command entry point sketch
  existed at all) since they turned out to be the same gap. See
  `OPEN_QUESTIONS.md` B5/B6.

## How to use this file

When something in `OPEN_QUESTIONS.md` or a new OpenSpec change is
ready to be sequenced, add one line here under the phase it belongs
to, with a pointer to the change/question it comes from — don't
duplicate the full reasoning, that stays in `OPEN_QUESTIONS.md` and
the change's own `proposal.md`.
