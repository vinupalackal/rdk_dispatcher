# Phase 1 Requirements Document (Beginner Edition, with Examples)

This document states exactly what Phase 1 of the RDK Dispatcher must
do, in plain language, with a worked example under every requirement.
It reflects the project's current, confirmed scope — including the
2026-08-14 decision (`add-phase1-command-execution-exception`) that
brought real command execution and `toolset.push` into Phase 1
alongside its original read-only discovery scope. Every requirement
below traces back to a master FR/NFR in
`RDK_Dispatcher_Architecture_and_Requirements.md`, an OpenSpec change,
or an `OPEN_QUESTIONS.md` decision ID — nothing here is invented fresh
for this document.

## 1. What is the RDK Dispatcher, in one paragraph

Think of a set-top box or router as a little computer with lots of
capabilities — it can check WiFi status, reset a radio, read a
DOCSIS metric, apply a config change. Historically, RDK devices used
`rpcd`/`ubus` (borrowed from OpenWrt) to let outside callers (the
cloud, or a local process) ask a device to do one of these things.
The RDK Dispatcher is a brand-new replacement for that layer, built
from scratch, that lets the cloud (or an AI agent, via MCP) discover
what a device can do and ask it to do something — safely, with one
consistent permission check, regardless of which specific capability
is being invoked.

## 2. Glossary (read this before the requirements below)

**WRP** — Web Routing Protocol. The envelope every message travels in
between the cloud and the device, over a connection called XMiDT/
Parodus. Think of it like an envelope with a "to" and "from" address;
whatever's actually being asked goes inside as the letter.

**JSON-RPC 2.0** — a simple, standard way to write "call this method
with these arguments" as JSON text. This project puts a JSON-RPC 2.0
message inside every WRP envelope's "letter."

**Toolset** — a self-contained unit of device capability (e.g. "wifi,"
"triage"). Each toolset owns its own list of methods it can perform
and describes itself, rather than a central list knowing about every
method in the system.

**Plane** — a label describing *what kind* of thing a toolset's logic
does (config-apply, management, control, or triage). It's just a tag,
not a different security or execution model — see `OPEN_QUESTIONS.md`
A2/A3.

**ACL (Access Control List) checkpoint** — the one place in the whole
system that decides "is this caller allowed to do this." Every
request passes through exactly one of these, no exceptions.

**`toolset.push`** — a way to deliver new or updated toolset code (or
a toolset definition) to a device directly over the same connection
used for commands, without the slower, asynchronous RDM install
process.

**RDM (Remote Download Manager)** — the existing, established
mechanism for installing/rolling back a device's firmware-level or
binary-level software, verified and controlled.

**SAT token** — a short-lived credential (like a temporary badge) a
caller presents with each request, proving who they are and what
they're allowed to do, without the device needing to remember a
server-side session.

**In-process / out-of-process, sandboxed** — running "in-process"
means a toolset's code runs inside the same program as Dispatch Core
itself (faster to build, less isolated). "Out-of-process, sandboxed"
means it runs as its own separate, walled-off program (safer, more
work to build). Phase 1 deliberately uses in-process for now, as an
explicit, tracked exception — see requirement P1-6 below.

## 3. Phase 1 scope summary

Phase 1 now covers two things, not one:

1. **Read-only capability discovery** — a cloud caller can ask "what
   triage capabilities does this device have," and get back a list.
   This was Phase 1's original scope.
2. **Real command execution and `toolset.push`** — a cloud caller can
   now also actually invoke a command against a device, and new
   toolset code (or a toolset definition) can be pushed to the device
   directly. This is new as of 2026-08-14, added as a narrow,
   explicitly tracked exception (see requirement P1-6).

Everything else the project has designed — full out-of-process
sandboxing for every toolset, payload encryption, RBUS/Thunder
mapping, the independent security review — stays in Phase 2. Phase 1
is not a hardened, production-ready implementation; it's a real,
working, but deliberately narrower slice, with every gap named
explicitly rather than silently skipped.

## 4. Functional requirements, with beginner examples

### P1-1 — JSON-RPC 2.0 over the existing WRP transport (traces to FR-1)

Every Phase 1 request and response is a JSON-RPC 2.0 message, carried
inside a WRP envelope (`msg_type: 3`), over the existing XMiDT/Parodus
connection. No new transport is introduced for Phase 1.

**Beginner example:** picture mailing a letter (JSON-RPC) inside an
envelope (WRP) through the existing postal system (XMiDT/Parodus) —
Phase 1 doesn't invent a new postal system, it just writes new kinds
of letters.

