# Delta for capability-sync

## ADDED Requirements

### Requirement: Distinct from MCP's live tool-change notification, fanned out from one shared trigger point
The system's event-triggered, device-identity-authenticated capability
push to Device Model Mapping/Tool Catalog SHALL remain a separate
delivery mechanism from `dispatch-core/spec.md`'s
`notifications/tools/list_changed`, permanently — neither SHALL become
the other's source. Both SHALL originate from a single internal
emission point in Plugin Manager's load/unload/reload path (not two
independently registered listeners), read the same underlying
`capabilities()`/`schema()` data, but SHALL be delivered via fully
independent downstream queues with no shared retry/backoff logic and
no ordering dependency between them.

**(Revised 2026-08-13, per direct confirmation:** resolves the prior
"worth revisiting later" status — permanent separation confirmed, with
the shared-emission-point requirement added to close the drift risk
two independently-written listeners would otherwise carry.)

#### Scenario: Both mechanisms fire from one reload, independently
- GIVEN a toolset reload event
- WHEN Plugin Manager completes the reload
- THEN the device-identity-authenticated capability-sync push fires to
  Device Model Mapping/Tool Catalog, and, independently, any actively
  connected MCP client receives `notifications/tools/list_changed` —
  neither delivery depends on the other succeeding or existing

#### Scenario: A slow catalog backend does not delay the live notification
- GIVEN a toolset reload event
- AND the Device Model Mapping/Tool Catalog backend is unreachable,
  triggering the capability-sync push's retry/backoff cycle
- WHEN an MCP client is actively connected at the moment of reload
- THEN it still receives `notifications/tools/list_changed` promptly,
  unaffected by the catalog push's retry state

#### Scenario: A new reload-triggering code path cannot silently skip one delivery
- GIVEN a future change that adds a new way for a toolset's schema to
  change (e.g. a re-sandboxing event with unchanged capabilities)
- WHEN that change is implemented correctly, per this requirement
- THEN it can only reach either delivery mechanism by calling Plugin
  Manager's single `toolset_changed` emission point — there is no way
  to trigger the capability-sync push without also triggering
  `notifications/tools/list_changed`, or vice versa

### Requirement: Device publishes its toolset list on session establishment
The device SHALL proactively publish its full toolset list via this
push mechanism as soon as a session with the cloud is established, in
addition to the existing load/unload/reload triggers. This publish is
the primary discovery path; `tools/list` (see `dispatch-core/spec.md`)
is a secondary, on-demand confirmation of the same data, not the
mechanism a cloud client should rely on to learn about a device for
the first time.

#### Scenario: Cloud learns a device's toolsets without querying first
- GIVEN a device establishing a new session with the cloud
- WHEN the session is established
- THEN the device publishes its current toolset list before any
  cloud-initiated `tools/list` request is expected or required

(Added 2026-08-13, per direct confirmation.)
