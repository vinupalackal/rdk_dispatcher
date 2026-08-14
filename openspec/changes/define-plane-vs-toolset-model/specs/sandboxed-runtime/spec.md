# Delta for sandboxed-runtime

**Revised 2026-08-13** — replaces the original delta, which exempted
dispatcher-core plane logic from this domain. That exemption is
withdrawn.

## ADDED Requirements

### Requirement: Scope applies uniformly, regardless of plane
This domain's out-of-process, namespace, seccomp, cgroup, and
capability-drop requirements SHALL apply to every toolset, regardless
of which plane(s) — config-apply, management, control, triage, or any
domain category (common, network, wifi, DOCSIS, vendor) — its internal
logic implements. No toolset SHALL be exempt from these requirements
on the basis of being first-party, dispatcher-team-authored, or
shipped alongside Dispatch Core.

#### Scenario: First-party triage-plane logic is still fully sandboxed
- GIVEN a triage-plane toolset authored and shipped by the same team
  that builds Dispatch Core itself
- WHEN that toolset process is launched
- THEN it runs out-of-process, namespaced, seccomp-filtered, and
  cgroup-limited exactly as a vendor-supplied DOCSIS toolset would —
  authorship does not grant an exemption

#### Scenario: A toolset's internal plane logic does not affect its sandbox scope
- GIVEN a toolset whose internal method table spans config-apply,
  control, and triage logic
- WHEN that toolset process is launched
- THEN the entire process is sandboxed as one unit — no individual
  plane's logic within it is carved out for lighter or no isolation
