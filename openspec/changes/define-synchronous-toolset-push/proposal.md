# Proposal: Synchronous Toolset Push Over the Existing JSON-RPC/WRP Channel

## Intent

RDM Client (FR-12) is async by design: queue, download over HTTP(S),
verify post-download, install/reboot as needed — correct for
firmware-class artifacts, a poor fit for "push a small toolset plugin
and know synchronously whether it landed." This change adds a second,
size-bounded push path for plugin-scale artifacts: the artifact and
its signature travel in a JSON-RPC request over the same WRP/XMiDT
channel Dispatch Core already terminates, verified inline, with a
synchronous accept/reject response. RDM Client is unchanged and
remains the required path for firmware/large-package installs — this
does not replace it, it adds a lighter path for a class of artifact
RDM was never well-suited to.

This builds on, and does not contradict, `define-toolset-as-mcp-tool-model`'s
decision that verification stays mandatory regardless of push
convenience. The policy is unchanged; what moves is *where* the
verification happens for this specific artifact class — inline in
Dispatch Core rather than in RDM Client's async pipeline — and the
underlying signature/manifest check is the same one RDM already
performs.

## Scope

In scope:
- A new `toolset.push` JSON-RPC method, synchronous request/response,
  over the existing transport.
- Inline signature verification in Dispatch Core, reusing the same
  policy RDM Client already enforces (FR-12) rather than a separate,
  divergent check.
- A dedicated, narrow ACL scope for this method, distinct from any
  scope that grants ordinary `tools/call` access.
- A defined boundary for what belongs on this path (small, plugin-scale
  artifacts) versus RDM's path (firmware/large packages) — policy-based,
  not a single hardcoded size number invented here.
- A rollback story: Plugin Manager keeps the prior toolset version live
  until the newly pushed version passes its health check, consistent
  with Plugin Manager's existing load/reload/health responsibilities.

Out of scope:
- Payload-level encryption beyond the existing XMiDT/Parodus transport
  TLS — flagged as an open question (does this project have a
  confidentiality requirement transport TLS doesn't already cover?),
  not decided here.
- ~~The exact size/artifact-type threshold separating this path from
  RDM's~~ — **resolved 2026-08-13, per direct confirmation:** the
  boundary is artifact *type*, not size. RDM Client's path is used for
  **any** toolset implementation library file or binary meant to be
  `dlopen()`'d at runtime — i.e., any `load_type: dynamic` compiled
  artifact — regardless of how small it is. `toolset.push` SHALL NOT
  be used to deliver a `dlopen()`-able binary; that always goes through
  RDM Client. See `design.md`'s "RDM Client's exclusive scope" decision
  — this also narrows what `toolset.push` itself is still for, flagged
  there as needing your confirmation, not assumed.
- Replacing or modifying RDM Client's existing responsibilities in any
  way.
- `toolset.push` is explicitly NOT exposed as an MCP tool via
  `tools/list` — it's a toolset-management operation for a
  build/release pipeline identity, not a device capability for an AI
  agent to invoke as a skill. Keeping the two surfaces separate is a
  deliberate decision (see `design.md`).
