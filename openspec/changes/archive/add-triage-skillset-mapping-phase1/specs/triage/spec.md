# Delta for triage (new domain)

**Corrected 2026-08-16** — the original "WRP-framed skillset mapping
request/response" requirement below is removed. It specified a bespoke
`triage.capabilities` JSON-RPC method as the discovery mechanism,
predating `define-toolset-as-mcp-tool-model`'s later decision that
`tools/list` is the generic, project-wide discovery mechanism every
toolset gets automatically. Re-specifying a triage-specific discovery
method here would duplicate that project-wide requirement (already
covered by `define-toolset-as-mcp-tool-model`'s own spec deltas) rather
than add anything triage-specific. See `design.md`'s "Decision: reuse
`define-toolset-as-mcp-tool-model`'s generic `tools/list`" for the full
reasoning. The requirements below are what's left — genuinely specific
to the triage domain, not restating generic toolset/MCP mechanics.

## ADDED Requirements

### Requirement: Static and dynamic plugin capability merge
The Triage Toolset SHALL merge capability descriptors from both
statically compiled-in plugins and dynamically (`dlopen()`-)loaded
plugins into a single internal descriptor set, tagging each entry with
a `load_type` of `"static"` or `"dynamic"`. This descriptor set SHALL
be the single source both `capability-sync`'s push and the generic
`tools/list` projection (`define-toolset-as-mcp-tool-model/spec.md`)
read from — not two independently maintained copies.

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
`define-toolset-as-mcp-tool-model/spec.md`'s coarse per-toolset
granularity. It SHALL NOT expose a separate, triage-specific discovery
method (e.g. `triage.capabilities`) — capability discovery for triage
goes through the same mechanism as any other toolset, no exception.

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
  `"access_restricted": true` — per
  `resolve-tools-list-metadata-and-acl-scoping/spec.md`'s two-tier
  visibility model (confirmed 2026-08-16), which this domain doesn't
  re-specify, only conforms to

#### Scenario: No bespoke triage discovery method exists
- GIVEN a device with an active Triage Toolset process
- WHEN a cloud client sends a JSON-RPC request for method
  `"triage.capabilities"`
- THEN the device responds with a JSON-RPC `error` (method not found)
  — this method was never implemented under the corrected design

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

## REMOVED Requirements

### Requirement: WRP-framed skillset mapping request/response
**Removed 2026-08-16** — superseded by the generic `tools/list`
mechanism (`define-toolset-as-mcp-tool-model/spec.md`); see this
file's header note and `design.md` for the full reasoning. This
domain no longer specifies its own request/response envelope for
discovery.

### Requirement: Single ACL checkpoint applies to capability discovery
**Removed 2026-08-16** — this requirement assumed `triage.capabilities`
was an individually ACL-gated method, the same way a business method
is. Under `tools/list`, that framing doesn't directly transfer:
`tools/list` is a catalog listing, not itself bound to any one
toolset's ACL scope in the way `define-toolset-as-mcp-tool-model`
originally specified it. **Resolved 2026-08-16**, project-wide, not
decided unilaterally here:
`openspec/changes/resolve-tools-list-metadata-and-acl-scoping/`
(confirmed by direct instruction) — every loaded toolset is always
named in `tools/list`; per-toolset detail is gated by the caller's
existing ACL grant via the same `acl_policy_store_query()` function
`tools/call` already uses. Triage conforms to this generic model (see
the two `tools/list`-visibility scenarios above) rather than
re-specifying its own ACL behavior.
