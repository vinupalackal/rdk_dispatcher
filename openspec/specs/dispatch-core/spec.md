# Dispatch Core Specification

## Purpose

Terminates the device's transport connection, frames JSON-RPC 2.0
requests, validates session tokens, and is the single mandatory ACL
enforcement point for every request the device receives — from the
cloud or from a local UDS client. Dispatch Core does not depend on
`ubus`/`rpcd`, and does not perform method-level dispatch itself (see
`toolset-lifecycle/spec.md` for who does).

## Requirements

### Requirement: Transport
The system SHALL accept JSON-RPC 2.0 requests over the existing
XMiDT/WRP/Parodus transport. No new cloud-facing transport SHALL be
introduced.

#### Scenario: Command arrives over existing transport
- GIVEN a cloud/ops client with an active XMiDT session
- WHEN it sends a JSON-RPC 2.0 request via Parodus
- THEN Dispatch Core accepts and parses it without requiring any
  additional transport-layer negotiation

### Requirement: MCP tool method surface
The system SHALL implement `initialize`, `tools/list`, `tools/call`,
and `notifications/tools/list_changed` as JSON-RPC 2.0 methods over
the existing XMiDT/WRP/Parodus transport (see Transport requirement
above). No additional transport SHALL be introduced to support this.

#### Scenario: MCP client discovers toolsets
- GIVEN an MCP-compliant cloud client with an active XMiDT session
- WHEN it sends a `tools/list` request
- THEN Dispatch Core responds with every currently loaded toolset's
  methods, each expressed as one MCP tool definition
  (`toolset-lifecycle/spec.md`'s "Toolset schema maps to one MCP tool
  definition per toolset")

#### Scenario: Toolset reload triggers a live notification
- GIVEN an MCP client connected when a toolset is reloaded with a new
  method
- WHEN Plugin Manager completes the reload
- THEN Dispatch Core sends `notifications/tools/list_changed` to that
  client without the client needing to poll

### Requirement: `tools/call` uses the standard command path
`tools/call` SHALL be authorized, resolved, and executed through the
identical path as any other JSON-RPC command request — the single ACL
checkpoint below, Plugin Manager resolution, Execution Framework, and
sandboxed toolset execution. It SHALL NOT constitute a second,
independent command-authorization path.

#### Scenario: `tools/call` is denied exactly like an equivalent direct call
- GIVEN an identity without permission for `wifi.setChannel`
- WHEN it sends a `tools/call` request naming `wifi.setChannel`
- THEN Dispatch Core denies it via the same ACL Policy Store check
  that would deny an equivalent non-MCP JSON-RPC request for the same
  method

### Requirement: `tools/list` visibility is two-tier, scoped by the caller's existing ACL grant
The system SHALL list every currently loaded toolset by name in a
`tools/list` response, regardless of the caller's ACL grants. For each
listed toolset, the system SHALL additionally include that toolset's
`inputSchema` (and, where present, its `methods` metadata array) only
if the caller has at least read access to that toolset, per
`acl_policy_store_query(caller, toolset, method)` and the existing
write-implies-read default (`acl-policy-store/spec.md`). A toolset the
caller lacks any grant for SHALL still appear by name, with detail
fields replaced by `"access_restricted": true`.

#### Scenario: Unauthorized caller sees the toolset exists but not its schema
- GIVEN a caller with no ACL grant on the `docsis` toolset
- WHEN that caller sends `tools/list`
- THEN the response's `tools` array includes an entry named `"docsis"`
  with `"access_restricted": true`, and no `inputSchema` or `methods`
  field

#### Scenario: A read-only discovery grant sees full detail without gaining write access
- GIVEN a caller whose ACL grant scopes only read access to the
  `triage` toolset
- WHEN that caller sends `tools/list`
- THEN the response's `triage` entry includes full `inputSchema` and
  `methods` detail, and this grant alone does not authorize a
  subsequent `tools/call` against any write/execute method of that
  toolset

#### Scenario: Write access implies full listing detail
- GIVEN a caller with write access to the `wifi` toolset
- WHEN that caller sends `tools/list`
- THEN the response's `wifi` entry includes full `inputSchema` and
  `methods` detail, without a separate read grant being required

#### Scenario: Building a filtered listing reuses the existing ACL query interface
- GIVEN a device with N currently loaded toolsets
- WHEN it builds a `tools/list` response
- THEN it calls the existing `acl_policy_store_query(caller, toolset,
  method)` function once per loaded toolset to determine that
  toolset's visibility tier — no second, parallel authorization
  mechanism is introduced for `tools/list` specifically

### Requirement: Single ACL checkpoint
The system SHALL enforce access control exactly once, in Dispatch
Core, for every request. No other component (toolset plugin, adapter,
Execution Framework) SHALL make an independent access-control
decision.

#### Scenario: Request denied before reaching a plugin
- GIVEN an identity without permission for `wifi.setChannel`
- WHEN it sends a request targeting that method
- THEN Dispatch Core denies the request via the ACL Policy Store
  before Plugin Manager or any toolset plugin process is invoked

### Requirement: Local clients receive equal enforcement
The system SHALL apply the same ACL check to local UDS clients as to
cloud-originated requests. No local-socket exemption SHALL exist.

#### Scenario: Local daemon calls a write-scoped method
- GIVEN a local system service connected via UDS with only read
  permissions
- WHEN it calls a write-scoped toolset method
- THEN Dispatch Core denies the call identically to how it would deny
  the same call from a cloud client with the same permissions

### Requirement: Stateless session validation
The system SHALL validate command sessions using a stateless
token (SAT/JWT-style) rather than a server-side session table that
requires persistence across restarts.

#### Scenario: Dispatch Core restarts mid-fleet-operation
- GIVEN a valid, unexpired session token issued before a Dispatch Core
  restart
- WHEN the same token is presented after restart
- THEN the request is validated successfully without requiring a
  freeze/thaw session-recovery mechanism

### Requirement: Token revocation
The system SHALL bound the impact of a compromised token through
either short expiry with refresh or an explicit revocation list, given
that stateless tokens cannot be invalidated by deleting server-side
state.

#### Scenario: Revoked token presented after compromise
- GIVEN a token that has been explicitly revoked
- WHEN a request is made using that token
- THEN Dispatch Core rejects the request even though the token's
  signature and expiry are otherwise valid
