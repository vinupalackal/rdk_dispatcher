# Dispatcher Protocol Conventions

Companion to `ccsp-component-conventions` — that skill covers CCSP
sysevent-handler code; this one covers Dispatch Core's own request
path: encryption, message routing, MCP exposure, and toolset push.
Sourced from the OpenSpec changes under `openspec/changes/`:
`define-plane-vs-toolset-model`, `define-toolset-as-mcp-tool-model`,
`define-synchronous-toolset-push`, and
`require-payload-encryption-and-message-routing`. Read those (and
their linked `openspec/specs/` deltas) for the full rationale behind
each rule below — this file is the terse, apply-it-while-coding
version.

## Every request, before anything else
1. Decrypt the WRP payload first, before any routing or ACL check.
   Payload encryption sits under TLS/WSS transport, not instead of it
   — both layers are required on every payload, definition and
   command alike.
2. Classify the decrypted payload's **message kind** explicitly:
   `definition` (schema/manifest, no execution) routes to Plugin
   Manager's load path; `command` routes through local JSON-RPC IPC
   into Execution Framework. Never conflate message kind with a
   plugin's own `load_type` (compiled-in vs. `dlopen()`) — same words
   used informally in the reference diagram, two different axes in
   the spec. Say "message kind" for one, "load_type" for the other,
   in code and comments alike.
3. Decryption success is not authorization. ACL still evaluates every
   command exactly as it would an unencrypted one.

## MCP tool surface
4. `tools/list` entries are sourced directly from a toolset's own live
   `schema()` — never a separately maintained copy that can drift.
5. `tools/call` goes through the identical ACL/Execution
   Framework/sandbox path as any non-MCP JSON-RPC command. Adding a
   new entry point is exactly where a second, inconsistent
   authorization check tends to sneak in by accident — don't.
6. `toolset.push` is a management method, not an MCP tool — never
   surface it via `tools/list`. It requires its own dedicated
   `toolset-publish`-class ACL scope, never satisfied by a scope that
   only grants `tools/call` read/write on a toolset's methods.

## Toolset push
7. Verify signature and manifest policy inline, synchronously — same
   check RDM Client applies to its own install path, just relocated,
   never a weaker one.
8. Keep the prior toolset version live until the new one passes its
   health check; auto-revert on failure. Never drop the old version
   before the new one is confirmed healthy.
9. This path is for plugin-scale artifacts only. RDM Client's async
   download/verify/install flow remains required for firmware/large
   packages — don't route large artifacts through `toolset.push`.

## Plane vs. toolset
10. A plane (config-apply, management, control, triage) is first-party
    Dispatch Core logic, in-process, never independently distributed.
    A toolset (common, network, wifi, DOCSIS, vendor, ...) is
    out-of-process, sandboxed, and Toolset-Store/RDM-Client
    installable. A single toolset may implement logic across multiple
    planes internally — plane describes what code does, not a
    separate routable or installable unit. Never sandbox a plane or
    skip sandboxing a toolset because of which plane its logic touches.
