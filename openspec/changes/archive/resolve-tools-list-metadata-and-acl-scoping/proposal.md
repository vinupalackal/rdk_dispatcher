# Proposal: `tools/list` Per-Method Metadata Placement and ACL Scoping

**APPLIED 2026-08-16.** This change's deltas are merged into
`openspec/specs/dispatch-core/spec.md` ("`tools/list` visibility is
two-tier, scoped by the caller's existing ACL grant") and
`openspec/specs/toolset-lifecycle/spec.md` ("Descriptive per-method
metadata is a sibling field, not embedded in a `tools/list` entry's
`inputSchema`"). Applied after `define-toolset-as-mcp-tool-model`. This
directory is retained as historical record; the base spec files are
the current source of truth going forward.

## Intent

Two project-wide design questions surfaced while correcting
`add-triage-skillset-mapping-phase1` (2026-08-16) to use
`define-toolset-as-mcp-tool-model`'s generic `tools/list` mechanism
instead of a bespoke discovery method: neither `define-toolset-as-mcp-tool-model`
nor `dispatch-core/spec.md` says (1) where descriptive per-method
metadata (`load_type`, `version`, `timeout_ms` — first needed by
triage, but not triage-specific) lives in a `tools/list` entry, or (2)
whether `tools/list`'s catalog is filtered per caller's ACL scope or
shown in full to anyone who can reach it. Both apply to every
toolset's `tools/list` entry, not just triage's — this proposal
answers them once, project-wide, rather than letting triage (or the
next toolset that needs the same thing) decide alone.

**Status: confirmed 2026-08-16, by direct instruction ("confirm the
approach as-is").** Both decisions in `design.md` (the sibling
`methods` field; the two-tier ACL-scoped `tools/list` visibility
model) are adopted as designed. Not yet applied/archived — the spec
deltas below still need `/opsx:apply` before they merge into
`dispatch-core/spec.md` and `toolset-lifecycle/spec.md`. See
`OPEN_QUESTIONS.md` A17.

## Scope

In scope:
- Where per-method descriptive metadata lives in a `tools/list` entry
  — a sibling structured field, kept separate from `inputSchema`'s
  argument-shape branches, rather than embedded inside them.
- Whether `tools/list`'s response content is filtered by the caller's
  ACL grants, and if so, at what granularity (whole-toolset visibility
  vs. per-toolset detail visibility).
- Amending `define-toolset-as-mcp-tool-model/design.md`'s `tools/list`
  decision and `add-triage-skillset-mapping-phase1`'s corrected
  `design.md` to reference this resolution instead of carrying their
  own ad hoc answers (the `x-rdk-*`-in-`inputSchema` idea and the
  "not resolved" ACL note, respectively).

Out of scope:
- Any change to `tools/call`'s existing ACL enforcement (FR-4, the
  single checkpoint) — unaffected, already settled.
- Any change to `capability-sync`'s push mechanism or its payload
  shape — unaffected; this proposal's metadata placement is about
  what a live `tools/list` caller sees, not what the cloud's Device
  Model Mapping/Tool Catalog receives via push.
- Deciding the exact ACL group/grant model for a "discovery" tier —
  reuses the existing named-permission-group and write-implies-read
  mechanics from `acl-policy-store/spec.md` unchanged, doesn't invent
  a new one.

## Why this needs to be answered project-wide

Both questions are structural, not toolset-specific: any toolset with
more than one method and any mix of ACL grants across callers hits the
same two questions triage did. Leaving them as ad hoc, per-toolset
decisions risks each toolset's `tools/list` entry shaping its metadata
differently (breaking any generic tooling that reads `tools/list`
across toolsets) or, worse, some toolsets leaking full schema detail
to unauthorized callers while others don't, with no single place that
states which behavior is correct.
