# Diag-Server: Architecture, User Guide, and Input Format

**Source:** `external/diag-server/diag-server-nn.c` (single-file C service, ~2,700 lines)
**Verified against:** the current source, line by line, on 2026-08-16.

> A note on the repo's own docs: `external/diag-server/README.md` is actively maintained and matches the source. `ARCHITECTURE.md` and `DIAGRAM_EXPLAINED.md` in the same folder are **stale** — their diagrams predate the per-plane catalog split, the dual (public + local) transport, the ACL gate, and the DESCRIBE/HEALTH/PUSH/CHANGED protocol additions described below. This document reflects the code as it exists today.

---

## 1. What it is

Diag-Server is a small diagnostic-execution daemon for RDK CPE devices. It receives a request naming a diagnostic tool (e.g. "check WAN status", "dump routing table"), looks that tool up in a JSON catalog, runs the associated command, and returns the command's exit code and captured stdout. It was originally built as a single-file service talking to Parodus over raw nanomsg (no `libparodus`/`wrp-c` dependency); it has since grown a second, local-only transport, a per-plane catalog model, an ACL gate, and a small local control protocol (catalog push, health check, capability description) layered on the same wire format.

---

## 2. Architecture

### 2.1 Components

| Component | Responsibility |
|---|---|
| Main thread | Startup, catalog load, socket setup, poll loop, keepalive replies, shutdown |
| Worker threads | One detached `pthread` per inbound request (EXEC/DESCRIBE/HEALTH/PUSH) — keeps the poll loop responsive |
| Per-plane catalog store (`g_planes[4]`) | One JSON catalog per plane (`triage`, `management`, `control`, `config-apply`), each independently loadable, pushable, and versioned |
| Safety gate | Blocklist (`is_blocked`) + program-pinning (`is_command_safe`) + static-command init-time validation |
| Command executor (`run_command`) | `fork()` + `execvp()` with a tokenized argv — no shell anywhere in the path — bounded output, hard timeout |
| WRP/msgpack layer | Encodes/decodes the outer WRP envelope and inner payload maps |
| ACL gate (`diag_acl_check`) | Thin wrapper around `acl_policy_store_query()` gating every EXEC request (see §2.5 — not yet linkable) |
| Capability-sync notifier | Fires a `capability_sync.updated` JSON-RPC notification to the cloud after a successful catalog push |

### 2.2 Transport: two independent socket pairs

Diag-Server binds/connects **two** separate nanomsg pipeline pairs, serviced by the same poll loop and the same message-handling code:

| Pair | Direction | Address | Notes |
|---|---|---|---|
| Public (Parodus-facing) | PULL (bind) | `tcp://127.0.0.1:6669` | Receives from Parodus |
| Public (Parodus-facing) | PUSH (connect) | `tcp://127.0.0.1:6666` | Sends to Parodus, retried with exponential backoff at startup |
| Local-only | PULL (bind) | `ipc:///run/dispatcher/diagnostics-in.sock` | Best-effort; if the socket directory isn't provisioned, diag-server logs a warning and simply runs without it |
| Local-only | PUSH (connect) | `ipc:///run/dispatcher/diagnostics-out.sock` | Single connect attempt (not retried) — a PUSH socket queues in the background regardless |

**Registration is currently disabled.** `REGISTER_WITH_PARODUS` is compiled to `0`: diag-server no longer sends its WRP type-9 registration to Parodus, so Parodus has no route to the public `CLIENT_URL`. The public PULL/PUSH sockets are still bound/connected (outbound traffic like capability-sync is unaffected) — only the inbound registration announcement is suppressed. In practice this makes the local-only endpoint the only address anything can currently reach diag-server through. Reverting is a single `#define` flip.

**PUSH (catalog update) is transport-restricted.** A catalog-push request is only honored if it arrives on the local-only endpoint; one received via the public pair is rejected outright, since no ACL check currently protects that path. DESCRIBE/HEALTH/EXEC carry no such restriction — EXEC is instead ACL-gated per tool (§2.5).

### 2.3 Threading and locking

