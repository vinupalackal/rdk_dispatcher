# Delta for dispatch-core

## ADDED Requirements

### Requirement: MCP tool method surface
The system SHALL implement `initialize`, `tools/list`, `tools/call`,
and `notifications/tools/list_changed` as JSON-RPC 2.0 methods over
the existing XMiDT/WRP/Parodus transport (see Transport requirement).
No additional transport SHALL be introduced to support this.

#### Scenario: MCP client discovers toolsets
- GIVEN an MCP-compliant cloud client with an active XMiDT session
- WHEN it sends a `tools/list` request
- THEN Dispatch Core responds with every currently loaded toolset's
  methods, each expressed as one MCP tool definition

#### Scenario: Toolset reload triggers a live notification
- GIVEN an MCP client connected when a toolset is reloaded with a new
  method
- WHEN Plugin Manager completes the reload
- THEN Dispatch Core sends `notifications/tools/list_changed` to that
  client without the client needing to poll

### Requirement: `tools/call` uses the standard command path
`tools/call` SHALL be authorized, resolved, and executed through the
identical path as any other JSON-RPC command request — the single ACL
checkpoint, Plugin Manager resolution, Execution Framework, and
sandboxed toolset execution. It SHALL NOT constitute a second,
independent command-authorization path.

#### Scenario: `tools/call` is denied exactly like an equivalent direct call
- GIVEN an identity without permission for `wifi.setChannel`
- WHEN it sends a `tools/call` request naming `wifi.setChannel`
- THEN Dispatch Core denies it via the same ACL Policy Store check
  that would deny an equivalent non-MCP JSON-RPC request for the same
  method
