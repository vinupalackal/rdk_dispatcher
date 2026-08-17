# Diag-Server Software Requirements Specification (SRS)

Version: 1.0
Date: 2026-07-30
Project: Diag-Server
Document Type: Requirements Baseline (As-Built + Target)

> **Corrections applied 2026-08-14** — see
> `docs/24_diag_server_merge_plan.md` §2 in the RDK Dispatcher project.
> NFR-20's status and Risk item 1 below described a build/source
> filename mismatch that does not actually exist in this baseline's
> `CMakeLists.txt` — corrected. Section 6.2's dependency list and
> Risk item 1 also referenced `wrp-c`/`libparodus` build integration
> that the code doesn't use — corrected to match what
> `diag-server-nn.c` actually links against.
>
> **FR-27 implemented 2026-08-14** — per-tool timeout is now enforced
> (30s default, catalog-overridable). See §8.2 and Risk item 2 below.
>
> **NFR-17 implemented 2026-08-14** — command execution no longer goes
> through a shell at all (`/bin/sh -c` removed, not just `popen()`).
> See §9.3 and Risk item 3 below.
>
> **Risk item 5 narrowed further, 2026-08-14** — a single common
> `is_command_safe()` check now pins any caller-supplied `command`
> override's program to the catalog's own declared program for that
> tool, on top of the existing blocklist. See Risk item 5 below for
> what this closes and what it deliberately still leaves open.
>
> **FR-25c and NFR-13a added 2026-08-14** — per-tool static command
> safety validation moved from per-request to process initialization,
> per an explicit design request. A tool whose static command fails
> validation is marked skipped and rejected for the process's entire
> lifetime, including against a request carrying its own override. See
> §8.2 and Risk item 5 below.
>
> **Renumbered 2026-08-15** — per `docs/24_diag_server_merge_plan.md`
> §9 Q5, by direct instruction: this document's requirements now
> continue the RDK Dispatcher project's own master `FR-1`…`FR-14`/
> `NFR-1`…`NFR-10` numbering instead of keeping an independent
> `FR-001`…`FR-018`/`NFR-001`…`NFR-010` sequence, to avoid the two
> schemes colliding in conversation and documentation. Every ID in this
> document from here on is `FR-15` onward / `NFR-11` onward; relative
> order and lettered sub-items (e.g. `FR-011a` → `FR-25a`) are
> unchanged, only the numbers shifted. `FR-4`, where cited in this
> document, remains a reference to the *master* project's FR-4 (the
> single ACL checkpoint) and was not renumbered — it was never a local
> ID.

## 1. Purpose

This document defines the requirements for Diag-Server, a diagnostic execution service used on RDK CPE devices. It captures:
- Functional behavior the service shall provide.
- Non-functional expectations for reliability, performance, and observability.
- External interfaces and constraints.
- Verification criteria for each requirement.

## 2. Scope

Diag-Server shall:
- Register itself with Parodus.
- Receive WRP diagnostic requests.
- Resolve and execute approved diagnostic commands.
- Return command output and exit status in WRP responses.
- Maintain responsiveness while handling multiple requests.

Out of scope:
- Cloud-side orchestration and policy management.
- UI/portal functions.
- Device provisioning and firmware updates.

## 3. System Context

Diag-Server runs on the device and communicates locally with Parodus.

High-level interaction:
1. OPS Gateway sends request to Parodus.
2. Parodus forwards WRP request to Diag-Server.
3. Diag-Server executes command and sends WRP response to Parodus.
4. Parodus returns result to OPS Gateway.

## 4. Definitions

- WRP: Web Routing Protocol message format.
- Parodus: Local messaging service on the device.
- Catalog: JSON file containing allowed tools and default commands.
- Tool: Named diagnostic action mapped to a command.

## 5. Stakeholders

- Device Diagnostics Team
- Operations/Support Team
- Security and Compliance Team
- Platform Integrators (RDK/Parodus integration)

