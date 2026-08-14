# Proposal: Define SAT Token Format and Revocation Mechanism

## Intent

`dispatch-core/spec.md` already requires stateless session validation
and a revocation mechanism (see its "Stateless session validation" and
"Token revocation" requirements), but leaves the actual token format
and revocation approach as an open question — flagged explicitly in
`RDK_Dispatcher_Architecture_and_Requirements.md`, §8. This change
closes that gap with a concrete choice.

## Scope

In scope:
- Choosing a concrete token format (JWT) and claim structure
- Choosing a revocation approach appropriate for a stateless token
- Updating `dispatch-core/spec.md`'s existing requirements with the
  concrete mechanism, without changing their behavioral intent

Out of scope:
- The RBUS/Thunder adapter identity-propagation mechanics (separate
  concern, already covered in `platform-adapters/spec.md`)
- Device-identity authentication for the capability-sync path (already
  specified as a distinct mechanism in `capability-sync/spec.md`)

## Approach

Use short-lived JWTs (5-minute expiry) signed by Dispatch Core's own
key, with an explicit refresh flow rather than a revocation list — a
revocation list would reintroduce server-side state that the original
stateless-token decision was specifically trying to avoid (see
`RDK_Dispatcher_Architecture_and_Requirements.md` §5, "Command auth and
capability-sync auth are different trust relationships"). Short expiry
bounds the blast radius of a compromised token without requiring
Dispatch Core to persist anything.