- The **main thread** owns the poll loop (`nn_poll` across both PULL sockets), catalog load/validation at startup, and inline keepalive (WRP type 10) replies.
- Every WRP type-3 request (EXEC/DESCRIBE/HEALTH/PUSH) is dispatched to a **detached worker thread**, so a long-running command never blocks the receive loop or the other socket.
- `g_catalog_mutex` protects only the *swap moment* of a catalog read: a worker locks it around `catalog_lookup()` and the field reads it needs off the resolved entry, and releases it **before** calling `run_command()` (which can block for the tool's full timeout). A catalog push takes the same mutex only for its own pointer/version swap. This keeps the critical section short without per-entry reference counting.
- Each plane also has its own `push_lock`, serializing concurrent pushes to *that one plane* independently of the others.

### 2.4 Data flow (EXEC request)

```
OPS Gateway --WRP type=3--> Parodus --(public pair, currently unreachable
                                        since registration is disabled)-->
                                       diag-server PULL
        or  Dispatch Core -------(local pair)-------------------------->  diag-server PULL

diag-server: decode outer WRP -> decode inner payload (tool/command/plane)
          -> ACL check (diag_acl_check)                      [deny -> exit_code 126]
          -> catalog_lookup(tool, plane)                     [miss  -> "tool not in catalog"]
          -> is this tool marked _skipped at init?           [yes   -> "tool skipped at init: <reason>"]
          -> static tool: always run catalog's own command (override ignored, logged)
          -> dynamic tool: run caller override if is_command_safe(), else catalog default
          -> run_command(): fork/execvp, bounded output, hard timeout
          -> build inner response {tool, exit_code, stdout}
          -> wrap in outer WRP, send back on whichever socket the request arrived on
```

### 2.5 Security and safety controls

1. **Allowlist by catalog.** A requested `tool` must exist in the resolved plane's (or, if no plane given, some unambiguous single) catalog.
2. **Blocklist by executable name.** The command's program token — matched both as given and by basename, so `/bin/rm` is caught the same as `rm` — is checked against: `rm, rmdir, reboot, shutdown, halt, poweroff, factory_reset, kill, killall, pkill, dd, mkfs, fdisk, mount, umount, iptables, passwd`.
3. **No shell.** Commands run via `execvp()` with an argument vector built by a custom tokenizer (whitespace-split, double-quoted segments as one token). Shell metacharacters (`| ; & $ \` > <` etc.) are inert literal text, not operators.
4. **Static vs. dynamic tools.** A catalog entry declares `"type": "static"` (default) or `"type": "dynamic"`.
   - **Static**: the catalog's own `command` always runs. A caller-supplied `command` override is discarded outright (logged at `LOG_INFO`, not an error) — this closes the "same program, different arguments" gap that program-pinning alone would leave open.
   - **Dynamic**: the caller is expected to supply the command; the blocklist still applies unconditionally, but there is no program to pin against.
5. **Init-time static-command validation.** Every tool's own catalog `command` is tokenized and blocklist-checked exactly once, at process startup, before either socket exists. A tool that fails is marked `_skipped` and rejected for **every** subsequent request naming it — including one carrying its own, independently-safe override — for the rest of the process's life. Each skip is logged individually; there is no catalog hot-reload, so a fix requires a restart.
6. **Output cap.** Captured stdout is bounded at 64 KiB; the child is killed if it keeps writing past the cap.
7. **Per-tool timeout.** Enforced as a hard wall-clock ceiling (`SIGKILL` on expiry) — from the catalog's `timeout` field, or a 30-second default. A timed-out command returns `exit_code: 124` with `stdout: "command timed out after Ns"`.
8. **ACL gate.** Every EXEC request calls `diag_acl_check()` → `acl_policy_store_query(caller, "diagnostics", tool)` before catalog lookup. A denial returns `{"tool", "exit_code": 126, "stdout": "access denied"}` without ever reaching the catalog. **Caveat:** `acl_policy_store_query()` has no implementation anywhere in this codebase yet — its transport is a still-open, project-wide dependency — so this code compiles but will not currently link into a runnable binary. Caller identity is also currently just the WRP `source` field with no group/permission data, pending a real token format.
9. **PUSH transport restriction.** As described in §2.2 — a catalog push is rejected unless it arrives on the local-only endpoint.

---

## 3. User guide

### 3.1 Prerequisites

- C11 compiler, CMake ≥ 3.10, pthread
- `nanomsg`, `msgpack-c`, `cJSON`
(`wrp-c`, `libparodus`, and `cimplog` are **not** dependencies — diag-server talks to Parodus over raw nanomsg directly.)

### 3.2 Build and install

```bash
cmake -S . -B build
cmake --build build
cmake --install build     # installs to ${CMAKE_INSTALL_PREFIX}/bin/diag-server
```

### 3.3 Prepare the catalog directory

As of the per-plane catalog model, the startup argument is a **directory**, not a single file. Place whichever plane files are relevant at:

- `/etc/diag-server/diag-triage-catalog.json`
- `/etc/diag-server/diag-management-catalog.json`
- `/etc/diag-server/diag-control-catalog.json`
- `/etc/diag-server/diag-config-apply-catalog.json`

A plane with no file present is simply not served — that's normal, not an error. Or pass a custom directory:

```bash
./diag-server /path/to/catalog/dir
```

### 3.4 Running it

Diag-Server registers signal handlers for `SIGTERM`/`SIGINT`, loads catalogs, validates every static command once, binds its sockets, and enters the poll loop. Logging goes to syslog under ident `diag-server` — startup, registration status, per-request UUID/source, executed command, exit code, and connection retry errors are all logged there.

### 3.5 Troubleshooting

| Symptom | Likely cause / fix |
|---|---|
| No responses at all | Verify Parodus is listening on `127.0.0.1:6666` and diag-server bound `127.0.0.1:6669`; check the startup log for registration status (registration to Parodus is currently disabled by default — see §2.2 — so the local endpoint may be the only reachable path). |
| `"tool not in catalog"` | The `tool` name isn't in the resolved plane's catalog, or (if no `plane` was given) the name is ambiguous across more than one loaded plane. |
| `"command blocked or missing"` | The resolved command hit the blocklist, or (dynamic tool) failed the program-pin/blocklist check in `is_command_safe()`. Check syslog for `unsafe command rejected for tool '<tool>'`. |
| `exit_code: 126`, `stdout: "access denied"` | ACL denial from `diag_acl_check()`. |
| `exit_code: 124`, `stdout: "command timed out after Ns"` | The command exceeded its catalog `timeout` (or the 30s default) and was killed. Raise the catalog `timeout` if the command is legitimately slow. |
| Output truncated | Stdout capture is capped at 64 KiB by design. |
| A tool that used to work now fails, or a pipe/redirect in `command` does nothing | Execution no longer goes through a shell — `|`, `>`, `2>`, `;`, `&&`, backticks, `$()` are literal text, not operators. Use `suppress_stderr: true` instead of `2>/dev/null`, and `count_lines_matching: "X"` instead of `\| grep X \| wc -l`. |
| `exit_code: 1`, `stdout` starts with `"tool skipped at init:"`, for every request against that tool | The tool's own catalog `command` failed startup validation (blocklisted or unparseable). Check syslog at service start, fix the catalog entry, and **restart** — there is no hot-reload. |

---

## 4. Expected input format (wire protocol)

### 4.1 Envelope

Every message is a WRP map, msgpack-encoded, sent over one of the nanomsg pairs above.

- `msg_type = 9`: service registration (diag-server → Parodus at startup, currently disabled — see §2.2)
- `msg_type = 3`: request/response, used for every EXEC/DESCRIBE/HEALTH/PUSH/CHANGED exchange
- `msg_type = 10`: keepalive ping/ack, handled inline by the main thread (no worker thread spawned)

A type-3 message's `payload` field is itself a msgpack map (the "inner payload"), whose shape depends on which of five kinds it carries.

### 4.2 EXEC (default — no `"kind"` field, or `"kind": "EXEC"`)

**Request:**
```json
{"tool": "device_uptime", "command": "", "plane": ""}
```
- `tool` — required.
- `command` — optional override. Honored only for a `"type": "dynamic"` catalog tool (and only if it passes the blocklist + program-pin check); ignored for a `"type": "static"` tool, which always runs its own catalog command.
- `plane` — optional. If given, `tool` is looked up **only** in that plane's catalog (a miss there is a miss, no fallback search). If omitted, every loaded plane is searched; a name present in more than one plane is rejected as ambiguous rather than guessed at.

**Response:**
```json
{"tool": "device_uptime", "exit_code": 0, "stdout": "<binary stdout bytes>"}
```

### 4.3 DESCRIBE

**Request:** `{"kind": "DESCRIBE", "plane": ""}` — `plane` optional.

**Response:** one plane's `{plane, version, tools: [{name, type, plane, timeout}, ...]}`, or — if no `plane` was given — an array of that same shape, one entry per currently-loaded plane.

### 4.4 HEALTH

**Request:** `{"kind": "HEALTH"}`

**Response:** `{"status": "ok"}` — side-effect-free, does not touch the catalog or its mutex.

### 4.5 PUSH

**Request:**
```json
{
  "kind": "PUSH",
  "plane": "triage",
  "base_version": 1,
  "target_version": 2,
  "diff": {"added": {}, "removed": [], "modified": {}}
}
```
- `base_version` must equal the target plane's **current, live** version exactly (compare-and-swap, not "newer than") or the push is rejected before anything else is attempted.
- `diff.removed` is applied first (name list), then `added`/`modified` (both upsert semantics — whole-tool replacement) — so a name in both `removed` and `added` in the same diff ends up present, not dropped.
- Only tools the diff actually touches are re-validated; a pre-existing skip on an untouched tool doesn't block the push.
- On success, the promoted catalog is persisted to disk (fsync'd temp file + atomic rename) **before** the response is sent — a persistence failure rejects the whole push, so memory and disk never diverge.
- **Only accepted via the local-only endpoint** — rejected outright if received on the public (Parodus-facing) pair.

**Response:**
```json
{"status": "loaded", "plane": "triage", "version": 2}
```
or
```json
{"status": "rejected", "plane": "triage", "reason": "base_version 1 does not match live version 3"}
```

### 4.6 CHANGED (unsolicited)

Sent by diag-server itself immediately after a successful PUSH promote, on the same socket the PUSH arrived on:
```json
{"kind": "CHANGED", "plane": "triage", "version": 2}
```
Also fires an outbound `capability_sync.updated` JSON-RPC 2.0 notification over the public (Parodus) connection at the same point:
```json
{
  "jsonrpc": "2.0",
  "method": "capability_sync.updated",
  "params": {
    "toolset": "diagnostics",
    "version": "2",
    "capabilities": [{"name": "device_uptime", "type": "static", "plane": "triage", "timeout": 5}, "..."]
  }
}
```

### 4.7 Catalog file format

```json
{
  "_catalog_version": 1,
  "tools": {
    "device_uptime": {
      "command": "cat /proc/uptime",
      "timeout": 5,
      "type": "static",
      "plane": "triage"
    },
    "active_connections": {
      "command": "/bin/netstat -n",
      "suppress_stderr": true,
      "count_lines_matching": "ESTABLISHED",
      "timeout": 5,
      "type": "static",
      "plane": "triage"
    },
    "adhoc_diagnostic": {
      "timeout": 15,
      "type": "dynamic",
      "plane": "triage"
    }
  }
}
```

| Field | Type | Meaning |
|---|---|---|
| `command` | string | Program + arguments, whitespace-tokenized (double-quoted segment = one token). No shell interpretation. Optional for a `dynamic` tool. |
| `timeout` | number (seconds) | Per-tool wall-clock execution ceiling. Falls back to 30s if absent/non-positive. |
| `type` | `"static"` \| `"dynamic"` | Static tools always run their own `command`; dynamic tools expect the caller to supply one. Defaults to `static`. |
| `plane` | `"config-apply"` \| `"management"` \| `"control"` \| `"triage"` | Categorization metadata only — diag-server logs it but does not branch on it. |
| `suppress_stderr` | boolean | Redirects the command's stderr to `/dev/null` (replaces a shell `2>/dev/null`). |
| `count_lines_matching` | string | If set, stdout becomes the decimal count of the command's own output lines containing this substring, followed by a newline (replaces a shell `\| grep X \| wc -l`). |
| `_catalog_version` | integer | Monotonic version stamp; PUSH's `base_version` is an exact-match compare-and-swap against this, not a newer-than comparison. |

---

## 5. Key constants

| Constant | Value |
|---|---|
| Public Parodus URL | `tcp://127.0.0.1:6666` |
| Public client bind URL | `tcp://127.0.0.1:6669` |
| Local recv endpoint | `ipc:///run/dispatcher/diagnostics-in.sock` |
| Local send endpoint | `ipc:///run/dispatcher/diagnostics-out.sock` |
| Registration to Parodus | disabled (`REGISTER_WITH_PARODUS = 0`) |
| Default catalog directory | `/etc/diag-server` |
| Receive timeout | 2000 ms |
| Send timeout | 5000 ms |
| Max captured stdout | 65536 bytes |
| Default per-tool timeout | 30 s |

---

## 6. Known open gaps

- **ACL enforcement is wired but not linkable.** `acl_policy_store_query()` has no implementation anywhere yet; its transport is an unresolved, project-wide dependency shared with the RDK Dispatcher project's own toolset ACL model.
- **Caller identity is minimal.** `diag_acl_check()` currently populates `caller_identity_t` from just the WRP `source` field, with no groups — real identity/token format is still open.
- **No unit tests for msgpack encode/decode**, and no integration harness with a mock Parodus endpoint (both listed as improvement areas in the repo's own README).