```json
{
  "msg_type": 3,
  "source": "dns:skillset-mapper.xmidt.example.com/svc",
  "dest": "mac:112233445566/rdk-dispatcher/triage",
  "content_type": "application/json",
  "transaction_uuid": "b3b1e6b0-1e34-4b7a-9b1a-2f6a9a9c8f10",
  "payload": {
    "jsonrpc": "2.0",
    "method": "triage.capabilities",
    "params": {},
    "id": "b3b1e6b0-1e34-4b7a-9b1a-2f6a9a9c8f10"
  }
}
```

### P1-2 — Every Phase 1 message carries a `type` field, and is verified regardless of type (traces to A5/A7)

Any message that pushes a definition or a command (not a plain read
like `triage.capabilities`) SHALL carry a `type` field: `"static"` for
a definition with no commands attached, `"dynamic"` for one carrying
an actual command. Both types are signature/manifest-verified before
being acted on — verification is never skipped, even though
encryption is deferred to Phase 2.

**Beginner example:** imagine two kinds of mail — a catalog (static,
just describes what's available) and an order form (dynamic, asks for
something to actually happen). Phase 1 always checks the sender's
signature on both kinds, even though it doesn't yet seal the envelope
shut with encryption (that's added in Phase 2).

```json
{
  "jsonrpc": "2.0",
  "method": "wifi.setChannel",
  "params": { "type": "dynamic", "channel": 6 },
  "id": "..."
}
```

### P1-3 — Read-only triage capability discovery (traces to `add-triage-skillset-mapping-phase1`)

A cloud caller can send `triage.capabilities` and receive a list of
every triage-plane plugin the device has, tagging each one as
`load_type: "static"` (compiled in) or `load_type: "dynamic"`
(`dlopen()`-loaded), along with its version and the events it reacts
to.

**Beginner example:** this is like asking a store "what do you sell,"
and getting back a catalog — nothing is bought or changed, just
listed.

```json
{
  "result": {
    "toolset_plane": "triage",
    "capabilities": [
      { "plugin": "wifi-triage", "load_type": "dynamic", "version": "1.2.0" },
      { "plugin": "core-triage", "load_type": "static", "version": "1.0.0" }
    ]
  }
}
```

### P1-4 — Real command execution against a resolved toolset method (new, traces to `add-phase1-command-execution-exception`)

A cloud caller can now send an actual command — not just a read — and
have Dispatch Core resolve which toolset should handle it, check that
the caller is allowed to (P1-8 below), and run it, returning a real
result.

**Beginner example:** this is the difference between asking "what can
you sell me" (P1-3) and actually saying "sell me the blue one" — an
actual action happens and something changes as a result.

```json
{
  "jsonrpc": "2.0",
  "method": "wifi.setChannel",
  "params": { "type": "dynamic", "channel": 6 },
  "id": "req-042"
}
```
```json
{
  "jsonrpc": "2.0",
  "id": "req-042",
  "result": { "status": "applied", "channel": 6 }
}
```

### P1-5 — `toolset.push` for delivering toolset code/updates directly (new, traces to `define-synchronous-toolset-push`, now in Phase 1)

A build/release pipeline identity can push a new or updated toolset
directly over the same connection, get a synchronous accept/reject
response, and have it verified before it's usable — without waiting
on RDM's full asynchronous install cycle, for artifacts that don't
require RDM's own `dlopen()`-able-binary path (see requirement P1-9).

**Beginner example:** think of RDM as ordering a part by mail (slower,
tracked, arrives days later) and `toolset.push` as handing a small
update directly to the store clerk while you're standing there
(faster, still checked, but only for things small/simple enough not
to need the mail-order process).

```json
{
  "jsonrpc": "2.0",
  "method": "toolset.push",
  "params": {
    "toolset": "wifi-triage-ext",
    "version": "1.3.0",
    "signature": "<...>",
    "signer": "release-authority-key-id"
  },
  "id": "push-017"
}
```

### P1-6 — Phase 1's command-executing toolset(s) run in-process, as a tracked exception (new, traces to A15)

Unlike every other toolset in this project's design (which must run
out-of-process and sandboxed, no exceptions — A2/A3), the specific
toolset(s) handling Phase 1's real commands and `toolset.push`
updates are explicitly permitted to run in-process for this phase
only. This is recorded as a deliberate, reviewed, time-bounded
exception — not a silent gap — and is scheduled to be hardened to the
out-of-process, sandboxed model in Phase 2.

**Beginner example:** imagine a construction site using a temporary
wooden walkway while the permanent steel bridge is still being built
— it's clearly marked as temporary, tracked on a punch list, and
scheduled to be replaced, not quietly forgotten.

