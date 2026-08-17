# Diag-Server: Toolsets, Dynamic Plugin Support, and Request Formats

**Source:** `external/diag-server/diag-server-nn.c`, `external/diag-server/diag-triage-catalog.json`
**Verified against:** the current source, 2026-08-16.

---

## 1. The toolset model

Diag-Server organizes its diagnostic commands into a **catalog**, split into up to four independent files, one per "plane": `triage`, `management`, `control`, `config-apply`. Each plane's catalog is a separate JSON file loaded from a single directory at startup:

| Plane | Catalog file | Status in this repo |
|---|---|---|
| `triage` | `diag-triage-catalog.json` | **Present** — 12 tools (see §2) |
| `management` | `diag-management-catalog.json` | Not present — plane simply not served |
| `control` | `diag-control-catalog.json` | Not present — plane simply not served |
| `config-apply` | `diag-config-apply-catalog.json` | Not present — plane simply not served |

A plane with no file in the catalog directory is not an error — it's just unserved. Only `triage` ships a populated catalog today; `management`/`control`/`config-apply` are architecturally supported (the code has a fixed 4-entry plane table and will load any of the other three files if one is placed in the catalog directory) but have no tools defined in this repo yet.

Every tool entry declares a `"type"`: `"static"` (fixed, catalog-defined command) or `"dynamic"` (caller supplies the command). Both live in the same catalog file, side by side — "static toolset" and "dynamic plugin" are not separate files or separate protocols, just a per-tool flag.

---

## 2. Static toolset — the `triage` plane's 12 tools

These are the tools currently defined in `diag-triage-catalog.json`. Every one below except `adhoc_diagnostic` is `"type": "static"` — the catalog's own `command` always runs; a caller-supplied override is ignored (logged, not rejected) rather than executed.

| Tool name | Command | Timeout | Notes |
|---|---|---|---|
| `ping_google_ip` | `/bin/ping -c 3 8.8.8.8` | 10s | |
| `wan_status` | `/sbin/ifconfig erouter0` | 5s | |
| `dns_servers` | `cat /etc/resolv.conf` | 5s | |
| `parodus_health` | `cat /tmp/parconnhealth.txt` | 5s | |
| `device_uptime` | `cat /proc/uptime` | 5s | |
| `memory_usage` | `cat /proc/meminfo` | 5s | |
| `interface_config` | `/sbin/ifconfig` | 5s | |
| `routing_table` | `/sbin/route -n` | 5s | |
| `active_connections` | `/bin/netstat -n` | 5s | `suppress_stderr: true`, `count_lines_matching: "ESTABLISHED"` — stdout is the decimal count of established connections, not netstat's raw output |
| `process_status` | `/bin/ps` | 5s | |
| `kernel_log` | `/bin/dmesg` | 10s | |
| `adhoc_diagnostic` | *(none — dynamic)* | 15s | See §3 |

All 12 are validated once at process startup (blocklist + parseability check on each static tool's own `command`); a tool that fails is marked `_skipped` and refuses every request against it — including one carrying its own override — until the service is restarted with a corrected catalog.

To see this list live rather than reading the catalog file directly, send a `DESCRIBE` request (§5.3).

---

## 3. Dynamic plugin support

A catalog entry with `"type": "dynamic"` has no fixed `command` — the caller is expected to supply one in the request. `adhoc_diagnostic` is the one example currently in the catalog:

```json
"adhoc_diagnostic": {
  "timeout": 15,
  "type": "dynamic",
  "plane": "triage"
}
```

For a dynamic tool:
- The blocklist (`rm`, `reboot`, `dd`, `mount`, `passwd`, etc. — the full list is in §2.5 of the architecture doc) still applies unconditionally.
- There is no catalog-declared program to pin an override against, so `is_command_safe()` skips the program-pinning check that static tools get — this is the deliberate tradeoff of declaring a tool dynamic, not a gap.
- If the caller sends no `command` at all for a dynamic tool with no catalog default, nothing runs and the response reports `"command blocked or missing"`.

This is the mechanism to reach for when you want an ad hoc diagnostic that isn't worth a permanent catalog entry — one dynamic tool can serve any number of different commands across different requests, each one only checked against the blocklist at request time.

Adding a *new* dynamic (or static) tool permanently to the catalog is a catalog change, not a code change — see §5.4 (PUSH) for how to add one without restarting the service, or simply edit the plane's JSON file on disk and restart.

---

## 4. How to send a single tool request (EXEC)

EXEC is the default request kind — no `"kind"` field, or `"kind": "EXEC"`. It's a WRP type-3 message; the payload below is the *inner* msgpack map (shown here as JSON for readability — on the wire it's msgpack, not JSON text).

