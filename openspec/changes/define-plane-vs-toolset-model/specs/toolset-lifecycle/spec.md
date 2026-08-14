# Delta for toolset-lifecycle

**Revised 2026-08-13** — the "may implement multiple planes
internally" requirement is retained from the original delta; a new
requirement is added stating the cloud's uniform invocation model
across planes.

## ADDED Requirements

### Requirement: A toolset may implement multiple planes internally
A toolset SHALL be permitted to implement logic spanning multiple
planes (config-apply, management, control, triage) within its own
internal method table. Plane SHALL be treated as a description of
what a given piece of a toolset's logic does, not as a separate
routable, installable, or execution-trust unit in its own right, and
not as grounds for that logic to run outside the toolset architecture.

#### Scenario: One toolset reports capabilities across planes
- GIVEN the wifi toolset with config-apply, control, and triage logic
  all implemented internally
- WHEN Schema & Discovery queries its `capabilities()`
- THEN the response includes all three planes' capabilities as part
  of the single wifi toolset's self-description, not as three
  separately routable entities

#### Scenario: Plugin Manager's coarse registry is unaffected by plane count
- GIVEN a toolset that adds triage-plane logic to its existing
  config-apply and control logic
- WHEN the toolset is reloaded with this addition
- THEN Plugin Manager's registry entry for that toolset changes only
  its health/reload timestamp, consistent with the existing
  coarse-only-registry requirement — plane is invisible at Plugin
  Manager's level of granularity

### Requirement: Uniform cloud invocation model across all planes
The cloud SHALL invoke any toolset capability — regardless of which
plane it belongs to — identically: by naming the toolset and passing
arguments via `tools/call`. No plane SHALL have its own,
separately-shaped invocation path.

#### Scenario: A triage toolset and a domain toolset are called the same way
- GIVEN a triage-plane toolset and a wifi domain toolset both loaded
- WHEN the cloud invokes a method on each
- THEN both invocations use the identical `tools/call` shape — toolset
  name, method, arguments — through the same ACL checkpoint and
  Execution Framework path, with no plane-specific protocol variant
