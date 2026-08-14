# Design: On-Demand Toolset Execution

## Technical Approach

### Decision: spawn on demand, not persistent — footprint over standing readiness

A toolset's process does not exist until something needs it: a
`tools/call` targeting it, or (per `define-plane-vs-toolset-model`) a
plane operation implemented by that toolset. When idle, no process,
no resident memory, no namespace/cgroup held open — the 300KB budget
(`OPEN_QUESTIONS.md` B3) only has to cover whatever's concurrently
active, not everything installed.

### Decision: recommended mechanism — idle-timeout spawn, not pure per-call fork/exec

Two shapes were available, both already precedented in this project's
own research (doc 17 §2):

- **Pure per-call fork/exec**, exactly like `rpcd`'s script plugins —
  simplest, smallest footprint (truly zero resident cost between
  calls), but pays full process-spawn + sandbox-setup (namespace,
  seccomp, cgroup) latency on *every single call*, including a burst
  of related calls in quick succession.
- **Idle-timeout spawn** — spawn on first demand, keep the process
  (and its sandbox) alive for a short idle window, tear it down if
  nothing else arrives before the window expires. Pays the spawn cost
  once per burst of activity instead of once per call, at the cost of
  some resident memory during the idle window.

**Recommended: idle-timeout spawn.** `CLAUDE.md`'s own hard rules
(never block the sysevent notification thread; a hung plugin must
never stall the event loop) argue against paying full sandbox-setup
latency on every single control-plane call, especially for a toolset
handling a rapid sequence of related events. This is a recommendation,
not treated as settled — flagged in `tasks.md` for explicit
confirmation, the same way other mechanism choices in this project
have been.

### Decision: health checking moves from periodic polling to spawn-time confirmation

Plugin Manager's existing health-check responsibility assumed an
always-resident process it could poll periodically. Under on-demand
execution, health is confirmed when a toolset is spawned (or
re-spawned after an idle teardown) — a successful spawn plus a
successful first response is the health signal, rather than a
recurring poll of a process that might not exist at any given moment.
Plugin Manager's coarse registry (`toolset-lifecycle/spec.md`) still
tracks load state, but "health" now means "last spawn succeeded and
responded," not "currently running and responsive right now."

### Decision: rollback keeps the prior artifact, not the prior process, resident

`define-synchronous-toolset-push`'s rollback decision ("keep the prior
version live and serving until the new version passes its health
check") assumed two processes coexisting. Under on-demand execution,
neither version is necessarily running at any given moment, so
"keep the prior version live" is corrected to: Plugin Manager retains
the prior artifact as the fallback to spawn. The next demand spawns
the *new* version first; if that spawn or its first response fails
health confirmation, Plugin Manager falls back to spawning the *prior*
artifact instead, and reports the failure — functionally the same
safety property (a bad push doesn't leave the toolset unusable),
achieved without requiring both versions to be simultaneously
resident.

## File/Component Changes

- Plugin Manager: spawn-on-demand logic with an idle-timeout teardown
  (recommended mechanism, pending confirmation per `tasks.md`); health
  confirmation moves to spawn-time.
- `define-toolset-as-mcp-tool-model`: its "persistent, supervised
  unit" decision is superseded — pointer added there, not silently
  overwritten.
- `define-synchronous-toolset-push`: its rollback decision is amended
  to "retain prior artifact as fallback," not "keep prior process
  resident" — pointer added there.
- `OPEN_QUESTIONS.md` B3/B11: reframed — the footprint tension eases
  substantially once only active toolsets are resident, though B11's
  underlying question (spawn-time IPC/sandbox-setup latency on the
  sysevent thread) still needs real measurement, now against
  spawn-cost specifically rather than steady-state resident cost.
