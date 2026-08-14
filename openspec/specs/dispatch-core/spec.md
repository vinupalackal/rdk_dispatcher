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
