# Delta for toolset-lifecycle

## ADDED Requirements

### Requirement: `diagnostics` names a concrete instance of the Phase 1 in-process exception
The `diagnostics` toolset (`external/diag-server/`) SHALL be the named,
concrete toolset `add-phase1-command-execution-exception`'s
`toolset-lifecycle` delta describes in the abstract ("Phase 1's
command-executing toolset(s)"). Its verified install, health-check-gated
rollback, and in-process execution SHALL follow that delta's
requirements unchanged; this requirement only attaches a name to what
was previously generic.

#### Scenario: `diagnostics` runs in-process under the existing Phase 1 exception
- GIVEN the `diagnostics` toolset serving diagnostic command requests
- WHEN a request is dispatched to it
- THEN it executes in-process, per
  `add-phase1-command-execution-exception`'s exception — not as a new,
  separately-justified deviation

### Requirement: Static/dynamic per-tool typing within a toolset's catalog
A toolset's catalog MAY declare, per tool, whether that tool is
`static` (a fixed, catalog-declared command; any caller-supplied
command override is discarded unconditionally, never partially
honored) or `dynamic` (no catalog-declared command; the caller
supplies the command, gated only by a blocklist check, never a
program-pinning check). This is `diagnostics`' resolution of its own
override-safety gap, not a general requirement imposed on every
toolset's catalog shape — a toolset with no caller-supplied-command
concept at all is unaffected.

#### Scenario: A static tool's override is never honored
- GIVEN a catalog tool declared `"type": "static"`
- WHEN a request for that tool includes a `command` field
- THEN the catalog's own declared command runs, and the request's
  `command` field is discarded without being validated or partially
  applied

#### Scenario: A dynamic tool's override is gated only by the blocklist
- GIVEN a catalog tool declared `"type": "dynamic"`
- WHEN a request for that tool includes a `command` field
- THEN that command runs if its first argument token (by exact match
  or basename) is not on the blocklist, with no program-pinning check
  applied

### Requirement: A toolset MAY use a permanent, non-generic wire framing
A toolset MAY keep a wire format that predates this project's generic
MCP `tools/call`/JSON-RPC 2.0 framing as its **permanent** framing,
rather than converting to the generic shape, when an explicit decision
records this as intentional rather than a pending-conversion gap. The
`diagnostics` toolset's legacy msgpack `{tool, command}` request /
`{tool, exit_code, stdout}` response shape is the first such case.

#### Scenario: `diagnostics` requests are never expected in generic JSON-RPC framing
- GIVEN a cloud caller targeting the `diagnostics` toolset
- WHEN it sends a request
- THEN it uses the legacy msgpack `{tool, command}` shape, and this is
  not treated as a gap to be closed by a future generic-framing
  conversion
