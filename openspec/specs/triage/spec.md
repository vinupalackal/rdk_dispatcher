# Triage Specification

## Purpose

Covers the Triage Toolset — the triage-plane domain toolset first
delivered in Phase 1 — specifically its static/dynamic plugin capability
merge and how that capability set is discovered by the cloud. Command
execution, evidence capture, and `trace_id`-correlated async enqueue
for triage events are a later phase's scope, not covered here.

## Requirements

### Requirement: Static and dynamic plugin capability merge
The Triage Toolset SHALL merge capability descriptors from both
statically compiled-in plugins and dynamically (`dlopen()`-)loaded
plugins into a single internal descriptor set, tagging each entry with
a `load_type` of `"static"` or `"dynamic"`. This descriptor set SHALL
be the single source both `capability-sync`'s push and the generic
`tools/list` projection (`toolset-lifecycle/spec.md`'s "Toolset schema
maps to one MCP tool definition per toolset") read from — not two
independently maintained copies.

#### Scenario: Response includes both plugin kinds
- GIVEN a Triage Toolset process with one compiled-in plugin and one
  `.so` plugin loaded from `/usr/libexec/dispatcher/triage/`
- WHEN the Triage Toolset's internal capability descriptor set is
  queried (by `tools/list`'s projection or by `capability-sync`'s
  push, either consumer)
- THEN the descriptor set contains both entries, each correctly tagged
  by `load_type`

#### Scenario: A dynamic plugin fails to load
- GIVEN a `.so` file in the triage plugin directory missing
  `describe()` or `handle()`
- WHEN the Triage Toolset process starts
- THEN that plugin is skipped with a logged error, and does not appear
  in the internal descriptor set — static plugins and other valid
  dynamic plugins are unaffected

### Requirement: Triage Toolset is discoverable through the generic `tools/list` mechanism, not a dedicated method
The Triage Toolset SHALL be listed in `tools/list`'s aggregated
response as a single entry named `"triage"`, per
`toolset-lifecycle/spec.md`'s coarse per-toolset granularity. It SHALL
NOT expose a separate, triage-specific discovery method (e.g.
`triage.capabilities`) — capability discovery for triage goes through
the same mechanism as any other toolset, no exception.

#### Scenario: `tools/list` includes the triage entry, in full, for an authorized caller
- GIVEN a device with an active Triage Toolset process, and a caller
  with at least read access to the `triage` toolset
- WHEN that caller sends `tools/list`
- THEN the response's `tools` array includes exactly one entry named
  `"triage"`, whose `inputSchema` is a `oneOf` discriminated union with
  one branch per internal triage sub-plugin method

#### Scenario: `tools/list` still names triage for a caller with no grant on it
- GIVEN a device with an active Triage Toolset process, and a caller
  with no ACL grant on the `triage` toolset
- WHEN that caller sends `tools/list`
- THEN the response's `tools` array still includes an entry named
  `"triage"`, but with `inputSchema`/`methods` replaced by
  `"access_restricted": true` — per `dispatch-core/spec.md`'s two-tier
  `tools/list` visibility requirement, which this domain doesn't
  re-specify, only conforms to

#### Scenario: No bespoke triage discovery method exists
- GIVEN a device with an active Triage Toolset process
- WHEN a cloud client sends a JSON-RPC request for method
  `"triage.capabilities"`
- THEN the device responds with a JSON-RPC `error` (method not found)
  — no such method is implemented

### Requirement: Phase 1 scope boundary
This domain's Phase 1 SHALL cover only capability discovery for the
triage plane, via the generic `tools/list` mechanism. It SHALL NOT
cover triage event execution, evidence capture, or
`trace_id`-correlated async enqueue behavior — those remain out of
scope until a later change extends this domain.

#### Scenario: Capability discovery does not trigger triage execution
- GIVEN a device that receives a `tools/list` request
- WHEN the response includes the `triage` entry
- THEN no triage plugin's `handle()` is invoked as part of answering
  the request — discovery and execution are independent paths
