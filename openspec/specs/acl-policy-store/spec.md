# ACL Policy Store Specification

## Purpose

A dedicated, queryable facility for access-control policy — named
permission groups, identity-to-group mapping, deny rules, and audit
logging — used by Dispatch Core for its single enforcement checkpoint
and by Platform Adapters for identity-scoped decisions. Modeled on
`rpcd`'s `acl.d`/`uci` login pattern, reimplemented independently.

## Requirements

### Requirement: Named permission groups
The system SHALL support declarative permission groups scoped by
`read`/`write` over `{toolset: [methods...]}`.

#### Scenario: Group grants scoped read access
- GIVEN a group `wifi-readonly` granting `read` on `wifi.getStatus`
- WHEN an identity holding only that group calls `wifi.getStatus`
- THEN the call is permitted
- AND the same identity calling `wifi.setChannel` is denied

### Requirement: Deny rules evaluated first
The system SHALL evaluate negative/deny rules before allow rules for
any given identity and method.

#### Scenario: Explicit deny overrides an allow grant
- GIVEN an identity in a group that allows `docsis.getMetrics`
- AND a separate deny rule for that identity on `docsis.getMetrics`
- WHEN the identity calls `docsis.getMetrics`
- THEN the request is denied

### Requirement: Write-implies-read fallback
The system SHALL treat a `write` grant on a method as implicitly
granting `read` on the same method unless explicitly denied.

#### Scenario: Write-scoped identity performs an implicit read
- GIVEN an identity granted only `write` on `network.setConfig`
- WHEN the identity calls the read-equivalent of that method
- THEN the call is permitted under the write-implies-read fallback

### Requirement: Hot-reloadable policy
The system SHALL allow group definitions and identity mappings to be
updated without restarting Dispatch Core.

#### Scenario: Policy updated while the device is live
- GIVEN a running Dispatch Core with an active policy set
- WHEN the ACL Policy Store's group definitions are updated
- THEN subsequent requests are evaluated against the new definitions
  without any process restart

### Requirement: Runtime query API
The system SHALL expose a query API answering "would identity X be
allowed to call toolset.method?", usable by both Dispatch Core and
Platform Adapters.

#### Scenario: Adapter queries scope before forwarding
- GIVEN a Platform Adapter about to forward a call into RBUS
- WHEN it queries the ACL Policy Store for the caller's effective
  permissions
- THEN it receives an authoritative allow/deny answer it can combine
  with RBUS's own native ACL check

### Requirement: Audit logging
The system SHALL log every access decision — identity, target,
outcome — to an audit trail.

#### Scenario: Denied request is auditable
- GIVEN a denied request
- WHEN an auditor reviews the access log
- THEN the denied identity, target method, and timestamp are all
  recorded
