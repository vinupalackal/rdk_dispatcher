# Design: Phase 1 Triage Skillset Mapping over WRP

## Technical Approach

### WRP envelope

Both request and response use WRP's **Simple Request-Response**
message type (`msg_type = 3` — per [xmidt.io's WRP spec](https://xmidt.io/docs/wrp/simple-messages/),
the same message type is used for both directions of a point-to-point
request/response pair; only `status` distinguishes a response). This
project represents WRP in its JSON serialization (not msgpack), per
WRP's own stated design goal of translating cleanly to/from JSON.

**Consistent with the Phase 1/Phase 2 split confirmed in
`require-payload-encryption-and-message-routing`** (`OPEN_QUESTIONS.md`
A7): the payload below is plain JSON-RPC, unencrypted — Phase 1 as
scoped. This `triage.capabilities` query is a read/discovery call, not
a toolset-push-style "toolset plus commands" message, so the
`static`/`dynamic` type field that change introduces doesn't apply to
it directly; that field is for toolset push and command-carrying
messages specifically. Verification (where relevant — this query
carries no artifact to verify) and ACL enforcement are unaffected
either way.

**Request (cloud → device):**

```json
{
  "msg_type": 3,
  "source": "dns:skillset-mapper.xmidt.example.com/svc",
  "dest": "mac:112233445566/rdk-dispatcher/triage",
  "content_type": "application/json",
  "accept": "application/json",
  "transaction_uuid": "b3b1e6b0-1e34-4b7a-9b1a-2f6a9a9c8f10",
  "payload": {
    "jsonrpc": "2.0",
    "method": "triage.capabilities",
    "params": {},
    "id": "b3b1e6b0-1e34-4b7a-9b1a-2f6a9a9c8f10"
  }
}
```

**Response (device → cloud):**

```json
{
  "msg_type": 3,
  "source": "mac:112233445566/rdk-dispatcher/triage",
  "dest": "dns:skillset-mapper.xmidt.example.com/svc",
  "content_type": "application/json",
  "transaction_uuid": "b3b1e6b0-1e34-4b7a-9b1a-2f6a9a9c8f10",
  "status": 200,
  "payload": {
    "jsonrpc": "2.0",
    "id": "b3b1e6b0-1e34-4b7a-9b1a-2f6a9a9c8f10",
    "result": {
      "toolset_plane": "triage",
      "schema_version": "1",
      "capabilities": [
        {
          "plugin": "wifi-triage",
          "load_type": "dynamic",
          "version": "1.2.0",
          "events": ["wifi-radio-reset", "wifi-radio-reset-timeout"],
          "timeout_ms": 200
        },
        {
          "plugin": "core-triage",
          "load_type": "static",
          "version": "1.0.0",
          "events": ["dispatcher-self-check"],
          "timeout_ms": 100
        }
      ],
      "generated_at": "2026-08-10T00:00:00Z"
    }
  }
}
```

### Decision: reuse FR-1's JSON-RPC framing inside the WRP payload

`dispatch-core/spec.md` already requires JSON-RPC 2.0 request/response
framing over the existing XMiDT/WRP/Parodus transport (FR-1). Rather
than invent a second, WRP-payload-specific schema for "skillset
mapping," this phase's request/response `payload` **is** a standard
JSON-RPC 2.0 call — `method: "triage.capabilities"` — against Schema &
Discovery's existing `capabilities()` (per
`RDK_Dispatcher_Architecture_and_Requirements.md` §4.4). This keeps
one request format instead of two, and means the triage-mapping call
goes through Dispatch Core's normal path: ACL check, Plugin Manager
resolution to the `triage` toolset, Schema & Discovery's
`capabilities()` on that toolset.

### Decision: this is a read op, not exempt from FR-4's single ACL checkpoint

`triage.capabilities` is still a Dispatch Core-mediated call and is
still subject to the one ACL checkpoint (FR-4) — it is not a
special-cased bypass. It is a **read**, so under the ACL Policy
Store's "write implies read" default (§4.5), any identity/group with
write access to the triage toolset already has this; a lower-privilege
"discovery" group can also be granted read-only access to it without
granting triage write/execute — see `triage/spec.md`'s ACL scenario.

### Decision: static + dynamic plugin merge happens inside the Triage Toolset process, not Dispatch Core

`reference-impl/dispatcher_core.c`'s existing `dispatcher_load_plugins()`
sketch `dlopen()`s plugins directly into whatever process runs it. Per
FR-13, toolset plugins must run out-of-process from Dispatch Core — so
for this phase, that loader logic (and the new static-registration
counterpart added below) belongs inside the **Triage Toolset's own
process**, which Plugin Manager starts and supervises like any other
toolset. Dispatch Core never touches plugin `.so` files directly; it
only ever calls the Triage Toolset's `capabilities()` through the
normal resolved-handle path (§4.6).

Within that process:
- **Static** triage plugins are linked directly into the Triage
  Toolset binary and registered via a static table at process init —
  `describe()` is called directly, no `dlopen()`.
- **Dynamic** triage plugins are discovered from
  `/usr/libexec/dispatcher/triage/*.so` at init, exactly as
  `dispatcher_load_plugins()` already sketches.
- Both paths produce a `plugin_descriptor_t` (extended this phase with
  `load_type` and `version` — see `reference-impl/plugin_contract.h`).
  The Triage Toolset's own `capabilities()` implementation merges both
  lists into the single response shape above; `load_type` lets the
  cloud (and a device operator) see which plugins are compiled-in vs.
  field-upgradable without a full image rebuild.

### Relation to `capability-sync/spec.md`

`capability-sync` is **device-initiated, event-triggered push** (on
Plugin Manager load/unload/reload) authenticated by device identity —
unchanged by this phase. This phase adds a **cloud-initiated pull** of
the same underlying `capabilities()` data, authenticated by the normal
per-session SAT token/ACL path (not device identity). The two are
complementary, not overlapping: push keeps the cloud's catalog current
without polling in the common case; pull lets a cloud client ask on
demand (e.g. right before dispatching a triage-driven session, without
waiting for or trusting the last push). A future change could unify
them under one `capabilities()`-sourced code path with two delivery
mechanisms — noted here, not solved in this phase.

## File/Component Changes

- Triage Toolset process (new): static plugin registration table +
  reuse of the `dispatcher_load_plugins()` dynamic loader pattern,
  scoped to its own process per FR-13.
- `reference-impl/plugin_contract.h`: add `load_type` (`"static"` |
  `"dynamic"`) and `version` fields to `plugin_descriptor_t`.
- Dispatch Core: no change — routes `triage.capabilities` through the
  existing JSON-RPC/ACL/Plugin-Manager-resolution path unmodified.
- `capability-sync/spec.md`: unchanged this phase (see "Relation to
  capability-sync" above).
