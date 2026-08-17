# Design: Phase 1 Triage Skillset Mapping over WRP

**Corrected 2026-08-16, second of two required corrections** (see
`define-toolset-as-mcp-tool-model/tasks.md` §4 and `design.md`'s
"File/Component Changes"). The first correction that section pointed
at (`define-plane-vs-toolset-model/tasks.md` §3, "triage is a plane,
not a toolset") turned out, on re-reading that change's own later
addendum, to already be satisfied — see that document's "Consequence:
Phase 1's original 'Triage Toolset' framing was right after all"
(added 2026-08-14): a dedicated Triage Toolset is one of two
architecturally valid shapes under Decision B ("a single 'wifi'
toolset can have its own... triage logic inside it, or a dedicated
'triage' toolset can exist on its own — either is architecturally
valid"), so nothing about *that* needed changing here. What did still
need correcting, independent of the toolset-vs-plane question: this
document invented a bespoke `triage.capabilities` JSON-RPC method as
the discovery mechanism, predating `define-toolset-as-mcp-tool-model`'s
later decision that toolset self-description is already a generic,
project-wide primitive (`tools/list`) — a second, toolset-specific
discovery method duplicates that rather than using it. That's what
this correction fixes; everything about the *internal* static/dynamic
plugin merge logic below was already correct and is unchanged.

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
scoped. This `tools/list` query (corrected 2026-08-16, formerly a
bespoke `triage.capabilities` call — see below) is a read/discovery
call, not a toolset-push-style "toolset plus commands" message, so the
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
  "dest": "mac:112233445566/rdk-dispatcher",
  "content_type": "application/json",
  "accept": "application/json",
  "transaction_uuid": "b3b1e6b0-1e34-4b7a-9b1a-2f6a9a9c8f10",
  "payload": {
    "jsonrpc": "2.0",
    "method": "tools/list",
    "params": {},
    "id": "b3b1e6b0-1e34-4b7a-9b1a-2f6a9a9c8f10"
  }
}
```

Note the `dest` no longer names `.../rdk-dispatcher/triage` — `tools/list`
is a Dispatch Core-level MCP method, not addressed to any one toolset;
it lists every currently loaded toolset, triage included. A cloud
client wanting only triage's entry filters the returned array
client-side for `name: "triage"`; the device does not offer a
server-side filtered variant of `tools/list` (adding one would be a
second, bespoke discovery surface — exactly what this correction
removes).

**Response (device → cloud), showing only the `triage` entry from the
full `tools/list` array (other loaded toolsets' entries omitted for
brevity). Shape confirmed 2026-08-16 per
`resolve-tools-list-metadata-and-acl-scoping` — `inputSchema` carries
only argument shape; descriptive metadata lives in the sibling
`methods` field:**

```json
{
  "msg_type": 3,
  "source": "mac:112233445566/rdk-dispatcher",
  "dest": "dns:skillset-mapper.xmidt.example.com/svc",
  "content_type": "application/json",
  "transaction_uuid": "b3b1e6b0-1e34-4b7a-9b1a-2f6a9a9c8f10",
  "status": 200,
  "payload": {
    "jsonrpc": "2.0",
    "id": "b3b1e6b0-1e34-4b7a-9b1a-2f6a9a9c8f10",
    "result": {
      "tools": [
        {
          "name": "triage",
          "description": "Triage-plane capability discovery and evidence-capture entry points",
          "inputSchema": {
            "oneOf": [
              {
                "properties": {
                  "method": {"const": "wifi-radio-reset"},
                  "params": {"type": "object"}
                },
                "required": ["method"]
              },
              {
                "properties": {
                  "method": {"const": "dispatcher-self-check"},
                  "params": {"type": "object"}
                },
                "required": ["method"]
              }
            ]
          },
          "methods": [
            {"name": "wifi-radio-reset", "load_type": "dynamic", "version": "1.2.0", "timeout_ms": 200},
            {"name": "dispatcher-self-check", "load_type": "static", "version": "1.0.0", "timeout_ms": 100}
          ]
        }
      ]
    }
  }
}
```

**If the calling identity has no ACL grant on `triage` at all**, this
entry is instead `{"name": "triage", "access_restricted": true}` — no
`inputSchema`/`methods` — per the same confirmed two-tier visibility
model. See the ACL discussion further below.

### Decision: reuse `define-toolset-as-mcp-tool-model`'s generic `tools/list`, not a second discovery method

**Corrected 2026-08-16 — this decision replaces the original "reuse
FR-1's JSON-RPC framing" decision below it, which invented
`triage.capabilities` as its own JSON-RPC method.** By the time that
original decision was written, `define-toolset-as-mcp-tool-model` had
not yet established `tools/list` as the project-wide, generic
mechanism for exactly this question ("what can this toolset do") —
once it did, a toolset-specific `<domain>.capabilities` method became
redundant with a primitive every toolset already gets for free.
Keeping both would mean two ways to ask the same question, with two
response shapes to keep in sync — precisely the kind of duplication
`toolset-lifecycle/spec.md`'s "Self-described schema" requirement
(`capabilities()`/`schema()` authoritative, not duplicated elsewhere)
already warns against, just one level up (at the discovery-method
level rather than the data level).

So: no `triage.capabilities` method exists. A cloud client asks what
triage can do the same way it asks what any toolset can do — `tools/list`,
per `define-toolset-as-mcp-tool-model/design.md`'s "`tools/list`
aggregates across every currently loaded toolset — one MCP tool per
toolset, not per method" decision. The Triage Toolset's `name` in that
catalog is `"triage"`; its `inputSchema` is the `oneOf`
discriminated-union shape that decision defines, one branch per
internal triage sub-plugin's method.

**Resolved 2026-08-16, project-wide, by direct instruction**: A13's
`oneOf` schema shape covers a method's *argument* shape only, never
descriptive metadata like `load_type`/`version`/`timeout_ms` about the
plugin backing that method. `openspec/changes/resolve-tools-list-metadata-and-acl-scoping/`
(confirmed) settles this: descriptive metadata lives in a sibling
`methods` field on the `tools/list` entry, not embedded in `inputSchema`.
The response example above reflects this confirmed shape — see that
change's `design.md` Decision 1 for the full reasoning.

Every part of this phase's `capabilities()` implementation (the
static/dynamic merge described further below) is unaffected by this
correction: it still produces exactly the same underlying data
(`plugin`, `load_type`, `version`, `events`, `timeout_ms` per triage
sub-plugin) that Schema & Discovery already required
(`RDK_Dispatcher_Architecture_and_Requirements.md` §4.4) — only the
wire-level *question* the cloud asks to get at it changed, from a
bespoke method to the generic one.

### `tools/list` visibility: two-tier, ACL-scoped — resolved 2026-08-16

**Corrected 2026-08-16 — this replaces the original "read op, not
exempt from FR-4's single ACL checkpoint" decision**, which assumed
`triage.capabilities` was its own ACL-gated method, gated the same way
a business method is. Under `tools/list`, that framing didn't directly
apply — `tools/list` is a catalog listing, not itself an invocation of
any one toolset's method.

**Resolved, project-wide, by direct instruction**:
`openspec/changes/resolve-tools-list-metadata-and-acl-scoping/`
(confirmed) settles this with a two-tier model — every loaded toolset
is always named in `tools/list`; per-toolset detail (`inputSchema`,
`methods`) is gated by the caller's existing ACL grant via
`acl_policy_store_query()`, the same function `tools/call`'s
checkpoint already uses. A caller with no grant on `triage` still sees
`{"name": "triage", "access_restricted": true}` in the listing — the
toolset's existence isn't hidden, only its detail. See that change's
`design.md` Decision 2 for the full reasoning, and the response
example above for what this looks like on the wire. What's unchanged
and still true regardless: *invoking* a triage method (once execution
lands,
out of scope for this Phase 1 discovery-only change) still goes
through the single ACL checkpoint (FR-4) exactly like any other
toolset's method.

### Decision: static + dynamic plugin merge happens inside the Triage Toolset process, not Dispatch Core

**Unchanged by this correction** — this was already right; only the
outer discovery method around it changed (above).

`reference-impl/dispatcher_core.c`'s existing `dispatcher_load_plugins()`
sketch `dlopen()`s plugins directly into whatever process runs it. Per
FR-13, toolset plugins must run out-of-process from Dispatch Core — so
for this phase, that loader logic (and the new static-registration
counterpart added below) belongs inside the **Triage Toolset's own
process**, which Plugin Manager starts and supervises like any other
toolset (confirmed still valid — `define-plane-vs-toolset-model/design.md`'s
"Consequence: Phase 1's original 'Triage Toolset' framing was right
after all"). Dispatch Core never touches plugin `.so` files directly;
it only ever calls the Triage Toolset's `capabilities()`/`schema()`
through the normal resolved-handle path (§4.6), now surfaced to the
cloud via `tools/list`'s projection of that data rather than a
dedicated method.

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
  lists into one internal descriptor set; Schema & Discovery projects
  that set into the `tools/list` entry's `inputSchema.oneOf` shape
  above — `load_type` still lets the cloud (and a device operator) see
  which plugins are compiled-in vs. field-upgradable without a full
  image rebuild, just via the generic catalog now instead of a
  dedicated response field.

### Relation to `capability-sync/spec.md`

**Unchanged by this correction**, other than replacing the pull
method's name.

`capability-sync` is **device-initiated, event-triggered push** (on
Plugin Manager load/unload/reload) authenticated by device identity —
unchanged by this phase, and also unchanged by
`define-toolset-as-mcp-tool-model`'s later "permanently separate
deliveries, one shared trigger point" decision, which this phase
predates but does not conflict with. This phase adds a
**cloud-initiated pull** of the same underlying `capabilities()` data
— now via `tools/list` rather than a bespoke method — authenticated by
the normal per-session SAT token/ACL path (not device identity). The
two are complementary, not overlapping: push keeps the cloud's catalog
current without polling in the common case ("push is primary,
`tools/list` is secondary," per `define-toolset-as-mcp-tool-model/design.md`);
pull lets a cloud client ask on demand (e.g. right before dispatching a
triage-driven session, without waiting for or trusting the last push).

## File/Component Changes

- Triage Toolset process (new): static plugin registration table +
  reuse of the `dispatcher_load_plugins()` dynamic loader pattern,
  scoped to its own process per FR-13. **Unchanged by this correction.**
- `reference-impl/plugin_contract.h`: add `load_type` (`"static"` |
  `"dynamic"`) and `version` fields to `plugin_descriptor_t`.
  **Unchanged by this correction.**
- Dispatch Core: no change beyond what `define-toolset-as-mcp-tool-model`
  already specifies for `tools/list` project-wide — this change adds
  no Dispatch Core code of its own; `triage.capabilities` as a
  standalone JSON-RPC method is removed from this change's scope
  entirely, not just relocated.
- `capability-sync/spec.md`: unchanged this phase (see "Relation to
  capability-sync" above).
- `specs/triage/spec.md` (this change's own delta): the "WRP-framed
  skillset mapping request/response" requirement is removed —
  discovery framing is now `define-toolset-as-mcp-tool-model`'s
  concern, not re-specified here. See that file directly for what
  remains.
