# Device Side Diagram — Explained

## What Each Block Does

### ☁️ OPS Gateway (Cloud)
The remote cloud service that initiates diagnostic requests.
Sends WRP-formatted requests to devices asking them to run diagnostic tools (e.g., "check CPU usage", "get network stats", "read temperature").

### Parodus
Local messaging broker running on the device.
Acts as the intermediary between the cloud (OPS Gateway) and the device service (Diag-Server).
Receives requests from the cloud on one socket and forwards them to Diag-Server on another.

### Main Thread — Receive Loop
Always listening on the PULL socket for incoming messages from Parodus.
Stays responsive by delegating execution to worker threads so it never blocks.
Also handles keepalive heartbeat messages from the cloud inline.

### Decoder (WRP + Payload)
Unpacks two layers:
1. **WRP outer envelope** — extracts message type, source, destination, transaction UUID
2. **Inner payload** — extracts the tool name and optional command override

Validates message format before passing further down the pipeline.

### Catalog Manager (Tool Lookup)
Reads `catalog.json` — the file that lists all approved diagnostic tools and their default commands.
Verifies the requested tool exists before allowing execution.
Returns the default command if the request omits one.
Rejects any tool not listed in the catalog — acts as the first allowlist boundary.

### Safety Gate (Blocked-Token Check)
Security layer that prevents dangerous shell commands from executing.
Checks whether the command starts with any blocked token: `rm`, `reboot`, `shutdown`, `dd`, `mkfs`, `fdisk`, `iptables`, etc.
Returns an error response without executing anything if the command is blocked.

### Worker Thread — Execute Command
A detached thread spawned per request by the main thread.
Runs the actual shell command via `popen()` and captures stdout.
Output is captured up to a hard ceiling of **64KB** (`MAX_OUTPUT_BYTES`
in `diag-server-nn.c`) — actual captured output ranges from 0 bytes
(a silent command) up to that 64KB cap, where it's truncated.
*Corrected 2026-08-14 — this previously said 16KB, which didn't match
the code; see `docs/24_diag_server_merge_plan.md` §2.*
Execution has **no enforced time limit** in the current code —
`DEFAULT_TIMEOUT_SEC` is defined but never referenced anywhere else in
`diag-server-nn.c`, so a hung command blocks its worker thread
indefinitely. *Corrected 2026-08-14 — this previously said "10
seconds," which isn't actually enforced; matches the already-known
FR-013 gap (per-tool timeout is a target, not yet implemented).*
Records exit code alongside the captured output.

### Response Builder (WRP + Payload)
Packages the command result into a two-layer structure:
1. **Inner payload** (msgpack): `{ tool, exit_code, stdout }`
2. **Outer WRP envelope** (msgpack): `{ msg_type=3, source, dest, transaction_uuid, payload }`

Sends the assembled message back to Parodus via the PUSH socket.

---

## Flow Summary

```
Request → Parodus → Main Thread → Decoder → Catalog → Safety Gate → Worker → Response → Parodus → Cloud
```

---

## What is WRP (Web Routing Protocol)?

**WRP** stands for **Web Routing Protocol** — a message format used for communication between cloud services and devices in the RDK (Reference Design Kit) ecosystem used on cable/broadband CPE devices.

### Purpose
A standardized, binary way to route and deliver messages between the cloud and remote devices, with proper message envelope formatting and guaranteed routing metadata.

### Key Characteristics

| Property | Detail |
|----------|--------|
| **Encoding** | Binary msgpack (more compact than JSON, faster to parse) |
| **Structure** | Outer WRP envelope + inner payload (like HTTP headers + body) |
| **Routing** | Source, destination, and transaction UUID fields ensure correct delivery |
| **Types** | Different `msg_type` values serve different purposes |

### Message Types Used in Diag-Server

| msg_type | Name | Direction | Purpose |
|----------|------|-----------|---------|
| **3** | Request / Response | Bidirectional | Cloud sends diagnostic command; device returns result |
| **9** | Registration | Device → Cloud | Device announces itself as `diag-server` at startup |
| **10** | Keepalive | Bidirectional | Cloud pings device; device acknowledges it is alive |

### Message Structure

