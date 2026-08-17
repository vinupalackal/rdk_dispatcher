# Delta for toolset-lifecycle

## ADDED Requirements

### Requirement: Toolset schema maps to one MCP tool definition per toolset
**Revised 2026-08-13** — a toolset's self-described schema SHALL be
projectable, without duplication, into a single MCP tool definition
per toolset (`name` = the toolset's own name, `description`,
`inputSchema`), not one MCP tool definition per method. `inputSchema`
SHALL be a discriminated union (JSON Schema `oneOf`) with one branch
per method, each branch requiring a `method` field constrained to that
method's name and a `params` sub-schema matching that method's own
argument shape. This projection SHALL be derived directly from the
toolset's own `schema()` at query time, not maintained as a separate,
independently-updated copy.

#### Scenario: MCP tool definition reflects the live schema
- GIVEN a toolset plugin updated to version 2 with a changed method
  signature
- WHEN Dispatch Core serves `tools/list`
- THEN the MCP tool definition for that toolset's relevant `oneOf`
  branch reflects version 2's signature immediately, with no separate
  update required anywhere else

#### Scenario: ACL granularity is unaffected by the coarser catalog entry
- GIVEN a toolset with five methods, projected as one `tools/list`
  entry with a five-branch `oneOf` schema
- WHEN an identity is authorized for only two of those five methods
- THEN a `tools/call` naming the other three methods is still denied
  per-method by the ACL Policy Store, exactly as if each method were
  its own separate `tools/list` entry

### Requirement: Dynamically-pushed toolsets run out-of-process — superseded on persistence
**Superseded 2026-08-13 by `define-on-demand-toolset-execution`:** a
toolset made available through the dynamic push/discovery mechanism
SHALL run as its own out-of-process unit — not loaded into any other
process's address space — but SHALL NOT be required to run
persistently; see that change for the on-demand spawn model and its
revised, spawn-time health-check requirement.

#### Scenario: A pushed toolset is health-checked like any other
- GIVEN a toolset added via the dynamic push mechanism
- WHEN Plugin Manager confirms its health (at spawn time, per
  `define-on-demand-toolset-execution`)
- THEN the pushed toolset is checked identically to a toolset that
  shipped with the initial build — no separate code path exists for
  pushed vs. built-in toolsets at the Plugin Manager level

### Requirement: Verification gates registration regardless of push convenience
A dynamically pushed toolset SHALL NOT be registered with Plugin
Manager or appear in `tools/list` until RDM Client has verified its
signature and manifest-declared capabilities against policy, exactly
as for any other toolset install path.

#### Scenario: An unverified pushed toolset is invisible to the cloud
- GIVEN a toolset file dropped into the push/discovery location but
  not yet verified by RDM Client
- WHEN an MCP client sends `tools/list`
- THEN that toolset's methods do not appear in the response
