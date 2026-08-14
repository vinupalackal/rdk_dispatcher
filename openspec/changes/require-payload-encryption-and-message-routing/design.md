# Design: Payload Encryption and Message-Kind Routing

## Technical Approach

### Decision: two layers, not one — payload encryption under TLS/WSS transport

Every WRP payload is encrypted before it's placed in the WRP envelope
and sent over the existing TLS/WSS (XMiDT/Parodus) connection. This is
deliberate defense in depth, per your diagram: transport TLS protects
the connection hop-by-hop; payload encryption protects the content
even from infrastructure that terminates or inspects the transport
layer (Parodus, XMiDT routing components) without needing to trust
them with plaintext. This applies uniformly — it is not specific to
`toolset.push`; ordinary command payloads are encrypted the same way.

This supersedes the assumption in `define-synchronous-toolset-push/design.md`
("transport TLS covers confidentiality... payload encryption is not
assumed necessary") for the confidentiality question specifically.
That change's other decisions (synchronous accept/reject, dedicated
ACL scope, health-check-gated rollback) are unaffected and still hold.

### Decision: message kind is decided immediately after decryption, before ACL

Once Dispatch Core decrypts a payload, it reads the message kind —
**definition** (protocol + toolset schema/manifest, no execution
request) or **command** (protocol + toolset + the actual command to
run) — and routes accordingly:

- **Definition** → Plugin Manager's load path (`toolset-lifecycle/spec.md`,
  and the `toolset.push` method from `define-synchronous-toolset-push`
  specifically when the definition arrives via that method rather than
  RDM Client).
- **Command** → the existing local JSON-RPC IPC boundary (§4.6) into
  Execution Framework, i.e. the unchanged §5.1 command-execution flow
  (or `tools/call`, per `define-toolset-as-mcp-tool-model`).

Decryption happens before Dispatch Core's single ACL checkpoint (FR-4)
runs — encryption is a wire-level confidentiality control, not an
authorization mechanism, and does not change where or how ACL is
evaluated. A decrypted-but-unauthorized command is still denied
exactly as an unencrypted one would be.

### Decision: phased rollout — plain JSON-RPC with type hierarchy in Phase 1, encryption added in Phase 2

**Added 2026-08-13, per direct confirmation.** Encryption itself is
deferred to Phase 2. Phase 1 carries both toolset definitions and
commands in **plain JSON-RPC**, with the `type` field (`static` |
`dynamic` — this change's wire-level name for the message-kind axis
below) always present, so the device always knows whether a message
is a definition (`static`, no commands) or carries a command to
execute (`dynamic`, command payload present). What's phased is
confidentiality/integrity protection, not the classification itself —
message-kind routing and signature/manifest verification both apply
from Phase 1 onward, for `static` and `dynamic` messages alike;
neither is weakened by deferring encryption. Phase 2 adds payload
encryption on top of this same `static`/`dynamic` structure, per the
"Payload-level encryption" decision below — encryption is additive,
not a redesign of the message shape.

**Why encryption specifically, not verification, is what's deferred:**
signature/manifest verification (RDM Client's existing check, or
`toolset.push`'s inline equivalent) protects against a tampered or
unauthorized artifact — that risk exists from the first message this
project ever sends, so it was never a candidate for deferral.
Encryption protects confidentiality and, via an authenticated cipher,
integrity/corruption detection on top of what signing already
provides — valuable, but layerable on afterward without changing
`static`/`dynamic` classification, ACL evaluation, or verification
logic already built for Phase 1.

### Decision: "message kind" and `load_type` are different axes — naming fixed now to prevent drift

To avoid the collision:

| Axis | Values | Answers | Lives in |
|---|---|---|---|
| Message kind | `definition` \| `command` | What is this WRP payload for? | The decrypted payload's own envelope/header, read by Dispatch Core |
| `load_type` | `static` \| `dynamic` | How was this specific plugin binary loaded — compiled-in, or `dlopen()`'d? | `plugin_contract.h`'s `plugin_descriptor_t`, reported per plugin |

Spec text and code should say "message kind: definition/command" for
the WRP-level distinction and reserve "static/dynamic" strictly for
`load_type`, even though your diagram's own labels ("Static: ...",
"Dynamic: ...") used the same words for the message-kind axis
informally. The two are independent: a `load_type: static` (compiled-in)
plugin can be the target of a `command`-kind message just as easily as
a `load_type: dynamic` one, and either kind of plugin can be
registered via a `definition`-kind message.

## File/Component Changes

- Dispatch Core: decrypt payload immediately on receipt, before any
  other processing; read message kind and route to Plugin Manager
  (definition) or Execution Framework via local JSON-RPC IPC (command).
- `define-synchronous-toolset-push`: its `toolset.push` method is the
  concrete mechanism by which a `definition`-kind message reaches
  Plugin Manager outside of RDM Client's own path — cross-referenced,
  not duplicated.
- No change to ACL evaluation order or the single-checkpoint principle
  (FR-4) — encryption sits strictly before it in the pipeline.