### P1-7 — Toolset code and command messages stay unencrypted, but always verified (traces to A7, unchanged)

Phase 1 does not encrypt payloads at the message level (that's added
in Phase 2). Every message still travels over the existing transport
security (TLS/WSS), and every `toolset.push` or command-carrying
message is still signature/manifest-verified — confidentiality is
deferred, authenticity is not.

**Beginner example:** the letter travels in a locked mail truck (TLS)
even though the letter itself isn't individually sealed in a tamper-
evident envelope yet (payload encryption) — and every letter's
signature is still checked against who's allowed to send it.

### P1-8 — Single, mandatory ACL checkpoint for every real command (traces to FR-4, unaffected by P1-6's exception)

Every real command in Phase 1 — including ones against the in-process
exception toolset(s) — passes through exactly one access-control
check before it's allowed to run. Running in-process defers sandboxing
(a containment control); it does not defer this check (a permission
control). These are independent, and Phase 1 does not relax the
second to make up for temporarily relaxing the first.

**Beginner example:** a security guard checking ID at the one door
everyone must walk through — whether the room behind that door has
extra internal locks yet (sandboxing) or not doesn't change whether
the guard checks your ID first.

### P1-9 — `toolset.push` never carries a `dlopen()`-able binary; RDM Client still owns that (traces to A6, unchanged)

Regardless of size, any artifact meant to be `dlopen()`'d at runtime
goes through RDM Client's existing verified install pipeline — never
through `toolset.push`. This boundary is unchanged by Phase 1's
exception; only where already-delivered, already-verified code
executes (in-process for now) is affected, not which pipeline is
trusted to deliver which kind of artifact.

**Beginner example:** a courier service (`toolset.push`) that's fast
for letters and small packages, but anything that needs to go through
customs inspection (a compiled binary) still has to go through the
slower, more thorough process (RDM), no matter how small the package
is.

### P1-10 — Phase 1 `toolset.push` rollback is a health-check-gated swap, not a spawn-based fallback (traces to `add-phase1-command-execution-exception` §2)

Because Phase 1's command-executing toolset(s) run in-process (P1-6),
there's no separate process to spawn a fallback into. If a pushed
update fails its health check, the prior in-process version keeps
serving calls, and Plugin Manager simply doesn't switch over — it
never involves spawning anything.

**Beginner example:** like a live TV broadcast keeping the current
feed on air if the backup feed fails its check, rather than cutting to
black and trying to boot up spare equipment.

### P1-11 — Toolset capability reporting to the cloud (traces to FR-7, unchanged)

Whenever a toolset is loaded, unloaded, or reloaded (including via
`toolset.push`), the device reports its current capability list to
the cloud automatically, authenticated by device identity — the cloud
never has to poll to find out.

### P1-12 — Local clients get the same ACL check as cloud clients (traces to FR-8, unchanged)

A local process talking to Dispatch Core over its UDS (Unix domain
socket) interface goes through the exact same single ACL checkpoint
(P1-8) as a cloud-originated request. There is no local-socket
bypass.

**Beginner example:** the same security guard checks ID whether you
walked in the front door or the side door — there's no unguarded
back entrance.

### P1-13 — Housekeeping: `dispatcher_handlers.c` and `dispatcher_triage.c` exist and pass their configured checks (traces to B10)

The two files `.claude/settings.json`'s hooks already reference but
which don't exist in the repo yet are implemented as part of Phase 1,
so those hooks (a `cppcheck` pass and a schema validator) actually
have something to run against.

## 5. Non-functional requirements relevant to Phase 1

| ID | Requirement | Phase 1 status |
|---|---|---|
| NFR-1 (Portability) | Identical implementation across RDK-B/RDK-V | Applies in principle; the per-platform event namespace mechanism (A9) that makes this concrete is Phase 2 engineering work, not yet built |
| NFR-2 (Isolation) | A toolset crash shouldn't affect Dispatch Core or other toolsets | **Weakened by design under P1-6's exception** — an in-process toolset crashing can affect the process it shares. This is exactly the risk the exception trades away temporarily, named here rather than hidden |
| NFR-3 (Least privilege) | No toolset runs with more access than its manifest declares | Not enforceable yet for Phase 1's in-process toolset(s) — there's no sandbox to enforce it against until Phase 2 |
| NFR-4 (No confused deputy) | Adapters forward real caller identity, don't act under one fixed elevated identity | Not yet exercised in Phase 1 — no Platform Adapter forwarding is in this phase's scope |
| NFR-5 (Auditability) | Every ACL decision is logged | Applies from Phase 1 — the ACL Policy Store's audit logging requirement is unconditional, not phased |
| NFR-9 (Session resilience) | Stateless token validation, no server-side session table | Applies once SAT tokens are implemented (P1-14 below) |
| NFR-10 (Revocation) | Short expiry + refresh, or a revocation list | **Now a Phase 1 blocker** — A1 (SAT token format) must be confirmed and implemented; Phase 1's original read-only scope could defer this, but real command execution cannot |

