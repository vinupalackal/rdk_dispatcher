# Delta for toolset-lifecycle

## ADDED Requirements

### Requirement: Phase 1 exception to out-of-process delivery for dynamically-pushed toolsets
`define-toolset-as-mcp-tool-model`'s requirement that a dynamically
pushed toolset run as its own out-of-process unit SHALL NOT apply to
Phase 1's command-executing toolset(s), as a scoped, tracked
exception. `toolset.push`'s verification gate (signature and
manifest-declared capabilities checked before registration) and the
RDM Client boundary (any `dlopen()`-able binary goes through RDM,
never through `toolset.push`, regardless of size) SHALL remain
unchanged — only the execution location of already-verified,
already-delivered code is affected by this exception, not what is
allowed to load it.

#### Scenario: A Phase 1 toolset.push still verifies before registering
- GIVEN a `toolset.push` request targeting Phase 1's in-process
  toolset
- WHEN Dispatch Core receives it
- THEN signature and manifest verification still gate registration
  exactly as they would for an out-of-process toolset — only where
  the resulting code runs differs

### Requirement: Phase 1 rollback uses a health-check-gated in-process swap, not artifact fallback
Because Phase 1's command-executing toolset(s) run in-process rather
than as separate spawned processes, `toolset.push`'s rollback
behavior for Phase 1 SHALL follow a health-check-gated swap: the
prior in-process version stays loaded and serving until the new
version passes its health check, at which point Plugin Manager
switches which version handles calls. This is distinct from — and
SHALL NOT be conflated with — `define-on-demand-toolset-execution`'s
artifact-fallback rollback model, which assumes out-of-process,
on-demand spawning and applies once Phase 2's hardening of this
toolset lands, not before.

#### Scenario: A failed health check keeps the prior version serving
- GIVEN a `toolset.push` delivering a new version of a Phase 1
  in-process toolset
- WHEN the new version fails its health check
- THEN the prior version continues serving calls uninterrupted, and
  no process spawn or artifact-fallback logic is invoked
