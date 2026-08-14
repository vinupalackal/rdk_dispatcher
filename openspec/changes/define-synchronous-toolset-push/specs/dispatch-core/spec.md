# Delta for dispatch-core

## ADDED Requirements

### Requirement: Synchronous toolset push method
The system SHALL implement `toolset.push` as a JSON-RPC 2.0 method
over the existing XMiDT/WRP/Parodus transport, accepting a signed
toolset artifact and returning a synchronous accept-or-reject response
based on inline signature and manifest-policy verification. No
download step or asynchronous job queue SHALL be required for this
path.

#### Scenario: Valid push is accepted synchronously
- GIVEN a `toolset.push` request with a valid signature from a trusted
  release authority and a manifest within policy
- WHEN Dispatch Core processes the request
- THEN the response confirms initial load success within the same
  request/response cycle, with no separate polling step required

#### Scenario: Invalid signature is rejected, not partially applied
- GIVEN a `toolset.push` request whose signature does not verify
- WHEN Dispatch Core processes the request
- THEN the toolset is not loaded or registered with Plugin Manager,
  and the response is a JSON-RPC error identifying the failure

### Requirement: `toolset.push` requires a dedicated ACL scope
Authorization for `toolset.push` SHALL be evaluated against a scope
distinct from any scope granting `tools/call` access to a toolset's
own methods. Holding read or write access to a toolset's capabilities
SHALL NOT imply authorization to replace that toolset's code.

#### Scenario: Ordinary toolset access does not grant push authority
- GIVEN an identity holding write access to the `wifi` toolset's
  methods via `tools/call`
- WHEN that identity sends a `toolset.push` request for the `wifi`
  toolset
- THEN Dispatch Core denies the request unless that identity also
  separately holds the dedicated toolset-publish scope
