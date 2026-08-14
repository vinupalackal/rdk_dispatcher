# Design: SAT Token Format and Revocation

## Technical Approach

Tokens are JWTs, signed with an EdDSA key Dispatch Core generates at
first boot and persists to secure storage. Claims: `sub` (identity),
`groups` (ACL Policy Store group names, embedded at issuance so
Dispatch Core doesn't need a round-trip to the store on every
request), `iat`/`exp` (5-minute expiry), `jti` (unique token ID, used
only for audit correlation, not revocation state).

## Architecture Decisions

### Decision: Short expiry + refresh, not a revocation list
A revocation list requires Dispatch Core to persist and check
server-side state on every request — exactly the statefulness the
original design decision (NFR-9) was chosen to avoid. A 5-minute
expiry bounds exposure from a compromised token to at most 5 minutes,
with a refresh endpoint for legitimate long-running sessions.

### Decision: Groups embedded in the token, not looked up per-request
Embedding `groups` at issuance means Dispatch Core's ACL check is a
local claim inspection, not a live ACL Policy Store query on every
request — matching NFR-6's "keep the broker's routing/auth path cheap"
principle. Trade-off: a group membership change doesn't take effect
for an already-issued token until it expires (≤5 minutes) or is
refreshed; acceptable given the short expiry.

## File/Component Changes
- Dispatch Core: JWT issuance (`session.login` equivalent) and
  validation (claim signature + expiry check, no store lookup)
- ACL Policy Store: unchanged interface, still authoritative for
  Platform Adapters' own queries
