# Design: `tools/list` Metadata Placement and ACL Scoping

## Decision 1: descriptive per-method metadata is a sibling field, not embedded in `inputSchema`

### Alternatives considered

**A. Vendor-extension keys inside each `oneOf` branch** (what
`add-triage-skillset-mapping-phase1`'s draft originally proposed:
`x-rdk-load_type`, `x-rdk-version`, `x-rdk-timeout_ms` as extra
properties alongside `method`/`params` in each branch). Rejected:
conflates two different questions — "what arguments does this method
take" (what `inputSchema` is for) and "what is this method/plugin"
(descriptive metadata). A generic tool that walks `inputSchema` to
build a call form or validate arguments now has to know to ignore
`x-rdk-*` keys specifically, and some JSON Schema validators reject
unknown keys under `additionalProperties: false`-style strictness —
this makes correctness depend on every consumer's schema strictness
setting, not on the shape itself.

**B. Rely entirely on `capability-sync`'s push; carry nothing in
`tools/list`.** Rejected: `capability-sync` pushes to "the cloud's
Device Model Mapping/Tool Catalog" (`capability-sync/spec.md`) — a
system of record, not every individual caller. `define-toolset-as-mcp-tool-model`
also permanently keeps a plain, non-MCP JSON-RPC path alongside
`tools/call` (Decision, A12) for callers with no reason to speak MCP's
framing — nothing requires such a caller to have ever received or
subscribed to the cloud-side push. A caller that only ever talks to
the device directly, live, needs a way to get this data from the
device itself.

**C. A sibling structured field on the `tools/list` entry, parallel to
`inputSchema` (adopted).** Each toolset's `tools/list` entry gains a
`methods` array — one entry per method, keyed by method name, carrying
`load_type`/`version`/`timeout_ms` (or whatever descriptive fields a
given toolset has) — sitting alongside `name`/`description`/`inputSchema`,
not inside it. A generic MCP client that only understands the standard
fields ignores `methods` harmlessly (it's additive, not a
schema-breaking change to `inputSchema` itself); an RDK-aware caller
reads it directly. `inputSchema`'s `oneOf` branches stay pure argument
schemas — `method` (a `const`) and `params` only, exactly what
`define-toolset-as-mcp-tool-model/design.md` originally specified,
unmodified by this decision.

### Shape

```json
{
  "name": "triage",
  "description": "Triage-plane capability discovery and evidence-capture entry points",
  "inputSchema": {
    "oneOf": [
      {"properties": {"method": {"const": "wifi-radio-reset"}, "params": {"type": "object"}}, "required": ["method"]},
      {"properties": {"method": {"const": "dispatcher-self-check"}, "params": {"type": "object"}}, "required": ["method"]}
    ]
  },
  "methods": [
    {"name": "wifi-radio-reset", "load_type": "dynamic", "version": "1.2.0", "timeout_ms": 200},
    {"name": "dispatcher-self-check", "load_type": "static", "version": "1.0.0", "timeout_ms": 100}
  ]
}
```

`methods[].name` correlates to the matching `oneOf` branch's `method`
`const` value — the two arrays are meant to be read side by side, not
merged. A toolset with nothing descriptive to add beyond what
`inputSchema` already says (most toolsets, plausibly) may simply omit
`methods` entirely; it's optional, not a new mandatory field every
toolset must populate.

## Decision 2: `tools/list` names every loaded toolset; per-toolset detail is scoped by the caller's existing ACL grant

### Alternatives considered

**A. Fully unfiltered — every caller sees every toolset's full
detail.** Rejected: `dispatch-core/spec.md`'s "Single ACL checkpoint"
requirement says access control is enforced "exactly once... for
every request" — `tools/list` is a request the device receives and
answers, so reading it as exempt requires an explicit carve-out this
project has never actually written down. Absent that carve-out, full
disclosure is the harder position to defend, not the safer default.

**B. Fully filtered — a toolset the caller has no grant for doesn't
appear in the listing at all.** Rejected: breaks discoverability in a
way that's actively confusing, not just conservative. A caller can't
distinguish "this toolset doesn't exist on this device" from "this
toolset exists but I'm not allowed to see it," which makes debugging a
missing-permission situation harder than it needs to be, and makes the
catalog's size vary caller-to-caller in a way nothing else in this
project's discovery model does (`capability-sync`'s push, by contrast,
is a single per-device catalog, not filtered per eventual consumer).

**C. Two-tier: every loaded toolset's name always appears; per-toolset
detail (`inputSchema`, `methods`) is populated only for toolsets the
caller has at least read access to (adopted).** A toolset outside the
caller's grant still appears in the `tools` array by `name`, with
`inputSchema`/`methods` replaced by an explicit marker rather than
silently omitted or left empty:

```json
{"name": "docsis", "access_restricted": true}
```

This mirrors `acl-policy-store/spec.md`'s existing write-implies-read
tiering (a grant already comes in degrees, not just yes/no) instead of
inventing a new access model for `tools/list` specifically, and keeps
the single-checkpoint requirement's "exactly once... for every
request" honest: building a `tools/list` response means calling
`acl_policy_store_query(caller, toolset, method)` — the same function
`tools/call`'s checkpoint already uses — once per loaded toolset (a
coarse existence/read check, not per method), not a new interface or a
second, parallel authorization mechanism. This is a new *call pattern*
(N queries to build one filtered listing, instead of one query to gate
one invocation) against an already-existing function, not new
authorization logic.

**Cost, named rather than left implicit**: building a `tools/list`
response now costs O(loaded toolsets) ACL Policy Store queries instead
of one. Given the Policy Store is in-memory/hot-reloadable
(`acl-policy-store/spec.md`, "Hot-reloadable policy" — no network
round trip implied by that requirement), this is not expected to be a
real performance concern at the toolset counts this project has
discussed so far, but it's worth sizing once a real toolset count
exists to test against, same caution `sandboxed-runtime`'s footprint
questions already apply elsewhere in this project.

### What counts as "read access" for this purpose

Reuses `acl-policy-store/spec.md`'s existing write-implies-read
default unchanged: any grant with write access to a toolset already
has read, so already sees full detail. A caller with only a
"discovery"-scoped read grant (the same shape
`add-triage-skillset-mapping-phase1`'s original, now-removed ACL
section anticipated for triage specifically) also sees full detail,
without gaining write/execute. A caller with no grant at all on a
toolset sees only its name and the `access_restricted` marker. No new
grant type or policy file format is introduced — this decision is
entirely about how `tools/list`'s response-building step *uses* grants
that already exist.

## File/Component Changes

- `dispatch-core/spec.md`: add the two-tier `tools/list` visibility
  requirement (Decision 2).
- `toolset-lifecycle/spec.md`: add the `methods` sibling-field
  requirement for descriptive per-method metadata (Decision 1).
- `add-triage-skillset-mapping-phase1/design.md`: once this change is
  confirmed, update its `tools/list` response example to match this
  shape (currently shows the rejected `x-rdk-*`-in-`inputSchema`
  approach, explicitly flagged there as unresolved) and remove its
  "not resolved in this correction" ACL note, replacing it with a
  pointer here.
- `define-toolset-as-mcp-tool-model/design.md`: no change to its core
  `tools/list` decision (one entry per toolset, coarse `oneOf`) — this
  change is additive to it, not a revision.
