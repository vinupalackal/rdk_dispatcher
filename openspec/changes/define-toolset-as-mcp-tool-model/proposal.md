# Proposal: Expose Toolsets as MCP Tools, and Require Out-of-Process Execution for Dynamically-Pushed Toolsets

## Intent

The architecture's cloud region has always had an "MCP Gateway" box,
but nothing in the device-side design was actually shaped to speak
MCP — Schema & Discovery's `capabilities()`/`schema()` output and
Phase 1's `triage.capabilities` method were both bespoke JSON shapes.
This change makes the device MCP-native: Dispatch Core implements the
standard MCP tool primitives (`tools/list`, `tools/call`,
`notifications/tools/list_changed`) over the existing JSON-RPC 2.0/WRP
transport (FR-1), so any MCP-compliant cloud agent — not just this
project's own Cloud Tool & Skill Platform — can discover and invoke
toolsets directly.

It also closes the toolset half of `docs/19`'s process-model finding.
`define-plane-vs-toolset-model` already settled that planes stay
in-process. This change settles the toolset side explicitly: a
toolset meant to be dynamically pushed and cloud-invocable (the
rpcd-script-plugin-style extensibility this project wants) must run
out-of-process — the same conclusion FR-13/FR-14 already required, now
justified independently by the MCP/dynamic-push use case, not just by
spec compliance.

## Scope

In scope:
- MCP method surface on Dispatch Core: `initialize`, `tools/list`,
  `tools/call`, `notifications/tools/list_changed`.
- Mapping each toolset method to one MCP tool definition (name,
  description, JSON input schema), sourced from that toolset's own
  `schema()` — consistent with the existing "self-described, not
  centrally duplicated" principle in `toolset-lifecycle/spec.md`.
- Requiring `tools/call` to route through the exact same ACL/Execution
  Framework/sandboxing path as any other command — not a second,
  parallel command surface.
- Requiring the toolset push/discovery mechanism to be out-of-process,
  and stating which of the two precedent models (rpcd-script
  fork/exec-per-call, or RBUS-provider-style persistent process) this
  design adopts and why.
- Reaffirming that RDM Client's manifest/signature verification (FR-12)
  still gates a pushed toolset before it's registered or exposed via
  `tools/list`, regardless of how lightweight the push mechanism is.

Out of scope:
- Adopting MCP's resource or prompt primitives — only tools are in
  scope here; whether config/state-reads should instead be modeled as
  MCP resources is flagged as an open question, not decided.
- Whether MCP fully replaces generic JSON-RPC 2.0 command traffic or
  coexists alongside it for non-agent cloud/ops clients — flagged as
  an open question, not decided.
- Implementation code.
- A second, required correction to `add-triage-skillset-mapping-phase1`:
  `triage.capabilities` is superseded by `tools/list` (see
  `tasks.md` §4) — tracked, not applied here, and layered on top of
  the correction `define-plane-vs-toolset-model` already required.
