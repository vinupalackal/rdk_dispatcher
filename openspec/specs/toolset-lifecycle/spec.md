# Toolset Lifecycle Specification

## Purpose

Covers Plugin Manager's coarse routing/health responsibilities, each
toolset plugin's self-described schema and independently-owned method
table, and the Toolset Store/RDM Client install pipeline.

## Requirements

### Requirement: Coarse-only plugin registry
Plugin Manager SHALL maintain only which toolsets exist, which
process/socket serves each, load state, and health — not a
device-wide table of every method every toolset exposes.

#### Scenario: Adding a method does not touch Plugin Manager
- GIVEN a toolset plugin that adds a new method to its own internal
  dispatch table
- WHEN the plugin is reloaded with this change
- THEN Plugin Manager's registry entry for that toolset is unchanged
  except for its health/reload timestamp

### Requirement: Lifecycle management without full restart
The system SHALL support loading, unloading, reloading, and
health-checking toolset plugins without requiring a device restart.

#### Scenario: Toolset reload during live operation
- GIVEN a running device serving commands to other toolsets
- WHEN one toolset plugin is reloaded to a new version
- THEN other toolsets continue serving requests uninterrupted

### Requirement: Self-described schema
Each toolset plugin SHALL expose its own `list()`, `schema()`,
`version()`, and `capabilities()` — this information SHALL be
authoritative from the plugin itself, not duplicated as a separately
maintained copy elsewhere.

#### Scenario: Schema query reflects the running plugin directly
- GIVEN a toolset plugin at version 2 with a new method not present
  in version 1
- WHEN Schema & Discovery queries that plugin's `schema()`
- THEN the new method appears immediately, with no separate update
  required to any other component's data

### Requirement: Toolset schema maps to one MCP tool definition per toolset
A toolset's self-described schema SHALL be projectable, without
duplication, into a single MCP tool definition per toolset (`name` =
the toolset's own name, `description`, `inputSchema`), not one MCP
tool definition per method. `inputSchema` SHALL be a discriminated
union (JSON Schema `oneOf`) with one branch per method, each branch
requiring a `method` field constrained to that method's name and a
`params` sub-schema matching that method's own argument shape. This
projection SHALL be derived directly from the toolset's own `schema()`
at query time, not maintained as a separate, independently-updated
copy.

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

### Requirement: Descriptive per-method metadata is a sibling field, not embedded in a `tools/list` entry's `inputSchema`
A toolset's `tools/list` entry MAY include an optional `methods` array
— one item per method, keyed by method name — carrying descriptive
metadata about that method or the plugin implementing it (for example
`load_type`, `version`, `timeout_ms`). This field SHALL be a sibling
of `inputSchema`, not embedded within any of `inputSchema`'s `oneOf`
branches. `inputSchema`'s `oneOf` branches SHALL contain only the
argument shape for invoking each method (`method` and `params`), never
descriptive metadata.

#### Scenario: A toolset's methods metadata is queryable without polluting its argument schema
- GIVEN a toolset with a `methods` array in its `tools/list` entry
- WHEN a caller inspects that entry
- THEN `inputSchema`'s `oneOf` branches contain only `method`/`params`
  keys, and per-method descriptive detail is read from the separate
  `methods` array, correlated by method name

#### Scenario: A toolset with nothing descriptive to add omits `methods` entirely
- GIVEN a toolset whose methods need no metadata beyond their argument
  shapes
- WHEN its `tools/list` entry is built
- THEN the `methods` field is simply absent — it is optional, not a
  field every toolset must populate

#### Scenario: A generic MCP client is unaffected by the presence of `methods`
- GIVEN an MCP client that only reads the standard `name`/
  `description`/`inputSchema` fields
- WHEN it receives a `tools/list` entry that includes a `methods`
  sibling field
- THEN it parses the entry correctly, ignoring the unrecognized
  additional field — `methods` is additive and never required for
  correct argument-schema interpretation

### Requirement: Dynamically-pushed toolsets run out-of-process
A toolset made available through the dynamic push/discovery mechanism
SHALL run as its own out-of-process unit — not loaded into any other
process's address space. It SHALL NOT be required to run persistently
(see `define-on-demand-toolset-execution` for the on-demand spawn
model and its spawn-time health-check requirement, not yet merged into
this spec as of this writing).

#### Scenario: A pushed toolset is health-checked like any other
- GIVEN a toolset added via the dynamic push mechanism
- WHEN Plugin Manager confirms its health
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

### Requirement: Manifest-declared requirements
Every toolset package SHALL declare its required capabilities,
device-node access, and target adapter(s) in an install-time manifest.

#### Scenario: Manifest declares sandbox requirements
- GIVEN a toolset package requesting network-interface capabilities
- WHEN it is prepared for the Toolset Store
- THEN its manifest explicitly lists that capability requirement,
  rather than the requirement being implicit or undeclared

### Requirement: Verified install with rollback
RDM Client SHALL verify a toolset's signature and manifest-declared
capabilities against policy before installing it, and SHALL support
rolling back to a prior version.

#### Scenario: Over-privileged toolset is rejected at install
- GIVEN a toolset package whose manifest requests a capability
  disallowed by device policy
- WHEN RDM Client attempts to install it
- THEN installation fails before the toolset is ever loaded or
  executed

#### Scenario: Rollback after a bad update
- GIVEN a toolset that was just updated to a new version
- WHEN the new version is found to be faulty
- THEN RDM Client can restore the prior version without manual
  intervention
