# Delta for dispatch-core

## ADDED Requirements

### Requirement: Phased rollout — type hierarchy and verification from Phase 1, encryption added in Phase 2
Phase 1 SHALL carry WRP payloads as plain JSON-RPC, always including a
`type` field (`static` for a definition/no-commands message, `dynamic`
for a message carrying a command to execute) — this project's
wire-level name for the message-kind classification below. Phase 1
SHALL NOT defer signature/manifest verification for either `static` or
`dynamic` messages regardless of encryption being deferred. Phase 2
SHALL add payload-level encryption on top of this same `type`
structure, per the Payload-level encryption requirement below —
encryption is additive to Phase 1's shape, not a redesign of it.

#### Scenario: Phase 1 message is classified and verified without encryption
- GIVEN a Phase 1 WRP message with `type: "dynamic"` carrying a command
- WHEN Dispatch Core receives it
- THEN it is classified by `type`, and its signature/manifest are
  verified exactly as they would be in Phase 2 — only payload
  encryption is absent, nothing else is relaxed

### Requirement: Payload-level encryption under transport TLS (Phase 2)
Every WRP payload SHALL be encrypted before being placed in the WRP
envelope and sent over the TLS/WSS transport. Dispatch Core SHALL
decrypt the payload immediately on receipt, before any further
processing. This applies uniformly to definition (`toolset.push`-style)
and command payloads alike, once Phase 2 implements it.

#### Scenario: Payload is encrypted independently of transport security
- GIVEN a WRP message sent over an established TLS/WSS connection
- WHEN the message is constructed
- THEN its payload is encrypted at the payload level in addition to,
  not instead of, the transport-level TLS protection

#### Scenario: Decryption precedes all other processing
- GIVEN a WRP message arriving at Dispatch Core
- WHEN it is received
- THEN the payload is decrypted before message-kind routing, ACL
  evaluation, or any other processing step begins

### Requirement: Message-kind routing after payload is available
The system SHALL classify every received payload (plain in Phase 1,
decrypted in Phase 2) by its `type` field as either a **definition**
(`static`) message (protocol + toolset schema/manifest, no execution
request) or a **command** (`dynamic`) message (protocol + toolset +
an execution request), and route definition messages to Plugin
Manager's load path and command messages through local JSON-RPC IPC
into Execution Framework. This classification SHALL NOT be conflated
with a toolset plugin's own `load_type` (compiled-in vs. `dlopen()`),
which is an independent, per-plugin attribute unrelated to which kind
of WRP message delivered it.

#### Scenario: Definition message never reaches Execution Framework
- GIVEN a `static` (definition-kind) payload, encrypted or not
  depending on phase
- WHEN Dispatch Core routes it
- THEN it reaches Plugin Manager's load path only — Execution
  Framework is never invoked for a definition message

#### Scenario: Encryption does not substitute for authorization
- GIVEN a successfully decrypted command-kind payload from an identity
  lacking permission for the target method
- WHEN Dispatch Core evaluates the request
- THEN the ACL Policy Store denies it exactly as it would deny an
  equivalent request that arrived unencrypted — successful decryption
  confirms confidentiality was preserved, not that the caller is
  authorized