## 6. Assumptions and Dependencies

### 6.1 Assumptions

- Parodus is available on localhost.
- Device has required command binaries for catalog tools.
- Service has permission to execute read-only diagnostics.

### 6.2 Dependencies

- C runtime with pthread support.
- nanomsg library (direct raw-socket integration with Parodus — see
  `diag-server-nn.c`'s own header comment: "no libparodus dependency").
- msgpack-c library.
- cJSON library.

## 7. External Interface Requirements

### 7.1 Network Endpoints

- The service shall connect PUSH to `tcp://127.0.0.1:6666`.
- The service shall bind PULL at `tcp://127.0.0.1:6669`.

### 7.2 WRP Message Types

- Registration message type: 9.
- Diagnostic request/response message type: 3.
- Keepalive message type: 10.

### 7.3 Catalog File Interface

- Default catalog path: `/etc/diag-server/catalog.json`.
- Startup argument 1 may override the catalog path.

Catalog schema requirement:
- Top-level object shall include `tools` map.
- Each tool entry should include:
  - `command` as string. Corrected 2026-08-14 (NFR-17): this string is
    now tokenized directly into an argument vector for `execvp()`, not
    passed to a shell — it must not rely on shell metacharacters
    (pipes, redirects, `;`, `&&`, backticks, `$()`, wildcards). A
    double-quoted segment is treated as a single token, so an argument
    containing spaces can be written `"like this"`.
  - `timeout` as integer seconds (implemented; see FR-27).
  - `suppress_stderr` as boolean, optional. Added 2026-08-14 (NFR-17).
    Redirects the executed command's stderr to `/dev/null` — the
    argv-based replacement for a shell command string's former inline
    `2>/dev/null`.
  - `count_lines_matching` as string, optional. Added 2026-08-14
    (NFR-17). If present, the response's `stdout` is replaced with the
    decimal count of the command's own output lines containing this
    substring, followed by a newline — the argv-based replacement for a
    shell command string's former inline `| grep <substring> | wc -l`.
  - `type` as string, optional, one of `"static"` (default) or
    `"dynamic"`. Added 2026-08-14; see FR-25b.
  - `plane` as string, optional, one of `config-apply`, `management`,
    `control`, `triage`. Added 2026-08-14; categorization metadata for
    the wider RDK Dispatcher project's toolset-manifest conversion, not
    consumed by diag-server's own execution logic.

## 8. Functional Requirements

### 8.1 Startup and Lifecycle

FR-15: The service shall start, initialize logging, and install signal handlers for graceful termination.
Acceptance:
- On startup, service logs initialization message.
- On SIGINT or SIGTERM, service exits loop and releases resources.

FR-16: The service shall load the catalog from configured path during startup.
Acceptance:
- Valid JSON file is parsed into in-memory catalog.
- Invalid or missing file is logged with warning/error condition.

FR-17: The service shall create and configure local transport sockets.
Acceptance:
- PULL socket binds to client URL.
- PUSH socket connects to Parodus URL.
- Receive and send timeouts are configured.

FR-18: The service shall retry PUSH connection to Parodus using backoff.
Acceptance:
- Connection failures produce retry logs.
- Retry delay increases up to a bounded maximum.

FR-19: The service shall send a WRP registration message after successful connect.
Acceptance:
- Registration contains `msg_type`, `service_name`, and `url`.

### 8.2 Request Handling

FR-20: The service shall continuously receive WRP messages while running.
Acceptance:
- Receive loop remains active until shutdown signal.

FR-21: The service shall process diagnostic requests where `msg_type = 3`.
Acceptance:
- Type 3 request is decoded and forwarded to execution path.

FR-22: The service shall decode payload fields `tool` and optional `command` from request payload.
Acceptance:
- Tool name is extracted when present.
- Command override is extracted when present.

FR-23: The service shall validate requested tool against catalog entries.
Acceptance:
- Unknown tool results in error output in response path.

FR-24: If request command is missing or empty, the service shall use catalog default command for the tool.
Acceptance:
- Effective command equals catalog command when override is absent.

FR-25: The service shall block execution of disallowed commands based on blocked first-token list.
Acceptance:
- Blocked command returns non-success result without execution.
- Block event is logged.

FR-25a (added 2026-08-14, docs/24_diag_server_merge_plan.md §2/§6,
Risk item 5): When a caller-supplied `command` override is present, the
service shall additionally verify that the override's program
(`argv[0]`) matches the catalog entry's own declared program for that
tool, through a single common check (`is_command_safe()`) shared with
the blocklist check in FR-25.
Acceptance:
- An override naming the same program as the catalog entry, with
  different arguments, executes normally.
- An override naming a different, non-blocklisted program is rejected
  without execution, and the rejection is logged — verified via a
  standalone harness: `{"tool": "device_uptime", "command": "ls -la
  /root"}` (catalog program `cat`) is rejected even though `ls` is not
  on the blocklist.
- A tool with no catalog-declared `command` at all is exempt from this
  check (nothing to pin against) and falls back to FR-25's blocklist
  check alone — a pre-existing, documented edge case, not a new gap.
- Scope note: this does not restrict an override's *arguments* when
  the program matches — see REQUIREMENTS.md §12 Risk item 5 for the
  explicit, still-open residual case this leaves.

FR-25b (added 2026-08-14): A catalog tool entry shall be able to
declare `"type": "dynamic"` to opt out of FR-25a's program-pin check,
for tools intentionally designed to run a caller-supplied command
rather than a fixed catalog one. The blocklist check (FR-25) shall
still apply unconditionally to a dynamic tool's resolved command; only
the program-pin is type-gated. An entry with no `"type"` field, or any
value other than `"dynamic"`, is treated as `"static"` — the same
behavior every tool had before this field existed.
Acceptance:
- A static tool (default) behaves exactly as FR-25a describes.
- A dynamic tool with no catalog `command` accepts a caller-supplied
  command naming any non-blocklisted program — verified via a
  standalone harness: `is_command_safe(NULL, "ls -la /var/log", 1)`
  returns safe, while `is_command_safe(NULL, "reboot", 1)` is still
  rejected.
- Flipping only the `dynamic_type` argument between two otherwise
  identical calls changes the outcome for a different-program case —
  verified directly, confirming the type flag (not some other
  difference) gates the pin check.

FR-25c (added 2026-08-14, explicit design request: "call the static
command safety during the process initialization itself to avoid the
delay in runtime"): The service shall validate every catalog tool's
own static `command` exactly once, at process startup, immediately
after the catalog loads and before any transport socket is created —
rather than re-validating it on every request that resolves to it.
Validation shall check (a) that the command tokenizes to at least one
argument, and (b) that it is not blocklisted (FR-25). A tool that
fails either check shall be marked with a persistent `"_skipped"`
status for the remainder of the process's lifetime and its program
name shall not be cached. A tool that passes shall have its program
name (`argv[0]`) cached on its catalog entry as `"_program_token"`,
for FR-25a's program-pin check to reuse at request time without
re-tokenizing the catalog's command string.
A tool marked `"_skipped"` shall be rejected for *every* subsequent
request naming it — including one that supplies its own,
independently-safe `command` override — without executing anything and
without repeating the blocklist/tokenize work already done at startup.
This is a deliberate, whole-tool exclusion, not a narrower rule that
blocks only the no-override path.
Acceptance:
- At startup, every tool with a declared `command` is checked exactly
  once; the result (skipped or not, and the cached program token) does
  not change again for the life of the process, since there is no
  catalog hot-reload path anywhere in this codebase.
- A request against a tool with no override, that was not marked
  skipped, executes without any additional blocklist or tokenization
  work being performed on the catalog's own command string at request
  time.
- A request against a tool marked skipped is rejected — verified via a
  harness that injects a deliberately blocklisted tool entry, confirms
  it is marked skipped with the correct reason at (simulated) startup,
  and confirms a request against it supplying an *independently safe*
  override command (`cat /proc/uptime`) is *still* rejected, proving
  the skip is not bypassable via override.
- A tool with an unparseable (e.g. whitespace-only) static command is
  also marked skipped, with a distinct reason — verified via the same
  harness.
- Every skip is logged individually at startup (tool name, offending
  command, reason), plus a one-line summary of tools checked vs.
  skipped.

FR-26: The service shall execute allowed commands and capture stdout up to configured maximum bytes.
Acceptance:
- Exit code is captured.
- Stdout is returned, truncated at max size if needed.

FR-27: The service shall enforce per-tool timeout from catalog.
Current status (corrected 2026-08-14, updated same day when NFR-17
landed): Implemented. `run_command()` executes via `fork()`, giving it
a real child PID it can `kill(pid, SIGKILL)` on deadline expiry. The
effective timeout is the catalog entry's own `timeout` field when
present and positive; otherwise it falls back to `DEFAULT_TIMEOUT_SEC`
(30s, previously defined but unused). A command still running when the
deadline passes is killed and reaped, and the response carries exit
code 124 (matching GNU coreutils' `timeout(1)` convention) with output
`"command timed out after Ns"`. The same kill-and-reap path also
bounds a related latent hang: previously, a command that exceeded
`MAX_OUTPUT_BYTES` while `diag-server` stopped reading could block
forever in `pclose()` writing to a full, undrained pipe; that path is
now also killed rather than left to hang. **Note on the child's exec
mechanism**: at introduction, this fix's child called
`execl("/bin/sh","sh","-c",cmd,NULL)` — shell interpretation was still
in use at that point, a distinct, later concern tracked under NFR-17.
NFR-17 was closed the same day, replacing that with
`tokenize_argv()`+`execvp()` (see NFR-17 above) — the child no longer
goes through a shell at all. The timeout/kill/reap mechanics described
here are unaffected by that follow-up change.
Acceptance:
- Command exceeding timeout is terminated. Verified via harness test:
  a `sleep 30` command under a 2s timeout is killed within ~2s
  (not 30s), returns exit code 124, and leaves no orphan/zombie
  process behind.
- Response includes timeout-specific failure output and non-zero exit
  code. Verified: output is `"command timed out after Ns"`, exit code
  124.

FR-28: The service shall build and send WRP response for processed requests.
Acceptance:
- Outer response contains `msg_type`, `source`, `dest`, `transaction_uuid`, `content_type`, `payload`.
- Inner payload contains `tool`, `exit_code`, `stdout`.

FR-29: The service shall handle keepalive messages (`msg_type = 10`) and return keepalive acknowledgment.
Acceptance:
- Incoming keepalive triggers outgoing keepalive message.

### 8.3 Concurrency and Resource Management

FR-30: The service shall process type 3 requests in detached worker threads.
Acceptance:
- Main receive loop remains responsive during long-running command execution.

FR-31: The service shall release dynamically allocated resources after request completion.
Acceptance:
- Request-specific memory is freed on success and failure paths.

FR-32: The service shall close sockets and release catalog memory at shutdown.
Acceptance:
- No open transport socket remains after process exit.

## 9. Non-Functional Requirements

### 9.1 Reliability

NFR-11: The service should recover from temporary Parodus unavailability through retry logic.
NFR-12: Receive loop should tolerate timeout conditions without process crash.

### 9.2 Performance

NFR-13: The service should remain responsive to incoming messages under concurrent request load.
NFR-13a (added 2026-08-14, see FR-25c): Static command safety
validation shall be performed once per tool, at process
initialization, not repeated on every request. A request that resolves
to a non-skipped tool's catalog default shall incur zero additional
blocklist or tokenization cost on the catalog side beyond what was
already done at startup; only a caller-supplied override's own content
shall be validated live, since it is the only input that is genuinely
new per request.
NFR-14: Keepalive handling should not be blocked by a single slow diagnostic command.

### 9.3 Security

NFR-15: Only catalog-declared tools shall be executable.
NFR-16: Blocklisted commands shall never be executed.
NFR-17: Command execution model should transition from shell-based execution to safer exec-style invocation.
Current status (corrected 2026-08-14): Implemented. `run_command()`
now executes via `execvp()` with a tokenized argument vector
(`tokenize_argv()`); no shell (`/bin/sh -c` or otherwise) is invoked
anywhere in the command-execution path. This is a distinct, later fix
from FR-27's `fork()`+`pipe()` change (which still routed through
`/bin/sh -c` to obtain a killable PID) — this fix removes that shell
layer entirely, replacing it with direct `execvp(argv[0], argv)`.
Because there is no shell, characters like `;`, `&&`, backticks,
`$()`, and `|` in any `command` string (catalog-declared or the
caller-supplied override in Risk item 5) are no longer operators —
they are inert literal bytes passed as plain arguments to `argv[0]`.
A compound string such as `cat /proc/uptime; rm -rf /tmp/x` now
tokenizes to five literal arguments handed to `cat`, which fails
(nonzero exit, no output) rather than running `rm` as a second
command. Verified via a standalone harness exercising the extracted
functions directly: this exact compound string was run and confirmed
to leave its intended target file untouched, proving no second
command executes; a `|`-containing string was run through `echo` and
confirmed to print the pipe character literally rather than piping
through `tr`; the tokenizer was checked against a normal multi-arg
command; and the two new catalog fields (`suppress_stderr`,
`count_lines_matching`, see §7.3) were verified to reproduce the
`active_connections` tool's exact former output shape without a
shell pipeline.
Acceptance:
- Command exceeding timeout is terminated — unaffected by this change,
  already covered under FR-27.
- No shell metacharacter in any command string reaches an interpreter,
  because no interpreter is invoked. Verified per the harness runs
  above.
- Existing catalog tools that relied on shell features
  (`active_connections`'s pipe + stderr redirect) still produce
  identical output, via the new `suppress_stderr`/
  `count_lines_matching` catalog fields rather than shell syntax.
Scope note: this closes NFR-17 for the execution *mechanism*. It does
not by itself resolve Risk item 5 (whether the caller-supplied
`command` override should be removed or restricted) — that remains a
separate, still-open decision (see `docs/24_diag_server_merge_plan.md`
§9 open question 3). It substantially reduces that risk's blast
radius (shell metacharacters can no longer be weaponized through it)
without closing the finding itself.

### 9.4 Observability

NFR-18: Key lifecycle and request events shall be logged through syslog.
NFR-19: Error events for socket operations, catalog issues, and command failures shall be logged.
NFR-19a (added 2026-08-14): The per-request execution log entry
(NFR-18) shall include the resolved `type` (`static`/`dynamic`) and
`plane` of the tool being run, so a static-vs-dynamic or plane
categorization question about a specific run can be answered from
syslog alone without cross-referencing the catalog file. `plane` logs
as `unset` when the catalog entry doesn't declare one. This is
observability only — diag-server's execution behavior does not depend
on `plane`.

### 9.5 Portability and Build

NFR-20: Build system shall compile the actual service source file in the repository.
Current status (corrected 2026-08-14): Met. `CMakeLists.txt` already
compiled `diag-server-nn.c`; the previously-recorded "source filename
mismatch" did not exist in this baseline and has been removed from
this document. Separately, `CMakeLists.txt` linked `wrp-c`,
`libparodus`, and `cimplog`, none of which the code calls into — those
unused dependencies were removed from the build configuration.

## 10. Constraints

- Service language: C (C11 baseline).
- Message encoding: msgpack.
- Transport mechanism: nanomsg PUSH/PULL.
- Runtime environment: RDK device context with Parodus available locally.

## 11. Verification Matrix

| Requirement | Verification Method | Evidence |
|---|---|---|
| FR-15, FR-32 | Run and graceful shutdown test | Syslog entries + process exit status |
| FR-16 | Valid/invalid catalog startup tests | Syslog parse results |
| FR-17, FR-18, FR-19 | Integration startup test with Parodus endpoint | Socket and registration logs |
| FR-20, FR-21, FR-22 | WRP request decode test vectors | Response validity + logs |
| FR-23, FR-24, FR-25 | Tool resolution and blocked command tests | Expected error/success responses |
| FR-25a | Program-pinning override tests | Standalone harness (2026-08-14): same-program-different-args allowed, different-program rejected, no-catalog-command edge case |
| FR-25b | Static/dynamic type-gating tests | Standalone harness (2026-08-14): dynamic tool with no catalog command allows a non-blocklisted caller program, blocklist still enforced for dynamic, flipping `dynamic_type` alone flips the outcome for an otherwise-identical call |
| FR-25c, NFR-13a | Init-time validation + skip-blocks-override tests | Standalone harness (2026-08-14), run against the real `diag-triage-catalog.json` via a minimal cJSON-compatible parser: all 11 real tools validate clean (0 false-positive skips); `device_uptime` gets a cached `_program_token` of `cat`; `adhoc_diagnostic` (no catalog command) gets none; a deliberately injected blocklisted tool and a deliberately injected unparseable (whitespace-only) tool are both correctly marked skipped with distinct reasons; a skipped tool rejects a request even when that request supplies an independently-safe override (`cat /proc/uptime`) — the critical bypass-prevention property |
| FR-26 | Command execution output size test | Truncation behavior + exit code |
| FR-27 | Timeout enforcement test | Timeout termination evidence |
| FR-28 | Response schema validation | Message inspection |
| FR-29 | Keepalive ping/ack test | Ack message observed |
| FR-30 | Concurrent request load test | Main-loop responsiveness metrics |
| FR-31 | Memory/resource checks | Leak scan results |
| NFR-18, NFR-19, NFR-19a | Log coverage test | Log event completeness report; NFR-19a additionally checks exec log lines carry `type=`/`plane=` |
| NFR-20 | Clean build test | Successful build artifact |

## 12. Risks and Open Items

1. ~~Build configuration currently references a different source
   filename than present in repository.~~ Corrected 2026-08-14 — this
   did not actually exist in `CMakeLists.txt`; see NFR-20.
2. ~~Catalog timeout is defined but not enforced in code baseline.~~
   Fixed 2026-08-14 — see FR-27 above. 30s default, catalog-overridable,
   enforced via `fork()`+`SIGKILL` on deadline.
3. ~~Shell-based command execution increases injection and
   command-surface risk.~~ Fixed 2026-08-14 — see NFR-17 above.
   `run_command()` no longer invokes a shell at all (`/bin/sh -c` was
   removed, not just `popen()`); execution is `execvp()` with a
   tokenized argument vector.
4. No formal automated test suite is included in current baseline.
5. ~~A caller-supplied `command` field in the request payload
   overrides the catalog's default command and is validated only by a
   first-token blocklist check~~ — added 2026-08-14, see
   `docs/24_diag_server_merge_plan.md` §2 in the RDK Dispatcher
   project. **Narrowed in two stages, both 2026-08-14:**
   1. The NFR-17 shell-removal fix (item 3 above) closed the
      compound-command technique described here (`;`, `&&`, a pipe) —
      those characters are now inert literal argument text passed to
      the first token's program, which typically just fails.
   2. `is_command_safe()` — a single, common check both the catalog's
      default command and any caller override now go through in
      `handle_request()` — additionally pins the override's program
      (`argv[0]`) to the catalog's own declared program for that tool.
      An override can still adjust arguments (e.g. a different ping
      target), but can no longer redirect a tool to run a different,
      non-blocklisted program.

   **Deliberately still open / not closed by either fix, for static
   tools:** an override naming the *same* program with *different
   arguments* than the catalog intended is still allowed — e.g.
   `{"tool": "device_uptime", "command": "cat /etc/shadow"}` still
   executes, because `cat` matches `device_uptime`'s own declared
   program. Fully closing this would mean either (a) not honoring a
   caller override at all, or (b) restricting an override to a
   catalog-declared parameter set (e.g. an allowed-flags/allowed-values
   schema per tool) rather than program-level pinning — both remain
   `docs/24` §9 open question 3, not resolved by this fix.

   **Related, added 2026-08-14 (FR-25b):** a catalog tool may now
   declare `"type": "dynamic"` to *intentionally* opt out of the
   program-pin entirely — this is a deliberate widening for tools
   designed to run arbitrary caller-supplied commands (see
   `adhoc_diagnostic` in `diag-triage-catalog.json`), not a bug or a
   silent regression of item 5's mitigation. The blocklist still
   applies to a dynamic tool's command unconditionally. Whoever
   authors the catalog is responsible for only marking a tool dynamic
   when that's the intended behavior — there's no separate approval
   gate on setting `"type": "dynamic"` itself, which is a new,
   catalog-author-trust-dependent surface worth noting alongside item
   6 below.

   **Also related, added 2026-08-14 (FR-25c):** a different, narrower
   risk than the caller-override gap above — a catalog *author* error
   (a static tool's own declared `command` happening to be blocklisted
   or malformed) — is now caught automatically at process startup
   rather than silently sitting in the catalog until someone happens
   to invoke that tool. The affected tool is marked skipped and
   rejected wholesale, including against a request supplying its own
   safe override, for the rest of the process's lifetime. This doesn't
   change anything about the still-open argument-level override gap
   described above; it closes a separate failure mode (a broken
   catalog default), not that one.
6. **No access-control check exists on who may invoke a tool** — added
   2026-08-14, same source as item 5. Any caller able to address a WRP
   message to `diag-server` through Parodus can invoke any catalog
   tool. **Design added 2026-08-14, scoped to Phase 2 by direct
   instruction — see `docs/24_diag_server_merge_plan.md` §10.**
   Recommended approach: retire diag-server's direct Parodus
   registration in favor of routing through Dispatch Core, which
   decodes the request (completing §8 step 3's legacy framing
   adapter), runs the existing ACL Policy Store query (FR-4) before
   ever forwarding to diag-server, and returns
   `{"tool", "exit_code": <nonzero>, "stdout": "access denied"}` on
   denial without diag-server seeing the request at all. Not yet
   implemented — design only in this pass; still blocked on A1
   (caller-identity format) and §8 step 3.

## 13. Requirement Prioritization

- Must Have:
  - FR-15 through FR-32, FR-25a, FR-25b, FR-25c (FR-27 moved here
    2026-08-14; now implemented; FR-25a, FR-25b, and FR-25c added
    2026-08-14, implemented at introduction)
  - NFR-15, NFR-16, NFR-17, NFR-18, NFR-19, NFR-19a, NFR-20
    (NFR-17 moved here 2026-08-14; now implemented, see §9.3;
    NFR-19a added 2026-08-14, implemented at introduction)
- Should Have:
  - FR-31, NFR-11 through NFR-14, NFR-13a (NFR-13a added
    2026-08-14, implemented at introduction)
- Target/Next Baseline:
  - (empty — both former entries here are now implemented)

## 14. Traceability to Repository Artifacts

- Runtime behavior reference: `diag-server-nn.c`
- Sample catalog reference: `diag-triage-catalog.json`
- Build/reference configuration: `CMakeLists.txt`
- Architecture/workflow context: `README.md`
