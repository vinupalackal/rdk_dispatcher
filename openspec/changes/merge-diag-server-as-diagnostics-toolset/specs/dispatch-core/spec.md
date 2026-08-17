# Delta for dispatch-core

## ADDED Requirements

### Requirement: Phase 1 explicit, time-bounded exception to the single-checkpoint rule, scoped to `diagnostics`
The `diagnostics` toolset (`external/diag-server/`) MAY enforce its own
access-control decision in-process (`diag_acl_check()`, calling the
same `acl_policy_store_query()` extern every other toolset's
checkpoint uses) as a narrow, tracked, time-bounded exception to the
"Single ACL checkpoint" requirement above. This exception exists only
because no Dispatch Core process currently forwards `diagnostics`
traffic to it — `diagnostics` is reachable solely via its own local
IPC endpoint and a disabled public registration, with no live
Dispatch-Core-fronted path yet. It SHALL NOT be read as reopening the
single-checkpoint rule in general, and SHALL NOT be extended to any
other toolset without its own explicit, reviewed exception request.

**Retirement condition**: once a real Dispatch Core process exists and
forwards `diagnostics` traffic over the local endpoint
(`DIAG_LOCAL_RECV_URL`/`DIAG_LOCAL_SEND_URL`), this exception SHALL be
retired — at that point `diag_acl_check()` either becomes redundant
defense-in-depth behind Dispatch Core's real checkpoint, or is removed
entirely; either decision is out of scope for this change and belongs
to whichever change actually builds that Dispatch Core path.

#### Scenario: `diagnostics` denies a request with no Dispatch Core process involved
- GIVEN the `diagnostics` toolset with no Dispatch Core process
  currently forwarding traffic to it
- WHEN a request for a tool the caller lacks permission for arrives at
  diag-server directly
- THEN `diag_acl_check()` denies it before catalog lookup or execution,
  returning `exit_code=126`/`stdout="access denied"`, without any
  Dispatch Core process having been involved in the decision

#### Scenario: This exception does not extend to any other toolset
- GIVEN a toolset other than `diagnostics` with no Dispatch Core
  process fronting it
- WHEN that toolset receives a request
- THEN it SHALL NOT make its own independent access-control decision —
  the single-checkpoint requirement applies to it unmodified, and any
  gap this creates (no enforcement until Dispatch Core exists) is a
  distinct problem, not one this exception can be assumed to also cover

#### Scenario: The exception is retired once a real checkpoint exists
- GIVEN a Dispatch Core process that forwards `diagnostics` traffic
  over the local endpoint
- WHEN this exception's retirement is evaluated
- THEN `diagnostics`' own `diag_acl_check()` call is no longer the sole
  access-control decision for its traffic, and this requirement's
  exception no longer applies