**Request:**
```json
{"tool": "device_uptime", "command": "", "plane": "triage"}
```
- `tool` — required. The name from the table in §2 (or any tool in another plane's catalog).
- `command` — optional. Only takes effect for a `"type": "dynamic"` tool; ignored (with a log line) for a static tool.
- `plane` — optional. If given, `tool` is looked up only in that plane's catalog. If omitted, every loaded plane is searched, and a name that exists in more than one plane is rejected as ambiguous.

**Example — running a dynamic tool with a caller-supplied command:**
```json
{"tool": "adhoc_diagnostic", "command": "cat /proc/loadavg", "plane": "triage"}
```

**Response:**
```json
{"tool": "device_uptime", "exit_code": 0, "stdout": "12345.67 8901.23\n"}
```

`exit_code`/`stdout` reflect the underlying command's own exit status and captured output (capped at 64 KiB, no shell involved — see the architecture doc for the full execution model). A denied, blocked, missing, or skipped tool comes back as a non-zero `exit_code` with an explanatory `stdout` string rather than a transport-level error.

---

## 5. How to send a toolset (multiple tools/commands at once)

There are two distinct things "sending a toolset with commands" can mean — reading the current catalog, or writing a new one:

### 5.1 Reading the current toolset — DESCRIBE

```json
{"kind": "DESCRIBE", "plane": "triage"}
```
Returns the live catalog's shape (not the commands themselves, just what's callable):
```json
{
  "plane": "triage",
  "version": 1,
  "tools": [
    {"name": "device_uptime", "type": "static", "plane": "triage", "timeout": 5},
    {"name": "adhoc_diagnostic", "type": "dynamic", "plane": "triage", "timeout": 15}
  ]
}
```
Omit `plane` to get this shape for every currently-loaded plane, as an array.

### 5.2 Writing a new toolset — PUSH

PUSH replaces or adds tools (each with its own `command`) in one plane's catalog, atomically and with the same safety validation every static command gets at startup. It's the mechanism for "sending a toolset with commands" in the literal sense — a diff of named tools, each carrying its own `command`.

```json
{
  "kind": "PUSH",
  "plane": "triage",
  "base_version": 1,
  "target_version": 2,
  "diff": {
    "added": {
      "check_disk_space": {"command": "df -h", "timeout": 5, "type": "static", "plane": "triage"},
      "check_dhcp_lease": {"command": "cat /var/lib/dhcp/dhclient.leases", "timeout": 5, "type": "static", "plane": "triage"}
    },
    "removed": [],
    "modified": {
      "device_uptime": {"command": "cat /proc/uptime", "timeout": 10, "type": "static", "plane": "triage"}
    }
  }
}
```
- `base_version` must equal the plane's **current, live** `_catalog_version` exactly — this is a compare-and-swap, not "newer than." A stale `base_version` is rejected outright with no partial effect.
- `added`/`modified` are both whole-tool upserts (each entry replaces any existing tool of the same name in full); `removed` is a plain list of tool names, applied first.
- Every tool the diff actually adds or modifies is re-validated (blocklist + parseability) before the push is accepted — a bad command in the diff rejects the entire push, leaving the live catalog untouched.
- On success, the new catalog is persisted to disk before the response is sent, so a crash right after can't leave memory and disk disagreeing.

**Response (success):**
```json
{"status": "loaded", "plane": "triage", "version": 2}
```
**Response (rejected):**
```json
{"status": "rejected", "plane": "triage", "reason": "base_version 1 does not match live version 3"}
```

**Important — current authorization state (see the architecture doc's §6 "Known open gaps" for the full picture):** as of 2026-08-16, PUSH is accepted on both the public (Parodus) and local-only transports, with **no ACL check on who is allowed to send one**. Anything that can reach the public pair can push a new toolset to any plane. Keep that in mind before using PUSH from anything other than a trusted internal tool.

---

## 6. Quick reference — message kinds

| Kind | Purpose | Mutates the catalog? |
|---|---|---|
| `EXEC` (default) | Run one tool, get its result | No |
| `DESCRIBE` | List the tools available in one or all planes | No |
| `HEALTH` | "Is the process alive" | No |
| `PUSH` | Add/replace/remove tools (a toolset) in one plane | Yes |
| `CHANGED` | Unsolicited — diag-server tells you a plane's catalog just changed | N/A (notification only) |

See `diag-server-documentation.md` (the companion architecture/user-guide document) for the full wire envelope, transport details, and safety-control writeup behind all of the above.
