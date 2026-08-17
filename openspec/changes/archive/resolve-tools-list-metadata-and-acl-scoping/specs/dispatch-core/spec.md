# Delta for dispatch-core

## ADDED Requirements

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
