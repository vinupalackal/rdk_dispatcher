# Delta for triage (new domain)

## ADDED Requirements

### Requirement: WRP-framed skillset mapping request/response
The system SHALL accept a cloud-initiated skillset mapping request as
a WRP Simple Request-Response message (`msg_type: 3`) with
`content_type: application/json`, whose payload is a JSON-RPC 2.0
request for method `triage.capabilities`, and SHALL respond with a
WRP message of the same `msg_type`, matching `transaction_uuid`, and
a JSON-RPC 2.0 result payload listing the device's triage
capabilities.

#### Scenario: Cloud requests the triage skillset map
- GIVEN a device connected to XMiDT with an active Triage Toolset
- WHEN the cloud sends a WRP `msg_type: 3` request with payload
  `{"jsonrpc":"2.0","method":"triage.capabilities","params":{},"id":"..."}`
- THEN the device responds with a WRP `msg_type: 3` message carrying
  the same `transaction_uuid`, `status: 200`, and a JSON-RPC result
  payload listing every currently loaded triage plugin

#### Scenario: Unauthorized or malformed request does not go unanswered
- GIVEN a skillset mapping request that fails ACL or JSON-RPC
  validation
- WHEN Dispatch Core rejects it
- THEN the device still returns a WRP `msg_type: 3` response with the
  same `transaction_uuid`, a non-200 `status`, and a JSON-RPC `error`
  object — the request is never silently dropped

### Requirement: Static and dynamic plugin capability merge
The Triage Toolset SHALL merge capability descriptors from both
statically compiled-in plugins and dynamically (`dlopen()`-)loaded
plugins into a single `triage.capabilities` response, tagging each
entry with a `load_type` of `"static"` or `"dynamic"`.

#### Scenario: Response includes both plugin kinds
- GIVEN a Triage Toolset process with one compiled-in plugin and one
  `.so` plugin loaded from `/usr/libexec/dispatcher/triage/`
- WHEN `triage.capabilities` is called
- THEN the response's `capabilities` array contains both entries,
  each correctly tagged by `load_type`

#### Scenario: A dynamic plugin fails to load
- GIVEN a `.so` file in the triage plugin directory missing
  `describe()` or `handle()`
- WHEN the Triage Toolset process starts
- THEN that plugin is skipped with a logged error, and does not
  appear in subsequent `triage.capabilities` responses — static
  plugins and other valid dynamic plugins are unaffected

### Requirement: Single ACL checkpoint applies to capability discovery
`triage.capabilities` SHALL be authorized through Dispatch Core's
single ACL checkpoint like any other method — it is not a bypass
exempt from `dispatch-core/spec.md`'s enforcement.

#### Scenario: Read-only discovery access without triage write access
- GIVEN an identity whose ACL group grants read-only access scoped to
  `triage.capabilities`
- WHEN that identity sends the skillset mapping request
- THEN the request is authorized for `triage.capabilities`
  specifically, without granting write or execute access to any
  triage plugin's other methods

### Requirement: Phase 1 scope boundary
This domain's Phase 1 SHALL cover only capability discovery
(`triage.capabilities`) for the triage plane. It SHALL NOT cover
triage event execution, evidence capture, or `trace_id`-correlated
async enqueue behavior — those remain out of scope until a later
change extends this domain.

#### Scenario: Capability discovery does not trigger triage execution
- GIVEN a device that receives a `triage.capabilities` request
- WHEN the Triage Toolset responds with its capability list
- THEN no triage plugin's `handle()` is invoked as part of answering
  the request — discovery and execution are independent paths
