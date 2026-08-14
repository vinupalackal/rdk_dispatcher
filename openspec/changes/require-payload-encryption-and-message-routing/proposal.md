# Proposal: Require Payload-Level Encryption, and Formalize Message-Kind Routing

## Intent

You supplied a diagram fixing the shape of every WRP interaction: a
static (definition) push and a dynamic (command) push both get their
payload encrypted before entering TLS/WSS transport and the WRP
envelope; the device decrypts on receipt and routes by message kind —
static to Plugin Manager's load path, dynamic through local JSON-RPC
IPC into Execution Framework. This change adopts that architecture as
written and resolves `OPEN_QUESTIONS.md` C7 (whether payload
encryption is required beyond transport TLS): it is.

It also heads off a naming collision. This diagram's "static/dynamic"
describes a WRP *message kind* (does this payload define a toolset, or
does it carry a command to run). `reference-impl/plugin_contract.h`'s
`load_type` field already uses "static"/"dynamic" for a different
axis — how a plugin binary got loaded (compiled-in vs. `dlopen()`).
Both axes are real and both stay, but they must not be conflated in
spec text or in code: a compiled-in (`load_type: static`) plugin can
be the target of either a definition push or a command push, and the
same is true of a `dlopen()`'d one.

## Scope

In scope:
- Payload-level encryption as a required layer under TLS/WSS
  transport, for both `toolset.push`-style definition pushes and
  ordinary command traffic — not scoped to one or the other.
- Formalizing "message kind" (definition vs. command) as the routing
  decision Dispatch Core makes immediately after decryption, before
  ACL evaluation.
- Explicitly disambiguating message-kind from `load_type` in the spec
  text, so implementers don't collapse the two.
- Amending `define-synchronous-toolset-push/design.md`'s earlier
  assumption ("signing is sufficient, payload encryption not assumed
  necessary") — this change supersedes that specific point with your
  direction; the rest of that change (synchronous accept/reject,
  dedicated ACL scope, health-check rollback) is unaffected.

**Phasing, added 2026-08-13 per direct confirmation:** encryption
itself is Phase 2 work. Phase 1 carries payloads as plain JSON-RPC
with the `static`/`dynamic` type field always present, and
signature/manifest verification mandatory for both — only
confidentiality/integrity-via-encryption is deferred, nothing about
classification or verification is relaxed. See `design.md`'s "phased
rollout" decision and `ROADMAP.md`.

Out of scope:
- The specific encryption algorithm/key-management scheme (symmetric
  vs. asymmetric, key distribution, rotation) — tracked as a task, not
  decided here.
- Any change to the MCP tool-call model (`define-toolset-as-mcp-tool-model`)
  or the plane/toolset taxonomy (`define-plane-vs-toolset-model`) —
  this change is about the wire-level envelope and routing step that
  happens before either of those become relevant.
