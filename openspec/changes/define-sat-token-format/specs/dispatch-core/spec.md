# Delta for dispatch-core

## MODIFIED Requirements

### Requirement: Stateless session validation
The system SHALL validate command sessions using a JWT signed by
Dispatch Core's own key, with `sub`, `groups`, `iat`, `exp` (5-minute
expiry), and `jti` claims, validated by signature and expiry check
only — no server-side session table and no per-request ACL Policy
Store lookup.
(Previously: unspecified stateless token format.)

#### Scenario: Dispatch Core restarts mid-fleet-operation
- GIVEN a valid, unexpired JWT issued before a Dispatch Core restart
- WHEN the same JWT is presented after restart
- THEN signature and expiry validation succeed without any session
  table to recover

### Requirement: Token revocation
The system SHALL bound compromised-token impact via a fixed 5-minute
token expiry with a refresh flow for legitimate sessions, rather than
an explicit revocation list.
(Previously: either mechanism left open.)

#### Scenario: Revoked token presented after compromise
- GIVEN a token issued more than 5 minutes ago that was not refreshed
- WHEN a request is made using that token
- THEN Dispatch Core rejects it as expired, bounding the exposure
  window to at most 5 minutes from issuance
