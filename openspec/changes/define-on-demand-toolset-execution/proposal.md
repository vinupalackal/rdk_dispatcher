# Proposal: On-Demand Toolset Execution (Not Always-Running)

## Intent

Toolset processes SHALL be spawned on demand, not run persistently at
all times. This directly answers the tension flagged when B3's
footprint number (under 300KB) was recorded: that number is tight for
Dispatch Core + Plugin Manager + N *simultaneously resident*
sandboxed toolset processes, but far more tractable once only the
toolsets actively being used are running at any given moment.

This supersedes one specific decision in `define-toolset-as-mcp-tool-model`
— "dynamically-pushed toolsets run out-of-process as a persistent,
supervised unit," which explicitly chose the RBUS-provider
always-running shape over rpcd's fork-per-call script model. That
choice is reversed here for footprint reasons. It does not reverse
anything about *whether* toolsets run out-of-process and sandboxed
(`define-plane-vs-toolset-model`'s A2/A3 decision stands unchanged) —
only *when* a toolset's process exists.

## Scope

In scope:
- Toolsets spawn on demand — when a `tools/call` (or a plane
  operation, since planes are toolsets too per A2/A3) actually needs
  them — rather than being started once and kept running indefinitely.
- A recommended concrete spawn/teardown model (idle-timeout, not pure
  per-call fork/exec — see `design.md` for the reasoning), flagged as
  needing confirmation rather than assumed final.
- Revising Plugin Manager's health-check model to fit on-demand
  processes: health is confirmed at spawn/first-successful-response
  time, not via periodic polling of an always-resident process.
- Revising `define-synchronous-toolset-push`'s rollback decision: the
  prior *artifact* is kept as the fallback to spawn if the new version
  fails its first on-demand health check, rather than keeping the
  prior *process* resident alongside the new one.

Out of scope:
- Changing whether toolsets are out-of-process/sandboxed at all — that
  remains `define-plane-vs-toolset-model`'s decision, unchanged.
- The exact idle-timeout duration — a tuning parameter, not an
  architecture decision, left as a task.
- Re-litigating B3's 300KB number itself — this proposal explains why
  it's now more achievable, it doesn't change the number.