```
┌─────────────────────────────────────────────┐
│  WRP Outer Envelope (msgpack)               │
│  ├─ msg_type       : 3                      │
│  ├─ source         : "dns:diag-server"      │
│  ├─ dest           : "cloud/ops-gateway"    │
│  ├─ transaction_uuid: "abc-123-..."         │
│  ├─ content_type   : "application/msgpack"  │
│  └─ payload        : [binary]               │
│       ┌────────────────────────────────┐    │
│       │  Inner Payload (msgpack)       │    │
│       │  ├─ tool      : "cpu_info"     │    │
│       │  ├─ command   : "" (optional)  │    │  ← Request
│       │  ├─ exit_code : 0              │    │  ← Response
│       │  └─ stdout    : [binary data]  │    │  ← Response
│       └────────────────────────────────┘    │
└─────────────────────────────────────────────┘
```

### How WRP Flows Through Diag-Server

1. Cloud sends **WRP msg_type=3** to Parodus with payload `{"tool": "cpu_info", "command": ""}`
2. Parodus forwards to Diag-Server via nanomsg PULL socket
3. Diag-Server decodes the outer WRP envelope to extract routing metadata
4. Diag-Server decodes the inner payload to get tool name and command
5. Diag-Server executes the command and packages the result
6. Diag-Server builds a **WRP msg_type=3** response with payload `{"tool": "cpu_info", "exit_code": 0, "stdout": "..."}`
7. Diag-Server sends via nanomsg PUSH socket back to Parodus
8. Parodus returns the WRP response to the cloud OPS Gateway

### Why WRP?

- **Reliable delivery**: Transaction UUIDs link every request to its response
- **Standardized routing**: Works across all RDK device services, not just Diag-Server
- **Device-agnostic**: Any cloud service can talk to any device service using the same protocol
- **Compact**: msgpack encoding is 20–40% smaller than equivalent JSON

Think of WRP as **HTTP for embedded devices** — it provides the routing, delivery, and envelope contract that all device services communicate through.

### Socket Configuration in Diag-Server

| Socket | Type | Address | Role |
|--------|------|---------|------|
| PUSH | nanomsg | `127.0.0.1:6666` | Send responses and registration to Parodus |
| PULL | nanomsg | `127.0.0.1:6669` | Receive requests from Parodus |

---

## Parodus Configuration

### Connection URLs

```c
PARODUS_URL  =  "tcp://127.0.0.1:6666"   // Parodus broker — Diag-Server connects TO this (PUSH)
CLIENT_URL   =  "tcp://127.0.0.1:6669"   // Diag-Server's own address — Parodus pushes TO this (PULL)
```

Both are **localhost-only**. Parodus handles all cloud-facing TLS/WebSocket connections independently — Diag-Server never communicates with the cloud directly.

### Socket Tuning Parameters

| Parameter | Socket | Value | Purpose |
|-----------|--------|-------|---------|
| `NN_RCVTIMEO` | PULL | **2000 ms** | Receive timeout — prevents blocking forever on an idle socket |
| `NN_RCVBUF` | PULL | **1 MB** | Large buffer so keepalives are not dropped while a request is being processed |
| `NN_SNDTIMEO` | PUSH | **5000 ms** | Send timeout — prevents `nn_send` from stalling indefinitely if Parodus is busy |

### Service Registration

At startup, Diag-Server sends a **WRP msg_type=9** to Parodus identifying itself:

```json
{ "msg_type": 9, "service_name": "diag-server", "url": "tcp://127.0.0.1:6669" }
```

This tells Parodus where to route all incoming requests addressed to `diag-server`.

### Connection Retry / Backoff

If Parodus is not ready at startup, Diag-Server retries the PUSH socket connection using **exponential backoff**:

```
Attempt 1 → wait 1s
Attempt 2 → wait 2s
Attempt 3 → wait 4s
Attempt 4 → wait 8s
...
Capped at 60s per retry — retries indefinitely until Parodus is available
```

### Architecture Boundary

```
┌───────────────────────── Device ──────────────────────────┐
│                                                            │
│   Diag-Server ──PUSH──► Parodus :6666                     │
│                                                            │
│   Diag-Server ◄──PULL── Parodus :6669                     │
│                                                            │
│                          Parodus ◄──── Cloud TLS/WSS ────► OPS Gateway
│                          (cloud config is Parodus-owned)   │
└────────────────────────────────────────────────────────────┘
```

Diag-Server only knows two addresses (`127.0.0.1:6666` and `127.0.0.1:6669`). All cloud endpoint configuration (hostnames, ports, TLS certificates) lives in Parodus — completely decoupled from Diag-Server.
