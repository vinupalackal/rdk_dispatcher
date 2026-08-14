# Delta for sandboxed-runtime

## ADDED Requirements

### Requirement: Phase 1 explicit, bounded exception to uniform sandboxing
The toolset(s) handling Phase 1's real command execution and
`toolset.push`-delivered updates MAY run in-process, deferring the
out-of-process/namespace/seccomp/cgroup/capability-drop requirements
`define-plane-vs-toolset-model`'s "Scope applies uniformly, regardless
of plane" requirement otherwise mandates without exception. This is a
narrow, tracked, time-bounded exception scoped to exactly Phase 1's
command-executing toolset(s) — it SHALL NOT be read as reopening that
requirement in general, and SHALL NOT be extended to any other
toolset or phase without its own explicit, reviewed exception request.

#### Scenario: Phase 1's command-executing toolset runs in-process, tracked
- GIVEN a toolset handling Phase 1 real command execution
- WHEN that toolset is loaded and serving requests
- THEN it runs in-process, and this deviation from uniform sandboxing
  is recorded here, not silently absent from the spec

#### Scenario: A Phase 2 toolset is not covered by this exception
- GIVEN a toolset whose command execution is scheduled for Phase 2,
  not part of this Phase 1 exception
- WHEN that toolset is loaded
- THEN it runs out-of-process and sandboxed per the uniform
  requirement, unaffected by this Phase 1-scoped carve-out

#### Scenario: Authorization is unaffected by this exception
- GIVEN a real command executed against Phase 1's in-process toolset
- WHEN the request reaches Dispatch Core
- THEN it still passes through the single ACL checkpoint
  (`dispatch-core/spec.md`) exactly as it would against an
  out-of-process toolset — this exception defers containment, not
  authorization
