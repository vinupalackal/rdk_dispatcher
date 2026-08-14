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
