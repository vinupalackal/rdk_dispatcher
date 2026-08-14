# Delta for toolset-lifecycle

## ADDED Requirements

### Requirement: Health-check-gated rollback for pushed toolsets
When a toolset is updated via `toolset.push`, Plugin Manager SHALL
keep the prior version live and serving until the new version passes
its health check within a defined window. On health-check failure,
Plugin Manager SHALL automatically revert to the prior version.

#### Scenario: Failed health check triggers automatic revert
- GIVEN a toolset pushed to a new version that fails its post-load
  health check
- WHEN the health-check window expires without success
- THEN Plugin Manager reverts to the prior version automatically, and
  `tools/call` requests against that toolset are served without
  interruption throughout

### Requirement: Two install paths, one verification policy
`toolset.push` and RDM Client SHALL both be recognized install paths,
scoped by artifact class (plugin-scale versus firmware/large-package),
and SHALL apply the same signature and manifest-policy verification
standard. Neither path SHALL apply a weaker check than the other.

#### Scenario: Same artifact would be rejected identically on either path
- GIVEN a toolset artifact whose manifest requests a capability
  disallowed by policy
- WHEN it is submitted via `toolset.push`
- THEN it is rejected for the same policy reason RDM Client would
  reject it for if submitted through its own install path
