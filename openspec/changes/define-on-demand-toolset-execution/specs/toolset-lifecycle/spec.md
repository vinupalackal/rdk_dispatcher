# Delta for toolset-lifecycle

## ADDED Requirements

### Requirement: Toolsets execute on demand, not persistently
A toolset's process SHALL be spawned when a request actually targets
it (a `tools/call`, or a plane operation implemented by that toolset)
and SHALL NOT be kept running indefinitely while idle. This supersedes
any prior requirement implying a toolset process runs continuously
once loaded.

#### Scenario: Idle toolset holds no resident process
- GIVEN a toolset with no pending or recent requests
- WHEN its idle window (if using idle-timeout spawn) or immediately
  after its last call (if using per-call spawn) elapses
- THEN no process for that toolset remains resident, freeing its
  memory and sandbox resources

#### Scenario: Health is confirmed at spawn time, not by periodic polling
- GIVEN a toolset spawned to serve a request
- WHEN Plugin Manager needs to know whether that toolset is healthy
- THEN a successful spawn plus a successful first response is the
  health signal — Plugin Manager does not require an always-resident
  process to poll periodically

### Requirement: Rollback retains the prior artifact, not the prior process
When a toolset is updated (via `toolset.push` or RDM Client), Plugin
Manager SHALL retain the prior version's artifact as a fallback. If
the newly spawned version fails its first on-demand health
confirmation, Plugin Manager SHALL fall back to spawning the prior
artifact instead, without requiring both versions to run
simultaneously.

#### Scenario: Failed health check on a fresh spawn triggers fallback, not a resident standby
- GIVEN a toolset just updated to a new version, with no process yet
  spawned for either version
- WHEN the first on-demand spawn of the new version fails its health
  confirmation
- THEN Plugin Manager spawns the prior artifact on the next demand
  instead, and reports the failure — the prior version was never
  required to be running in the background to serve as this fallback