### P1-14 — SAT token authenticated command execution (traces to A1, now urgent per A15)

Every real command (P1-4) and every `toolset.push` (P1-5) carries a
short-lived SAT token identifying the caller and their permitted
groups, validated by Dispatch Core before the ACL check (P1-8) even
runs. **This requirement's exact token format is not yet finalized —
see Section 7, Known Gaps.**

## 6. Explicitly out of scope for Phase 1

- Out-of-process execution and sandboxing (FR-13/FR-14) for any
  toolset **other than** the P1-6 exception, and even for that
  exception's own hardening — both stay Phase 2.
- Payload-level encryption (P1-7 already states this, restated here
  for clarity as an exclusion, not an oversight).
- RBUS/Thunder-to-TR-181 translation mapping (B1).
- Sandbox profile authoring workflow (B2).
- Footprint budget sizing against real hardware (B3).
- On-demand toolset spawn mechanism (A8) — not relevant while P1-6's
  toolset(s) stay in-process.
- RDK-V per-platform namespace mapping engineering (A9) — the
  decision is confirmed, the mapping table itself is not built.
- The independent NFR-3/NFR-4 security review (B4) — Phase 2's
  concluding gate, not attempted here.
- MCP's `tools/list`/`tools/call` surface as the primary framing for
  Phase 1 — Phase 1 can run on plain JSON-RPC 2.0 alone under A12's
  confirmed dual-path model; adopting the full MCP surface for Phase
  1's methods is not required.

## 7. Known gaps this document does not paper over

- **A1 (SAT token format)** is still drafted, unconfirmed. P1-14
  depends on it directly. This is the single most consequential open
  item for actually building Phase 1 as scoped here.
- **C8** (`toolset.push`'s `params.artifact` field contents) is still
  open. P1-5's example above deliberately omits an `artifact` field
  for this reason — do not assume a shape for it yet.
- **`docs/20`'s ACL design** (B5/B6) is a technical review and an
  illustrative `reference-impl/` sketch, not a built implementation.
  P1-8 depends on this actually being implemented, not just designed.
- `add-triage-skillset-mapping-phase1`'s own design document still has
  a pending correction (tracked in `define-toolset-as-mcp-tool-model/tasks.md`
  §4) reconciling `triage.capabilities` as a standalone method against
  the later MCP tool-projection decisions (A11/A13). Under A12's
  confirmed permanent dual-path model, keeping `triage.capabilities`
  as a plain JSON-RPC method (as shown in P1-3) is a legitimate,
  standing option — but this reconciliation has not been formally
  closed out.

## 8. Requirements traceability matrix

| Req ID | Summary | Traces to | Confirmation status |
|---|---|---|---|
| P1-1 | JSON-RPC 2.0 over WRP | FR-1 | Confirmed, original scope |
| P1-2 | `type` field + mandatory verification | A5, A7 | Confirmed |
| P1-3 | Read-only triage discovery | `add-triage-skillset-mapping-phase1` | Confirmed, pending doc reconciliation |
| P1-4 | Real command execution | `add-phase1-command-execution-exception` | Confirmed 2026-08-14 |
| P1-5 | `toolset.push` | `define-synchronous-toolset-push`, A15 | Confirmed 2026-08-14 |
| P1-6 | In-process exception | A15 | Confirmed, explicit and tracked |
| P1-7 | No encryption, verification mandatory | A7 | Confirmed unchanged |
| P1-8 | Single ACL checkpoint | FR-4 | Confirmed, unaffected by P1-6 |
| P1-9 | RDM boundary unchanged | A6 | Confirmed unchanged |
| P1-10 | Phase 1 rollback model | `add-phase1-command-execution-exception` §2 | Confirmed |
| P1-11 | Capability reporting | FR-7 | Confirmed, pre-existing |
| P1-12 | Local client parity | FR-8 | Confirmed, pre-existing |
| P1-13 | Housekeeping files | B10 | Confirmed, scheduled Phase 1 |
| P1-14 | SAT token auth | A1 | **Open — blocking** |
