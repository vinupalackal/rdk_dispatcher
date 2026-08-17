# Diag-Server Merge Plan (Plan Only — No Code Yet)

Per direct instruction: this is a plan, not a merge. Nothing in
`reference-impl/` is touched by this document, and no OpenSpec change
has been drafted yet — that's the deliberate next step, after this
plan is reviewed. Grounded in the 9 uploaded files: `README.md`,
`ARCHITECTURE.md`, `REQUIREMENTS.md`,
`DEVICE_SIDE_COMPONENT_DIAGRAM.md`, `DIAGRAM_EXPLAINED.md`,
`diag-triage-catalog.json`, `diag-server-nn.c`, `CMakeLists.txt`
(`DEVICE_SIDE_COMPONENT_BRIEF.md` was empty).

## 1. What diag-server actually is, in this project's own vocabulary

Diag-Server is a real, already-built C daemon (not an illustrative
sketch like everything under this project's `reference-impl/` so
far) that:

- Registers with Parodus over raw nanomsg — PUSH connects to
  `tcp://127.0.0.1:6666`, PULL binds `tcp://127.0.0.1:6669` — using
  WRP message type 9 for registration.
- Receives WRP type-3 requests, each carrying a **msgpack-encoded
  inner payload** (not JSON-RPC 2.0): `{"tool": "<name>", "command":
  ""}`.
- Looks the tool up in a JSON catalog (`diag-triage-catalog.json`),
  falls back to the catalog's default command if the request didn't
  override one, checks the command's first token against a hardcoded
  blocklist, executes via `popen()`, and returns
  `{"tool", "exit_code", "stdout"}`.
- Handles WRP type-10 keepalives inline on the main thread; dispatches
  every type-3 request to a detached pthread so the main receive loop
  never blocks.

In this project's terms: diag-server is a working example of **real
command execution** — the exact capability `add-phase1-command-execution-exception`
(A15) just brought into Phase 1 scope, and its catalog of ping/
ifconfig/cat/ps/dmesg-style commands is squarely **triage-plane**
work under `CLAUDE.md`'s own definition ("on failure/anomaly,
collects evidence... emits a structured...record").

## 2. Pre-existing issues found while reading the baseline (not introduced by any proposed merge)

Worth surfacing before planning around this code, not after:

- ~~**Build/source mismatch.** `CMakeLists.txt` compiles
  `diag-server.c`; the repository contains `diag-server-nn.c`.~~
  **Corrected 2026-08-14 — turned out not to be a real bug.** The
  uploaded `CMakeLists.txt` already correctly built
  `diag-server-nn.c`; `README.md` and `REQUIREMENTS.md` (NFR-20, Risk
  item 1) were describing a mismatch that didn't exist in the actual
  file. The documentation was corrected to match the code, not the
  other way around.
- ~~**CMake links `wrp-c`/`libparodus`/`cimplog`, but the C code never
  calls into them.**~~ **Corrected 2026-08-14** — removed from
  `target_link_libraries` and `target_include_directories`; only
  `nanomsg`, `msgpackc`, `cjson`, and `pthread` remain, matching what
  `diag-server-nn.c` actually uses. `README.md`'s prerequisites list
  and `REQUIREMENTS.md` §6.2 were updated to match.
- **Staging location established.** The corrected baseline now lives
  at `external/diag-server/` in this project: 5 files copied
  byte-identical from the original upload (`diag-server-nn.c`,
  `diag-triage-catalog.json`, `ARCHITECTURE.md`,
  `DEVICE_SIDE_COMPONENT_DIAGRAM.md`, `DIAGRAM_EXPLAINED.md`) plus 3
  files carrying the two housekeeping corrections above
  (`README.md`, `REQUIREMENTS.md`, `CMakeLists.txt`) — 8 files total
  (the 9th uploaded file, `DEVICE_SIDE_COMPONENT_BRIEF.md`, was empty
  and wasn't brought over). Nothing from §5–§7 below has been applied
  yet. This isn't the final home question from §9.4
  (`reference-impl/diagnostics/` vs. its own top-level directory) —
  that's still open.
- **No ACL/authorization of any kind.** Any caller that can address a
  WRP message to `diag-server` through Parodus can invoke any catalog
  tool. There is no equivalent of this project's FR-4 single ACL
  checkpoint anywhere in this code.
- **Caller-supplied command override bypasses the catalog's intended
  safety boundary.** `decode_request_payload()` accepts a `command`
  field directly from the request; if present, it's used as-is
  (subject only to the first-token blocklist), regardless of what the
  catalog says for that tool. A caller can send
  `{"tool": "device_uptime", "command": "cat /proc/uptime; rm -rf /tmp/x"}`
  — first token `cat` passes the blocklist, and everything after the
  `;` would have executed under `popen()`'s shell in the original
  baseline. **This was a real command-injection path**, not a
  hypothetical one — it existed independent of any merge and was
  treated as a finding regardless of what happened next. **Narrowed in
  two stages, both 2026-08-14:**
  1. The NFR-17 fix immediately below: with no shell in the execution
     path, this exact technique (`;`, `&&`, a pipe) no longer runs a
     second command — the whole string becomes literal argv tokens
     passed to `cat`, which just fails.
  2. **`is_command_safe()`, added the same day** — a single, common
     safety check that both a catalog-default command and any caller
     override now go through in `handle_request()` (previously only
     `is_blocked()` ran there). On top of the existing blocklist, it
     requires a caller override's program (`argv[0]`) to exactly match
     the catalog entry's own declared program for that tool. A caller
     can no longer redirect `device_uptime` to run `ls`, `dmesg`, or
     any other non-blocklisted program — only `cat`, `device_uptime`'s
     own declared program, is possible via this tool now.

  **Still open, deliberately not closed by either fix, for static
  tools:** an override naming the *same* program with *different
  arguments* than the catalog intended still executes —
  `{"tool": "device_uptime", "command": "cat /etc/shadow"}` still
  runs, because `cat` matches `device_uptime`'s declared program.
  `is_command_safe()` is program-level pinning, not a full
  argument-level allowlist. Closing this fully still means one of the
  two original alternatives from §9 open question 3: stop honoring a
  caller override at all, or restrict an override to a catalog-declared
  parameter set (e.g. per-tool allowed flags/values) — neither is
  implemented; program-pinning is a distinct, narrower middle ground
  this fix adds between "no check" and either of those. **Closed
  2026-08-16**: §9 Q3 was resolved in favor of the first alternative
  (stop honoring a caller override at all, for static tools) — see §14
  item 7 and §15's "two flagged behavioral changes" for the full
  implementation. This paragraph is left as-is for the historical
  record of the narrower program-pinning-only state that existed
  between 2026-08-14 and 2026-08-16.
- **Static vs. dynamic tool types, added 2026-08-14 (same day, follow-up
  to `is_command_safe()`).** Per your request: a catalog tool entry may
  now declare `"type": "static"` (default — every pre-existing tool)
  or `"type": "dynamic"`. This isn't a new gap; it's a labeled,
  intentional widening for the specific tools an operator chooses to
  mark that way, distinct from the previous *implicit* override
  ambiguity this whole finding started from:
  - **Static** (unchanged from the `is_command_safe()` fix above): the
    program-pin applies in full — an override may change arguments but
    not the program.
  - **Dynamic**: the program-pin is skipped for this tool by design —
    `handle_request()` reads the catalog entry's `type` field and
    passes a `dynamic_type` flag into `is_command_safe()`, which then
    bypasses the pin check (but never the blocklist — that still
    applies unconditionally to every command, static or dynamic). This
    directly implements what you described: "if request is dynamic we
    can skip the static lookup" (the program-pin/catalog-command lookup
    is skipped) "and add the guard check for all commands" (the
    blocklist check is never skipped, for either type). See the new
    `adhoc_diagnostic` example tool in `diag-triage-catalog.json`, which
    has no fixed `command` at all — it's inherently a dynamic,
    caller-command-driven tool.
  - Also added: a `"plane"` field per catalog tool entry (`config-apply`
    / `management` / `control` / `triage` — this project's plane
    model). Every current diag-server tool, including the new
    `adhoc_diagnostic`, is `"triage"`, consistent with A16/§7's
    conclusion that diag-server is entirely a triage-plane service.
    This is categorization metadata for the future toolset-manifest
    conversion (§8 step 2) — diag-server itself does not branch on it;
    it's logged alongside `type` per request for observability
    (REQUIREMENTS.md NFR-19a).
  - Verified via a standalone harness (nine checks): a static tool's
    override is still rejected for a different program; a dynamic tool
    with no catalog command accepts any non-blocklisted caller
    program; a dynamic tool's command is still rejected if blocklisted;
    and flipping only the `dynamic_type` flag between two otherwise
    identical calls flips the outcome — confirming the type is what
    gates the check, not some other difference.
- **Init-time static command validation, added 2026-08-14 (same day),
  per your explicit design request** ("call the static command safety
  during the process initialization itself to avoid the delay in
  runtime; if anything detected mark it as skipped toolset category").
  New `validate_static_commands()` runs once, right after
  `load_catalog()` in `main()`, before either transport socket is
  created and before any request can possibly arrive:
  - For every tool with a non-empty catalog `command`, it tokenizes
    the string and runs the existing blocklist check (`is_blocked()`)
    against it — the same checks that used to run again on every
    single request that resolved to that tool's default command,
    re-deriving an answer that cannot change for the process's
    lifetime (there is no catalog hot-reload path anywhere in this
    code).
  - A tool that fails (blocklisted, or unparseable — e.g.
    whitespace-only) is marked with a persistent `"_skipped"` status
    and a recorded reason. **This is the "skipped toolset category"
    you asked for**: a skipped tool is rejected for *every* subsequent
    request naming it, for the rest of the process's life — including
    a request that supplies its own, independently-safe `command`
    override. This is a deliberate, whole-tool exclusion, not a
    narrower rule that only blocks the "use the catalog default" path
    — a catalog author's mistake shouldn't leave a side door open via
    a well-chosen override.
  - A tool that passes has its program name (`argv[0]`) cached once,
    as `"_program_token"`, on its catalog entry. `is_command_safe()`'s
    program-pin check (the fix two bullets above) now takes this
    cached token directly instead of re-tokenizing the catalog's
    command string on every request with an override — the only work
    still done live, per request, is validating the override's own
    content, since that's the one piece of data that's genuinely new
    each time.
  - Every skip is logged individually at startup (tool name, offending
    command, reason) plus a one-line summary, so a misconfigured
    catalog is visible in syslog at service start rather than
    discovered later, only when something happens to invoke the broken
    tool.
  - Verified via a standalone harness run against the **real**
    `diag-triage-catalog.json` (parsed with a minimal cJSON-compatible
    shim, since the sandbox has no network access to fetch the real
    cJSON library): all 11 real tools validate clean with zero
    false-positive skips; `device_uptime` gets a cached token of
    `cat`; `adhoc_diagnostic` (no catalog command) correctly gets no
    token; a deliberately injected blocklisted tool and a deliberately
    injected unparseable tool are both correctly marked skipped with
    distinct reasons; and — the property that matters most — a skipped
    tool still rejects a request carrying an independently-safe
    override (`cat /proc/uptime`), proving the skip cannot be
    bypassed. See REQUIREMENTS.md FR-25c/NFR-13a and README.md §2.5
    for the full design writeup.
- ~~**`popen()` is shell-based execution** — the code's own `README.md`
  names this as a known gap (target: move to `fork`/`execve`).~~
  **Fixed 2026-08-14.** `run_command()` now executes via `execvp()`
  with a tokenized argument vector (`tokenize_argv()`); no shell is
  invoked anywhere in the path — this supersedes the FR-27 fix
  immediately below, which had moved to `fork()`+`pipe()` but still
  routed through `execl("/bin/sh","sh","-c",cmd,NULL)`. That shell
  layer is now gone too. `active_connections`, the one catalog entry
  that relied on shell features (a pipe and a stderr redirect), was
  rewritten to use two new optional catalog fields —
  `suppress_stderr` and `count_lines_matching` — that reproduce the
  same output natively, without a shell. See §6 for the full writeup
  and REQUIREMENTS.md NFR-17 for the acceptance-criteria detail.
  Verified via a standalone harness: the exact compound-injection
  string above was run against the fixed code and confirmed to leave
  its intended target file untouched (proving `rm` never executes); a
  `|`-containing string run through `echo` printed the pipe character
  literally instead of piping through a second program; and
  `active_connections`' new fields were confirmed to reproduce its
  original output shape exactly.
- ~~**Catalog `timeout` is parsed but never enforced.** A hung command
  blocks its worker thread indefinitely (though not the main loop,
  since execution is already off the receive thread).~~ **Fixed
  2026-08-14.** `run_command()` in `diag-server-nn.c` was rewritten
  from `popen()`/`pclose()` to `fork()`+`pipe()`+
  `execl("/bin/sh","sh","-c",cmd,NULL)`, which preserves the same
  shell-interpretation semantics (still required — `active_connections`
  in `diag-triage-catalog.json` uses a shell pipe) while exposing a real
  child PID. The parent tracks a `clock_gettime(CLOCK_MONOTONIC,...)`
  deadline and polls the read end of the pipe with a shrinking timeout
  budget; if the deadline passes with the child still running, it's
  `kill(pid, SIGKILL)`'d and reaped via `waitpid()`. Effective timeout
  resolves from the catalog entry's own `timeout` field when present
  and positive, else `DEFAULT_TIMEOUT_SEC` (30s — the value was
  previously defined but unused; now wired in as originally specified
  by the user for this fix). Response on timeout: exit code 124
  (matching GNU coreutils' `timeout(1)` convention), output `"command
  timed out after Ns"`. The same kill-and-reap path also closes a
  related latent hang identified during this fix: the old `pclose()`
  could block forever if a command exceeded `MAX_OUTPUT_BYTES` while
  diag-server stopped reading, leaving the child writing into a full,
  undrained pipe — that path is now also killed rather than left to
  hang, with whatever output was captured before the cap returned
  (same truncation behavior as before, just non-hanging). Verified via
  a standalone harness exercising `run_command()` directly: a normal
  command with a shell pipe still executes correctly; a `sleep 30`
  under a 2s timeout is killed within ~2s (not 30s) with exit code
  124; a command producing unbounded output under `yes` returns
  promptly instead of hanging; no orphan/zombie processes remain after
  either kill path. At the time of this fix, `/bin/sh -c` was still in
  use (out of scope for this specific fix, tracked separately under
  NFR-17 above) — **NFR-17 was subsequently closed the same day**,
  replacing this `execl("/bin/sh","sh","-c",cmd,NULL)` call with
  `tokenize_argv()`+`execvp()`. See the NFR-17 bullet above for that
  follow-up fix; the timeout/kill/reap mechanics described here are
  unaffected by it.
- ~~**Output cap is 64 KiB in the code**, though `DIAGRAM_EXPLAINED.md`
  and `ARCHITECTURE.md` both describe it as 16 KiB / 10s in prose.**~~
  **Corrected 2026-08-14 — and this bullet itself was slightly
  wrong.** Only `DIAGRAM_EXPLAINED.md` actually had the stale "16KB /
  10 seconds" figure; `DEVICE_SIDE_COMPONENT_DIAGRAM.md` already
  correctly said 64KB, and `ARCHITECTURE.md` doesn't state a specific
  figure at all — this bullet's original claim that both files had it
  wasn't checked closely enough before writing it down. Fixed in
  `DIAGRAM_EXPLAINED.md`: output is now described as ranging from 0
  bytes up to the real 64KB ceiling (`MAX_OUTPUT_BYTES`), and the
  "10 seconds" execution-limit claim was corrected too — `diag-server-nn.c`
  defines `DEFAULT_TIMEOUT_SEC` but never references it anywhere, so no
  timeout is actually enforced (same underlying gap as the
  catalog-`timeout`-unenforced item above).

None of this blocks planning — it's exactly the kind of gap this
project's whole ACL/verification/sandboxing architecture exists to
close. It does mean the merge can't be purely mechanical ("wrap the
existing logic as-is") if FR-4 is to actually hold.

## 3. What "keep all external communication the same" means, precisely

This is the plan's central constraint, so it's worth being exact
about what's frozen and what's not.

**Stays byte-identical — no proposed change touches these:**
- Parodus addresses: PUSH to `tcp://127.0.0.1:6666`, PULL bound at
  `tcp://127.0.0.1:6669`.
- WRP message type numbers: 3 (request/response), 9 (registration),
  10 (keepalive).
- The registration message shape: `{msg_type:9, service_name:
  "diag-server", url: "tcp://127.0.0.1:6669"}`.
- The keepalive ack shape: `{msg_type:10}`.
- The outer WRP envelope fields and their msgpack encoding
  (`source`, `dest`, `transaction_uuid`, `content_type`, `payload`).
- The **inner payload shape and its msgpack encoding**:
  `{tool, command}` in, `{tool, exit_code, stdout}` out. A caller that
  today sends this exact request gets this exact response shape after
  any merge.
- Socket tuning (receive timeout, buffer size, send timeout,
  connection retry/backoff behavior).

**Changes internally, invisible to a well-behaved external caller,
visible only to a caller currently relying on the injection gap or
the missing ACL check:**
- What happens *between* "payload decoded" and "response built."
  Today: catalog lookup → weak safety check → `popen()`. After the
  merge: catalog-derived toolset resolution → Dispatch Core's real
  ACL Policy Store check → execution through this project's own
  pipeline (see §5–6).
- A request from an identity with no grant for the target diagnostic
  tool now gets denied instead of silently executing. This is a
  **behavior change**, but not a wire-format change, and not
  something "external communication stays the same" should be read
  to protect — closing an authorization gap is the point of this
  merge, not something it accidentally breaks.

## 4. Where diag-server fits in this project's taxonomy

Following the same reasoning already settled for application
management (`OPEN_QUESTIONS.md` A16): this doesn't need a new plane.
Every diag-server tool (`ping_google_ip`, `wan_status`,
`dns_servers`, `device_uptime`, `memory_usage`, `interface_config`,
`routing_table`, `active_connections`, `process_status`,
`kernel_log`) is read-only, evidence-gathering work — squarely
Triage, exactly as `CLAUDE.md` defines it.

**Proposed: a new toolset domain, `diagnostics`** (alongside
`common, network, wifi, DOCSIS, vendor, application`), implementing
the triage plane — not folded into config-apply/management/control,
since none of its current tools mutate device state.

**Confirmed 2026-08-14 — `diagnostics` is a separate toolset from the
existing Triage Toolset** (`add-triage-skillset-mapping-phase1`'s
`core-triage`/`wifi-triage` plugins), not folded into it. Both
implement the triage plane, but the Triage Toolset is built around
automatic, event-driven evidence capture (reacts to sysevent/Netlink,
per `CLAUDE_CODE_WORKFLOW.md`'s `dispatcher_triage.c` sketch, static +
dynamic plugins merged inside one process's `capabilities()`), while
`diagnostics` is on-demand, cloud-initiated command execution — a
different trigger model, kept out of the Triage Toolset's process and
MCP schema rather than mixed into it. This decides §9 open question 1;
see §9 for what it changes downstream (questions 4 and 5).

**How this connects to A13's already-confirmed MCP granularity
decision:** diag-server's catalog-driven "pick a tool, run it" shape
is *exactly* what A13 already designed for — one MCP tool per
toolset, method as a structured argument. `tools/call` with
`name: "diagnostics"`, `arguments: {"method": "ping_google_ip",
"params": {}}` and the legacy msgpack `{"tool": "ping_google_ip",
"command": ""}` shape both resolve to the identical internal
`(toolset: "diagnostics", method: "ping_google_ip")` tuple. This
isn't a coincidence to route around — it's confirmation the existing
design already anticipated this shape.

## 5. The framing mismatch — a real, unresolved tension, not papered over

This project's own WRP examples so far (`triage.capabilities`,
`toolset.push`) use **JSON** serialization for both the outer
envelope and the inner JSON-RPC 2.0 payload — a deliberate choice
recorded in `require-payload-encryption-and-message-routing` and
elsewhere. Diag-server uses **msgpack** for both layers, and a
**bespoke, non-JSON-RPC inner shape** (`{tool, command}`, not
`{jsonrpc, method, params, id}`).

**Confirmed 2026-08-14 — the msgpack transport and payload shape stay
exactly as diag-server already speaks them, permanently, not as a
time-bounded shim.** Direct instruction: keep the transport mechanism
and data packaging identical to what diag-server's code already uses
today. The deciding reason given is a practical one, not just
"unchanged is simpler": there is currently **no other transport
mechanism** by which diag-server's real, existing callers can reach
`diagnostics` — MCP `tools/call` and generic JSON-RPC 2.0 aren't
things those callers speak today, so there's nothing to migrate them
*to* yet. A deprecation plan for a shape with no working alternative
isn't a real plan, so this closes §9 question 2 as: **msgpack is the
third permanent entry framing**, decoding down to the same internal
`(toolset, method, params)` tuple as the other two and flowing through
the identical ACL/resolution/execution path, exactly as first proposed
above — just confirmed rather than left open.

**New, separate work this decision creates — flagged, not yet
planned:** since msgpack isn't going away, the thing actually worth
planning now is the *other direction* — making `diagnostics` reachable
through MCP `tools/call` and JSON-RPC 2.0 as well, so a future caller
that does speak one of those isn't forced through the legacy shape.
Both of those entry framings already exist generically in Dispatch
Core (per A12) and decode any toolset's requests the same way; they
don't yet reach `diagnostics` specifically only because `diagnostics`
isn't yet a real toolset in Dispatch Core's manifest. Per §8 step 2
(toolset manifest derivation, already partially done — the catalog
carries `type`/`plane` now), finishing that conversion is most of what
"the other format" needs — this isn't a second bespoke adapter to
build alongside the msgpack one, it's `diagnostics` becoming a normal
toolset like any other and inheriting MCP/JSON-RPC reachability for
free. Confirming that inheritance actually holds once the manifest
conversion is finished (rather than assuming it) is the concrete
next step, tracked as a addition to §8's staged plan.

**What stays permanently true regardless:** every future `diagnostics`
method still needs to work correctly through the msgpack legacy shape
indefinitely — that's the accepted, ongoing cost of confirming it
permanent, the same cost A12 already named for MCP vs. JSON-RPC, now
carried by a third, less closely related framing (binary, not JSON;
bespoke, not JSON-RPC) rather than a deprecatable one.

**Amendment, 2026-08-15 — scoped, additive exception for an optional
`plane` field (§15 B.5), per direct instruction.** "Permanently frozen"
above meant the *existing* shape (`{tool, command}` request,
`{tool, exit_code, stdout}` response) is never removed or restructured
— it did not anticipate a purely additive optional key. `plane` is
added to the request map as an **optional** third key:
`{"tool": "<name>", "command": "", "plane": "<optional>"}`. This is
backward-compatible in both directions, not just in principle:
`decode_request_payload()` already iterates the msgpack map and
extracts only the keys it recognizes by name, silently skipping any
other key — so an old caller that never sends `plane` is completely
unaffected, and an old (not-yet-upgraded) diag-server binary receiving
a `plane` key from a newer caller already ignores it today, with no
crash or parse failure, exactly as it ignores any other unexpected
key. Nothing about the existing two fields, the response shape, or the
outer WRP/msgpack framing changes. This is recorded as a scoped
exception to the freeze above, not a reopening of it — the freeze
still holds for the two original fields and the framing itself.

## 6. Security gaps this merge should close, and how, without touching the wire contract

| Gap (§2) | Fix, scoped to stay wire-compatible |
|---|---|
| No ACL check | Insert Dispatch Core's ACL Policy Store query (FR-4) between tool resolution and execution — same checkpoint every other toolset already goes through, per `docs/20`'s design. A denied caller gets `{"tool", "exit_code": <nonzero>, "stdout": "access denied"}` in the *same* response shape, not a protocol-level change. **Full design in §10 (Phase 2, 2026-08-14)** |
| Caller-supplied command override bypasses catalog intent | Treat the catalog as authoritative for what a tool *does*; do not honor a caller-supplied `command` override that diverges from the catalog entry, or restrict any allowed override to a strict, catalog-declared parameter set instead of an arbitrary shell string. **Narrowed in three stages, all 2026-08-14**: the shell-removal row below made shell metacharacters in an override inert; `is_command_safe()` additionally rejects an override naming a different *program* than the catalog declares for that tool, via the same common check the catalog's own default command is validated through; a per-tool `"type"` field (`static`/`dynamic`, see §2 finding) then let a catalog author explicitly, intentionally opt a *specific* tool out of the program-pin for tools designed to be caller-command-driven, while the blocklist stays mandatory for every tool regardless of type. **Resolved 2026-08-15 (§9 Q3): static tools drop overrides entirely** — a static tool always runs its own catalog-declared command; program-pinning's residual "same program, different arguments" gap is closed by removing the override path for static tools, not by narrowing it further. Not yet implemented in `diag-server-nn.c` — the permanent model is decided, the code change is a separate, explicit next step (see §9 Q3) |
| ~~`popen()` shell execution~~ | **Done 2026-08-14.** `run_command()` now executes via `execvp()` with a tokenized argument vector (`tokenize_argv()`) — no shell anywhere in the path, superseding the FR-27 fix's intermediate `execl("/bin/sh","sh","-c",...)` step. `active_connections`, the only catalog entry needing shell features, now uses two new catalog fields (`suppress_stderr`, `count_lines_matching`) instead — see the §2 finding above for the full writeup |
| ~~Timeout not enforced~~ | **Done 2026-08-14.** Catalog `timeout` field (or a 30s default) is now enforced by killing the child with `SIGKILL` at the deadline — see the §2 finding above for the full implementation writeup |

## 7. How this fits Phase 1 vs. Phase 2

This lines up cleanly with the just-confirmed Phase 1 exception
(`add-phase1-command-execution-exception`, A15) rather than requiring
new architecture: the `diagnostics` toolset is real command execution,
run **in-process** as Phase 1's explicit, tracked exception (same
model as any other Phase 1 command-executing toolset — no new
exception needed, this is what A15 already covers). Its Phase 1
rollback (if `diagnostics` is later updated via `toolset.push`) would
follow the same health-check-gated in-process swap already designed
for A15, not the artifact-fallback model reserved for Phase 2's
out-of-process hardening.

Sandboxing, encryption, and the independent security review stay
Phase 2 for `diagnostics` exactly as for every other Phase 1 toolset
— this merge does not ask for, or need, an exception beyond the one
already granted.

**Added 2026-08-14, by direct instruction:** the ACL/authorization gap
(§2, §6 table, Risk item 6 in `REQUIREMENTS.md`) is explicitly scoped
to **Phase 2** — not part of what needs to be true before `diagnostics`
can run under the A15 in-process exception. §10 below is the Phase 2
design and staged implementation plan for it; no code changes are made
in this pass.

## 8. Staged merge plan — steps, no code written yet

1. **Baseline correction** (housekeeping, not architecture) — **done,
   2026-08-14**: the `diag-server.c`/`diag-server-nn.c` build mismatch
   turned out not to be real (docs corrected instead); the CMake link
   list was corrected to match what the code actually uses; the
   output-cap/execution-timeout documentation drift in
   `DIAGRAM_EXPLAINED.md` was reconciled against the code (0–64KB
   output range, no enforced execution timeout).
2. **Toolset manifest derivation**: convert `diag-triage-catalog.json`
   into a toolset manifest shape consistent with this project's
   existing toolset-lifecycle model (`load_type`, `plane: "triage"`,
   method schemas per tool) — without changing the catalog file's own
   external format, since it's not part of the wire contract with
   Parodus/cloud, only an on-device config file. **Partially done
   ahead of schedule, 2026-08-14**: the catalog itself now carries a
   `"plane"` field per tool (all currently `"triage"`, confirming the
   A16/§7 conclusion) plus a `"type"` field (`static`/`dynamic`, see
   §2 finding and §6) — both requested directly, ahead of this step's
   original manifest-conversion trigger. This is real catalog data
   diag-server now reads and (for `type`) acts on, not just a design
   note; the full manifest-shape conversion this step describes
   (`load_type`, method schemas, the actual OpenSpec toolset-lifecycle
   mapping) is still pending.

   **Added 2026-08-14, follow-on from §5/§9 question 2's confirmation:**
   once this step's manifest conversion is finished, confirm
   `diagnostics` is actually reachable through MCP `tools/call` and
   generic JSON-RPC 2.0 the same way any other toolset already is — per
   A12, those two entry framings are generic and toolset-agnostic, so
   this should fall out of `diagnostics` simply being a properly
   registered toolset, not require a bespoke third adapter. Confirming
   that holds (rather than assuming it) is the concrete next step here,
   separate from and in addition to step 3's msgpack-specific adapter
   below, which stays permanent per §5.
3. **Legacy framing adapter design**: specify (not yet implement) how
   Dispatch Core decodes diag-server's msgpack `{tool, command}`
   payload into the internal `(toolset, method, params)` tuple, and
   re-encodes the internal result back into
   `{tool, exit_code, stdout}` msgpack — a translation layer, not a
   rewrite of either side. **Done, 2026-08-14 —
   `reference-impl/diag_legacy_framing.c`.** An illustrative sketch,
   consistent with the rest of `reference-impl/`, showing the full
   decode/encode pair for both hops: the external WRP request into
   Dispatch Core (`diag_legacy_handle_request()`,
   `diag_decode_request()`, `diag_build_response_msgpack()`), and
   Dispatch Core's forward to diag-server's own process
   (`diag_toolset_ipc_forward()`, `diag_decode_response_to_json()`),
   using diag-server's exact existing msgpack field layout on both —
   mirrored field-for-field from `decode_request_payload()`/
   `build_response_payload()` in `diag-server-nn.c` (not copied code;
   this runs in Dispatch Core, a different binary, deliberately kept
   byte-shape-identical). `dispatcher_command_path.c`'s
   resolve/authorize/dispatch logic is reused completely unmodified —
   this file only supplies the one toolset-specific realization of its
   already-generic, already-extern `toolset_ipc_forward()` for
   `diagnostics`. Net effect: `diag-server-nn.c` itself needs zero code
   changes to be integrated this way.
4. **ACL insertion point design**: specify where the ACL Policy Store
   query sits in this specific request path, mirroring
   `dispatcher_command_path.c`'s existing shape. **Done, 2026-08-14 —
   see §10.** Scoped explicitly to Phase 2 per direct instruction;
   design only, no code in this pass.
5. **Execution model design**: specify the `fork`/`execve` replacement
   for `popen()`, and the timeout-enforcement mechanism, at a design
   level. **Done ahead of schedule, 2026-08-14, in two stages** —
   implemented directly in the staged baseline (not yet the OpenSpec
   change or reference-impl proper) per your explicit requests, ahead
   of step 6/7. Stage one added `fork()`+`pipe()` for timeout
   enforcement but still routed through `execl("/bin/sh","sh","-c",...)`;
   stage two (same day) removed that shell layer too, replacing it with
   `tokenize_argv()`+`execvp()`. Both FR-27 (timeout) and NFR-17
   (no shell) are now fully implemented in `external/diag-server/` —
   see §2 and §6.
6. **Draft the OpenSpec change** (e.g.
   `openspec/changes/merge-diag-server-as-diagnostics-toolset/`) once
   the open questions in §9 are answered — this is the next step
   after this plan, still not the reference-impl code itself.
7. **Reference-impl code** — explicitly deferred, per your instruction
   ("next steps, not now").

## 9. Open questions needing your decision before the OpenSpec change is drafted

1. ~~Separate toolset or fold into the existing Triage Toolset?~~
   (§4) **Confirmed 2026-08-14: separate.** `diagnostics` gets its own
   toolset domain, own MCP tool identity (per A13's coarse
   one-tool-per-toolset shape — `name: "diagnostics"`, not folded into
   `name: "triage"`'s method schema), and its own process boundary,
   distinct from the Triage Toolset's `core-triage`/`wifi-triage`
   process. This strengthens the case for question 4's "own top-level
   directory" option below (a separate toolset is less naturally a
   subdirectory of `reference-impl/plugins/`'s existing layout), and
   for question 5's separate requirements-numbering option — neither
   is decided by this confirmation alone, both are still open.
2. ~~Legacy msgpack framing — permanent third entry shape, or a
   time-bounded compatibility shim with a stated deprecation
   direction?~~ (§5) **Confirmed 2026-08-14: permanent.** Transport
   and payload shape stay identical to diag-server's existing code, no
   deprecation plan — there's no other transport diag-server's real
   callers speak today, so nothing to migrate them to. Follow-on work
   this creates (not yet done): confirm `diagnostics` also becomes
   reachable via MCP `tools/call` and JSON-RPC 2.0, once §8 step 2's
   manifest conversion is finished, so those framings aren't blocked
   even though msgpack stays primary for existing callers.
3. **Caller-supplied command override — remove entirely, or restrict
   to catalog-declared parameters only?** (§6) **Narrowed twice,
   2026-08-14**:
   1. `is_command_safe()` added a third, intermediate option not
      originally listed here — program-level pinning (an override may
      change arguments but not the program) — as a common check
      applied uniformly to catalog and override commands alike.
   2. A per-tool `"type": "static"`/`"dynamic"` catalog field then
      made that pin *optional per tool*: a catalog author can mark a
      specific tool `"dynamic"` to deliberately exempt it from the
      pin, rather than the pin being an unconditional, uniform rule
      across every tool. `adhoc_diagnostic` in
      `diag-triage-catalog.json` is the reference example.

   Both are implemented and are not placeholders; this is a real, if
   partial and now per-tool-configurable, answer already in the code.

   **Resolved 2026-08-15, by direct instruction.** Static tools drop
   caller-supplied overrides entirely — a static tool always runs its
   own catalog-declared command, full stop, no `command` field
   honored regardless of whether it names the same program. Reasoning
   given: the static/dynamic split already gives a caller who
   genuinely needs flexibility a proper, secured path (a `"dynamic"`
   tool, now additionally gated behind ACL grant, mandatory
   encryption, and entry-framing restriction, per §14 item 3) — so a
   static tool's override no longer serves a real use case, and
   removing it closes the residual "same program, different
   arguments" gap completely rather than narrowing it further (the
   `cat /etc/shadow`-via-`device_uptime` example this question has
   used throughout is no longer possible under this model, not just
   harder). Dynamic tools keep honoring caller-supplied commands, still
   subject to `is_command_safe()`'s blocklist — mandatory regardless of
   type, unchanged — plus whatever additional grant/encryption/framing
   requirements a specific dynamic tool carries.

   **Flagged, not yet done**: this is a real change to
   `handle_request()`'s and `is_command_safe()`'s existing, already
   verified behavior — today, a static tool's override *does* execute
   when the program matches (confirmed by
   `test_init_validation.c`'s own passing checks, e.g. "device_uptime,
   override same program diff args: executes"). Per the standing rule
   for this plan, a change to diag-server's actual code gets confirmed
   before it's made, not bundled into recording the decision — this
   entry documents the *permanent model* being resolved; implementing
   it in `diag-server-nn.c` is a separate, explicit next step.
4. ~~Where does the diagnostics toolset's source live~~ — **Resolved
   2026-08-15: `external/diag-server/`, unchanged, not moved into
   `reference-impl/diagnostics/`.** See §14 item 8.
5. ~~Does `diagnostics` need its own requirements numbering?~~
   **Resolved 2026-08-15, by direct instruction: no separate prefix —
   reuse the master project's own `FR`/`NFR` sequence.** `REQUIREMENTS.md`'s
   requirements, originally `FR-001`…`FR-018`/`NFR-001`…`NFR-010`, are
   now renumbered to continue directly from the master's `FR-14`/
   `NFR-10`: `FR-15`…`FR-32` (lettered sub-items shifted with their
   parent, e.g. `FR-011a` → `FR-25a`) and `NFR-11`…`NFR-20`. Applied
   across `REQUIREMENTS.md`, `README.md`, and this document's own
   cross-references — see the dated callout at the top of
   `REQUIREMENTS.md` for the full mapping note. `FR-4`, cited a few
   times in this plan and in `REQUIREMENTS.md` as the master project's
   single ACL checkpoint, was never a local diag-server ID and was left
   untouched.

## 10. ACL/authorization design (Phase 2) — added 2026-08-14

**Finding this closes:** "No ACL/authorization of any kind. Any caller
that can address a WRP message to `diag-server` through Parodus can
invoke any catalog tool. There is no equivalent of this project's FR-4
single ACL checkpoint anywhere in this code." (§2; `REQUIREMENTS.md`
§12 Risk item 6.) Everything below is a **design**, scoped to **Phase
2** by direct instruction — no code changes accompany this section,
unlike the Phase-1-relevant fixes in §2/§6 above.

### 10.1 Why this isn't a small patch to `handle_request()`

Every other fix in this plan (timeout, shell removal, program-pinning,
init-time validation) is a self-contained change inside
`diag-server-nn.c`, because the thing being fixed is entirely local to
what the process does once a request already reached it. ACL is
different: it's a question of *whether a request should have reached
diag-server at all*, and answering that means checking against the
same policy source every other toolset in this project checks against
— the ACL Policy Store (FR-4) that lives in Dispatch Core, a different
process. diag-server today has no relationship with Dispatch Core at
all; it registers with Parodus directly and is fully self-contained.
Closing this gap is therefore an architecture question first, and an
implementation question second.

### 10.2 Two ways to reach the ACL Policy Store, and the tradeoff between them

**Option A — route diagnostics traffic through Dispatch Core (recommended).**
Dispatch Core takes over diag-server's current Parodus registration
(the WRP type-9 registration, the public-facing address the cloud
sends `{"tool", "command"}` requests to). Cloud-facing behavior is
unchanged — same address, same msgpack shape, same responses — but the
request now lands at Dispatch Core first. Dispatch Core:

1. Decodes the msgpack `{tool, command}` payload into the internal
   `(toolset="diagnostics", method=<tool>, params={command})` tuple —
   this is exactly the "legacy framing adapter" already sketched as
   §8 step 3, so this design completes a step already on the roadmap
   rather than inventing a new one.
2. Runs the ACL Policy Store query (FR-4) — the same checkpoint every
   other toolset already passes through, using the caller identity
   from the WRP request's `source` field (the SAT-token/identity
   format itself is still open per `docs/22`/A1; this design consumes
   whatever that resolves to, it doesn't re-decide it).
3. On **allow**, forwards `(tool, command)` unchanged to diag-server's
   own execution engine over a local, Dispatch-Core-only transport
   (see 10.3) — diag-server's catalog lookup, `is_command_safe()`,
   `_skipped` check, timeout enforcement, and everything else in
   `handle_request()` runs exactly as it does today, untouched.
4. On **deny**, Dispatch Core builds
   `{"tool", "exit_code": <nonzero>, "stdout": "access denied"}`
   itself and never contacts diag-server at all.
5. Re-encodes diag-server's real response (or its own denial response)
   back into the frozen msgpack shape and sends it via Parodus.

The denial path in step 4 is strictly stronger than checking inside
diag-server: a denied request never reaches the execution engine, so
there's no code path inside diag-server itself that a bug could cause
to run before the ACL check fires.

**Option B — diag-server queries the ACL Policy Store itself, per
request, in place.** diag-server keeps its current direct Parodus
registration. Right before `is_command_safe()` currently runs, it adds
a query — either a live call out to Dispatch Core's Policy Store over
some local interface diag-server would need as a new dependency, or a
self-contained check against its own duplicated copy of relevant
policy (e.g. a caller-identity allowlist embedded in the catalog or a
separate config file).

The duplicated-policy variant of Option B is the one to be wary of: it
recreates, inside diag-server, the exact "second inconsistent
authorization surface" shape of risk this project has already flagged
elsewhere (the dual JSON-RPC/MCP framing concern noted under A12) —
now for a third entry point (diag-server's legacy framing) with its
own, separately-maintained copy of who's allowed to do what. It would
drift from Dispatch Core's authoritative policy the first time either
side changes without the other, and "single ACL checkpoint" (FR-4) is
a whole-project invariant, not a per-toolset one — duplicating it
anywhere weakens that invariant for every toolset, not just this one.

**Recommendation: Option A.** It reuses the existing checkpoint with
no new policy-storage surface anywhere, it's strictly stronger (denied
requests never reach diag-server), and it's the direct completion of
§8 step 3, which this plan already called out as needed regardless of
ACL. Option B is worth keeping on record as a faster, weaker interim
fallback only if the legacy framing adapter's timeline slips badly
enough that some protection is wanted before Option A can ship — not
as the permanent design.

### 10.3 Design detail, for Option A

**Caller identity.** Unchanged from how every other toolset's ACL
check already works: the WRP request's `source` field, whatever
identity format A1 settles on. `diagnostics` doesn't need, and this
design doesn't propose, a new identity model.

**ACL query shape.** `(toolset="diagnostics", method=<tool name>,
caller_identity=<source>) → ALLOW | DENY` — the same `(toolset,
method)` granularity this project's ACL model already uses elsewhere,
so diagnostics' 12 tool names (including `adhoc_diagnostic`) plug in
without a new mechanism. Whether `adhoc_diagnostic` — the one dynamic-
type tool, which accepts an arbitrary caller-supplied command — should
carry a stricter policy entry than the static tools is a policy-
authoring decision for whoever owns the ACL Policy Store's data, not
an architecture decision; flagged here so it isn't missed, not decided
here.

**Transport between Dispatch Core and diag-server.** Recommend
diag-server keep running as its own process — no rewrite of its
execution engine, catalog validation, or timeout logic — but stop
registering with the public Parodus address. It instead binds a
local-only endpoint (reusing its existing nanomsg PUSH/PULL machinery,
just pointed at Dispatch Core instead of `tcp://127.0.0.1:6666/6669`)
that only Dispatch Core connects to. From diag-server's point of view,
Dispatch Core simply becomes its one and only caller, in the exact
same `{tool, command}`/`{tool, exit_code, stdout}` shape it already
speaks — this is close to a zero-line change to `diag-server-nn.c`
itself, and is now sketched concretely in
`reference-impl/diag_legacy_framing.c` (added 2026-08-14, see §8
step 3): `diag_toolset_ipc_forward()` there is exactly this hop,
re-packing the decoded `(method, command)` back into diag-server's own
msgpack request shape and decoding its real response, rather than
inventing a converted dialect diag-server would need to learn. A side
effect worth naming: once diag-server is no longer
directly reachable from Parodus, an attacker who somehow learned its
old address can no longer reach it at all, which is a real hardening
gain beyond just "requests are now checked."

**Failure handling.**
- diag-server unreachable (process down, local-transport timeout):
  Dispatch Core returns a distinct error response, not the "access
  denied" shape — operators need to tell "backend down" apart from
  "caller denied" in logs and alerting.
- ACL Policy Store itself unreachable or erroring: fail closed (deny).
  This is an assumption, stated explicitly so it can be corrected —
  confirm it matches how FR-4 already fails elsewhere in this project
  before implementing.

**What stays exactly as-is.** diag-server's catalog format,
`is_command_safe()`, `validate_static_commands()`, timeout/kill logic,
`suppress_stderr`/`count_lines_matching`, `type`/`plane` — all
untouched. Dispatch Core's adapter forwards the already-decoded
`(tool, command)` pair through diag-server's existing local socket;
nothing about how diag-server decides whether *it itself* considers a
command safe changes. The cloud-facing wire contract (WRP types 3/9/10,
msgpack `{tool,command}`/`{tool,exit_code,stdout}`) is unchanged — only
which process Parodus's diagnostics address resolves to changes, and
that's invisible externally.

### 10.4 Staged implementation plan (Phase 2 — no code in this pass)

1. Confirm A1 (SAT token / caller-identity format) — this design
   consumes it for the ACL query's `caller_identity` input and can't
   be implemented ahead of it.
2. Build the legacy framing adapter (§8 step 3) inside Dispatch Core:
   decode/encode only, no ACL yet. **Sketched, 2026-08-14 —
   `reference-impl/diag_legacy_framing.c`**; still needs a real
   msgpack-c/nanomsg build (not the illustrative stand-ins) and the
   round-trip test below before this step is actually done. Verify
   wire-format fidelity with a round-trip test — the adapter's
   re-encoded response should byte-match what diag-server would have
   sent directly, for every current catalog tool.
3. Wire the ACL Policy Store query into the adapter's request path, at
   the `(toolset="diagnostics", method=<tool>)` granularity from 10.3.
4. Re-point diag-server's registration from the public Parodus address
   to the Dispatch-Core-only local endpoint; remove its direct WRP
   type-9 registration to the real Parodus.
5. End-to-end verification: an unauthorized caller identity is denied
   before diag-server ever sees the request (confirm via diag-server's
   own logs showing zero received requests for the denied case); an
   authorized caller's request flows through to a response
   byte-identical to today's direct-to-diag-server behavior.
6. Update `REQUIREMENTS.md` Risk item 6 to closed, and this section's
   status line, once implemented.

### 10.5 Decisions needed from you before implementation starts

- Confirm Option A over Option B (10.2), or explicitly choose B as a
  deliberate interim measure if Option A's dependency chain (the
  framing adapter, A1) is too slow for what's needed in the meantime.
- A1's caller-identity format — blocks both options, not just this one.
- Exit code convention for "access denied" responses (not yet
  standardized anywhere in this project's diag-server work; needs a
  value that doesn't collide with existing meanings like the timeout
  path's convention).
- Whether `adhoc_diagnostic` gets a stricter ACL policy entry than the
  static tools (10.3) — a policy-authoring call, not mine to make.

## 11. Toolset-lifecycle gaps — diag-server against the reference implementation's own requirements (added 2026-08-15)

Everything in §2/§6/§10 so far came from reading diag-server's own code
for bugs and security gaps. This section is a different pass: reading
this project's own reference-implementation requirements —
`openspec/specs/toolset-lifecycle/spec.md`,
`openspec/specs/capability-sync/spec.md`,
`openspec/specs/dispatch-core/spec.md` — and checking what they require
of *any* toolset that diag-server doesn't yet have, independent of
whether diag-server's own code has a bug. Findings only, by direct
instruction — nothing here is implemented yet; this is the plan to
discuss before any of it is acted on.

### 11.1 A unified local control protocol — ties F1, F3, F4, F5 together

Planning these individually (as first drafted, 2026-08-15) missed that
four of the five findings below all reduce to the same missing thing:
a channel between Dispatch Core and diag-server that isn't the
external `{tool,command}` shape. That channel already exists in
design — §10.3's local, Dispatch-Core-only endpoint
(`DIAG_LOCAL_ENDPOINT`, `reference-impl/diag_legacy_framing.c`) — today
it carries exactly one message kind, `diag_toolset_ipc_forward()`'s
tool-execution forward. None of what follows touches the external
wire, so none of it is constrained by §5's "keep the wire format
exactly as today" freeze — that freeze is specifically about
diag-server's contract with an *external* caller; this local hop
doesn't exist externally at all and was only introduced by §10's own
design in the first place.

Four message kinds on that local endpoint, extending
`diag_legacy_framing.c`/`diag-server-nn.c`'s local-socket handling:

1. **EXEC** (already designed, §10.3) — `{tool, command}` in,
   `{tool, exit_code, stdout}` out. Unchanged.
2. **DESCRIBE** (new — resolves F1) — no input required; returns the
   *currently-promoted* catalog's self-description: its version
   (F2's `_catalog_version`) and a per-tool `{name, type, plane,
   timeout}` list. This is diag-server's own `capabilities()`
   surface, reachable only by Dispatch Core over the local socket, not
   exposed to the cloud directly. §8 step 2's manifest-derivation
   logic calls this instead of reading the catalog file off disk
   directly — reading the file would violate toolset-lifecycle's "SHALL
   be authoritative from the plugin itself" the moment F3 introduces a
   candidate catalog that isn't live yet: DESCRIBE only ever answers
   from whichever catalog is currently marked live, never a candidate
   mid-validation, so there's no window where the manifest could read
   something diag-server itself isn't actually serving yet.
3. **HEALTH** (new — resolves F4) — a cheap, side-effect-free liveness
   probe, answered without walking the catalog. This replaces F4's
   originally-open "reserved tool name vs. new message shape" question
   — moving it to this local-only channel sidesteps the wire-freeze
   question entirely, since it's not a reserved *catalog* tool name at
   all, and it isn't the external shape either. Used by A8's
   on-demand-spawn readiness check. **Distinct from F3's validation
   gate below**: HEALTH answers "is the process alive," not "is this
   specific candidate catalog acceptable to promote" — related
   mechanisms, different questions, both live on this same channel.
4. **PUSH** (new — resolves F3) — **revised 2026-08-15, by direct
   instruction: carries a diff, not a full replacement catalog.**
   Original design (superseded) had `params.artifact` carry the entire
   new catalog inline; instead, PUSH now carries `{base_version,
   target_version, diff: {added: [...], removed: [...], modified: [...]}}`
   — only the tools that actually changed, each `added`/`modified`
   entry a full tool object (whole-tool replacement, not a field-level
   patch — simpler to reason about given how few fields a tool has),
   `removed` a list of tool names. `base_version` and `target_version`
   are `_catalog_version` values (F2) — see below, this is what
   resolves the version-comparison question. Still matches
   `toolset.push`'s RDM-boundary scoping (non-`dlopen()`-able content),
   just a smaller artifact than originally sketched. diag-server:
   checks `base_version` against the *currently live* catalog's own
   `_catalog_version` first — a mismatch is rejected immediately,
   without even attempting to apply the diff (see below); on a match,
   clones the live catalog into a *candidate* object, applies `added`/
   `removed`/`modified` to the clone only, sets the clone's version to
   `target_version`, and runs `validate_static_commands()` against the
   candidate in isolation (§11's F3 below has the full swap mechanics).
   Responds *synchronously* — mirroring `toolset.push`'s own
   synchronous accept/reject semantics — with either
   `{"status":"loaded","version":<target_version>}` on a successful
   promote, or `{"status":"rejected","reason":<detail>}`, leaving the
   old catalog serving unchanged either way until a promote actually
   succeeds.
5. **CHANGED** (new — resolves F5) — sent *unsolicited* by diag-server
   right after a successful PUSH-triggered promote, carrying the new
   version, so Dispatch Core's A14 shared capability-sync/notification
   trigger point fires the same way it would for any other toolset's
   reload. This is the missing notification F5 flagged as conditional
   on F3. Not a new capability to invent: diag-server's own
   `build_registration()` already sends an unsolicited message
   (the WRP type-9 registration) over its outbound PUSH channel today
   — CHANGED is the same shape of thing, over the same kind of
   channel, just a different payload and a different trigger.

**Resolved 2026-08-15 — both halves of §14 item 1, by the diff-based
PUSH design above:**

- **Version-comparison rule**: not an ordering comparison at all
  (monotonic vs. semver is moot) — a *base-version match* instead,
  compare-and-swap style. PUSH declares the exact version it's a diff
  *from*; diag-server accepts only if that matches what's actually
  live right now, otherwise rejects outright. This is a stricter,
  simpler, and safer rule than "is target_version newer": it can't be
  fooled by a stale or duplicate push (an old diff's `base_version`
  won't match a catalog that's already moved past it), and it needs no
  version-number ordering scheme decided at all — version strings
  don't need to sort, they only need to match or not.
- **Health-check pass bar**: also resolved as a direct consequence,
  not separately decided — since the diff makes the scope of change
  explicit and small, "passes" now means *no tool in the diff's
  `added`/`modified` set ends up `_skipped`* after
  `validate_static_commands()` runs against the candidate. Tools
  untouched by the diff aren't re-litigated (they already validated
  clean when they first entered the catalog); only what the diff
  actually changed has to justify itself.

**F1 — No self-described schema (`list()`/`schema()`/`version()`/`capabilities()`).**
Toolset-lifecycle's "Self-described schema" requirement: this
information "SHALL be authoritative from the plugin itself, not
duplicated as a separately maintained copy elsewhere." diag-server has
no WRP-queryable equivalent of what the Triage Toolset already has via
`triage.capabilities` (`triage_capabilities.c`) — its tool list lives
only in `diag-triage-catalog.json`, a file nothing queries live.
**Resolved by 11.1's DESCRIBE message above**: §8 step 2's
manifest-derivation logic queries diag-server's own live process
instead of reading the catalog file directly, closing the "duplicated
copy" risk the original requirement warns about — including the
specific drift window a naive file-read would have during F3's
candidate-validation window, which DESCRIBE avoids by construction.

**F2 — No version anywhere.** Neither the catalog file nor the running
process reports a version to anything — no per-tool `version` field
(unlike `plugin_descriptor_t.version`, already present for
Triage-style plugins), no whole-catalog version. Needed for two
things this project already committed to: capability-sync (F5 below)
needs to know a tool list *changed*, and `toolset.push`'s
health-check-gated swap (A15) needs to know *which* version is
currently loaded to tell a no-op push from a real update. **Proposed
extension, in diag-server's own catalog**: a top-level
`"_catalog_version"` field in `diag-triage-catalog.json`, bumped by
whoever authors a catalog change. Small, additive, no wire-format
impact — catalog format isn't part of the frozen external contract
(§3). **No longer just "read and logged"** — 11.1's DESCRIBE reports
it live, and 11.1's revised PUSH design (2026-08-15) carries both
`base_version` and `target_version` explicitly, checked against the
currently-live catalog's own `_catalog_version` as a compare-and-swap
precondition, not an ordering comparison — a mismatch is rejected
before the diff is even applied, so an accidental duplicate or stale
push can't get silently re-promoted. **Resolved, 2026-08-15** (was
open as of this finding's original writeup): see 11.1's PUSH entry for
the full mechanics — no monotonic-vs-semver decision needed at all,
version strings only need to match or not.

**F3 — No reload without a full process restart.** Toolset-lifecycle's
"Lifecycle management without full restart" requirement is explicit:
load/unload/reload SHALL work without a device restart. diag-server's
`main()` calls `load_catalog()` + `validate_static_commands()` exactly
once, before the socket exists (deliberately, for the init-time-
validation performance reason already designed — §8 step 5 / this
plan's earlier work). If `diagnostics` is later updated via
`toolset.push` — and A15 explicitly names diag-server's process as
eligible for exactly that — there is currently no code path to pick up
a new catalog without killing and restarting the whole process.

**Corrected 2026-08-15 — the reference implementation does already
design this, at the decision level, and it's a stronger model than a
plain validate-then-swap reload.** Two OpenSpec change design docs
cover it directly: `define-synchronous-toolset-push/design.md`'s
"Phase 1 exception — original health-check-gated rollback applies
again, but only there" (added 2026-08-14), and
`add-phase1-command-execution-exception/design.md`'s matching "a
Phase-1-specific rollback model for `toolset.push`". Both say the same
thing: because Phase 1's command-executing toolsets run **in-process**
(A15's exception to `define-plane-vs-toolset-model`'s Decision B) —
which is exactly diag-server's situation — the general on-demand model
("keep the prior *artifact* as a fallback, spawn it fresh on next
demand") doesn't apply, since there's no separate process to spawn.
Phase 1 instead uses the *original*, un-amended design: **the prior
version stays loaded and serving until the new version passes its own
health check, then whoever owns the swap promotes the new version —
never a stop-the-world replace, and never a promotion without a
passing health check first.** `reference-impl/` has no C sketch of
this yet (only comments referencing `toolset.push` in
`toolset_resolution.c`, and that file's `manifest_repair_async()` is a
different, narrower mechanism — stale-manifest repair, not a
push/rollback swap) — so the design exists, the code doesn't, in
either `reference-impl/` or `diag-server-nn.c`.

**What this changes about the proposed extension:** my original
SIGHUP-and-swap sketch above was a real simplification of this —
"validate then swap" is close, but it isn't "keep the old version
*actively serving* while the new one is validated," and it has no
notion of a distinct pass/fail health check separate from parse
success. The bigger architectural point: the general model's "Plugin
Manager swaps which version handles calls" assumes Plugin Manager can
act on a process it manages — but diag-server has no second process
for anything external to swap at Phase 1 (per A15, "ensure reachable"
is a no-op precisely because there's no separate process to spawn).
So diag-server has to implement this pattern **on itself, internally**,
not rely on an external swap: hold the currently-serving catalog
object untouched and answering every in-flight and new request as
normal; parse a candidate new catalog into a *separate* object and run
`validate_static_commands()` against it in isolation, without touching
what's live; only if that passes (and "passes" needs its own bar
decided — not just "did it parse," since every real catalog tool
already parses clean today; possibly zero unexpectedly-skipped tools,
or another explicit signal) atomically swap a pointer so new requests
use the new catalog, while any request already in flight against the
old one finishes against it unaffected; free the old catalog only
after that drains. A failed validation means diag-server keeps serving
unchanged from the old catalog and reports the push as rejected — the
"revert" half of "promote or revert," here without ever having gone
offline to begin with, since the old catalog was never taken down.
This is a genuinely bigger piece of work than the original sketch —
flagging that difference explicitly rather than quietly keeping the
smaller version.

**Superseding the SIGHUP idea entirely, 2026-08-15**: 11.1's PUSH
message is the actual trigger, not a signal. `SIGHUP` had no way to
carry the new catalog's *content* — it can only tell a process "go
re-read something," which begs the question of where the new catalog
comes from and how its arrival is itself authenticated/authorized.
PUSH arrives over the same local, Dispatch-Core-only channel every
other local message uses, meaning whatever already authenticated and
ACL-checked the original `toolset.push` JSON-RPC call at Dispatch Core
(FR-4, §10) is what gets diag-server the new catalog in the first
place — diag-server never needs to independently trust a signal from
an arbitrary local sender the way a bare `SIGHUP` handler would have
to. The validate-in-isolation/atomic-pointer-swap/drain-old-in-flight
mechanics above are unchanged; only the trigger changed, from a signal
to a proper request/response message with a real payload and a
synchronous accept/reject reply.

**Revised again, 2026-08-15, by direct instruction — PUSH carries a
diff, not a full catalog.** The candidate object above is no longer
built by parsing an entirely new catalog wholesale; it's built by
*cloning the currently-live catalog* and applying PUSH's `added`/
`removed`/`modified` diff to the clone only (11.1's PUSH entry has the
full message shape). Two direct consequences: first, PUSH's
`base_version` has to match the live catalog's `_catalog_version`
before the diff is even attempted — a stale or duplicate push is
rejected on that mismatch alone, resolving the version-comparison
question as a compare-and-swap precondition rather than an ordering
rule (11.1, F2 above). Second, the "passes" bar this section originally
left open is now well-defined: since the diff names exactly which
tools changed, "passes" means no tool in the diff's `added`/`modified`
set ends up `_skipped` after `validate_static_commands()` runs against
the candidate — tools the diff didn't touch were already valid in the
live catalog and aren't re-checked. Both halves of §14 item 1 are
resolved by this one design change, not two separate decisions.

**F4 — No health/readiness signal distinct from actually running a
diagnostic tool.** Ties to A8's on-demand-spawn health check and to
§10.3's "diag-server unreachable" failure path, which today can only
be detected by a failed request — there's no cheap, side-effect-free
way for Dispatch Core to ask "are you up" without that ask being (or
looking like) a real diagnostic invocation. **Resolved by 11.1's
HEALTH message above**, superseding the original "reserved tool name
vs. new wire shape" framing entirely: HEALTH lives on the local-only
channel, not the catalog and not the external `{tool,command}` shape,
so it never touches §5's frozen wire contract at all — there was no
real tension to trade off once the question was reframed as "which
channel," not "which shape within the existing channel."

**F5 — No participation in capability-sync's event-triggered push.**
Capability-sync's requirement: report to the cloud "when Plugin
Manager loads, unloads, or reloads a toolset" — push, not poll.
diag-server's only registration today is Parodus's WRP type-9
`SVC_REGISTRATION`, which is a transport-level "I exist" handshake, not
a capability announcement Plugin Manager or capability-sync currently
listens to. **Resolved by 11.1's CHANGED message above**: diag-server
sends it unsolicited immediately after PUSH promotes a new catalog,
giving Dispatch Core's A14 shared trigger point the same event any
other toolset's reload would produce. The "conditional on F3
notifying" caveat this finding originally carried is closed by
construction now that F3's mechanism (PUSH) and F5's notification
(CHANGED) are two steps of the same one flow, not two separately-built
pieces that could get built out of sync with each other.

**F6 — Not plugged into RDM Client's verified-install/rollback
pipeline; no install-time manifest.** Toolset-lifecycle's "Manifest-
declared requirements" and "Verified install with rollback"
requirements assume a toolset arrives through RDM Client's install
path with a signed manifest declaring capabilities/device-node access.
diag-server is a real, already-built, separately-buildable service —
it doesn't go through that pipeline today, and folding it in is a
packaging/deployment question tied to §9 open question 4 (source
location), not a code gap inside `diag-server-nn.c`. **Recommendation:
leave this as Phase 2**, consistent with §7's existing statement that
sandboxing/hardening/independent review stay Phase 2 for `diagnostics`
like every other Phase 1 toolset — this finding doesn't ask for an
earlier exception unless you want to pull it forward.

### 11.2 Staged implementation plan (planning only — no code in this pass)

Ordered by dependency, not by finding number — several of F1/F4/F5
have no independent work of their own once F2/F3 exist, so building in
finding order would mean redoing pieces of F3 three separate times.

1. **F2's version field** — additive, no dependencies, do first: add
   `"_catalog_version"` to `diag-triage-catalog.json`, have diag-server
   read it into the in-memory catalog object at load (same object
   `validate_static_commands()` already annotates with `_program_token`/
   `_skipped`). Nothing queries it yet; this just makes it exist.
2. **F3's core mechanism** — the load-bearing piece everything else
   sits on: candidate-catalog parse, isolated `validate_static_commands()`
   run against the candidate (never touching the live catalog),
   atomic pointer swap on pass, in-flight-drain-then-free of the old
   object, reject-and-keep-serving on fail. Built and tested
   standalone first, before it's wired to any external trigger —
   this is the piece worth the most scrutiny (concurrency correctness:
   what "in-flight" means for the detached-pthread-per-request model
   `handle_request()` already uses, and how the old catalog's
   lifetime is tracked so it's freed only once nothing still holds a
   pointer to it).
3. **Local protocol wiring (11.1)** — once step 2 works standalone,
   wire it to PUSH (the external trigger), DESCRIBE (read-only, reads
   whichever catalog is currently live), HEALTH (independent of the
   other three — can actually be built any time after step 1, listed
   here only because it shares the same local-socket plumbing), and
   CHANGED (the one side effect of a successful PUSH promote, sent
   immediately after step 2 swaps the pointer).
4. **Verification**: extend `test_init_validation.c`'s style of
   harness (real catalog file, real functions copy-pasted verbatim,
   live injection of bad entries) to cover the new surface — a bad
   PUSH candidate must leave the old catalog serving unchanged and
   in-flight requests against it undisturbed; a good PUSH candidate
   must fully replace what DESCRIBE reports and what EXEC actually
   runs, with no window where the two disagree.
5. **F6 stays out of this sequence entirely** — Phase 2 packaging
   work, not something step 1–4 above unblocks or depends on.

### Summary — what's actually diag-server's own code to extend, vs. what
falls out of finishing already-planned work elsewhere

| Finding | Where the work actually is |
|---|---|
| F1 self-description | **Resolved via 11.1's DESCRIBE** — new diag-server code (local protocol), not just a manifest-layer question anymore |
| F2 versioning | diag-server's catalog (`diag-triage-catalog.json`) + one read at load — step 1 of 11.2, smallest piece |
| F3 hot-reload | `diag-server-nn.c` itself — the largest real code gap in this list. Health-check-gated, keep-old-serving-until-new-passes model (matching `define-synchronous-toolset-push/design.md` and `add-phase1-command-execution-exception/design.md`'s Phase 1 exception), triggered by 11.1's PUSH message, not a signal — step 2 of 11.2 |
| F4 health-check | **Resolved via 11.1's HEALTH** — local-only channel, never touches the frozen external wire format at all |
| F5 capability-sync push | **Resolved via 11.1's CHANGED** — fires as the direct, guaranteed side effect of F3's promote step, not a separately-built, possibly-out-of-sync piece |
| F6 install/rollback pipeline | Phase 2 packaging concern, not diag-server code — explicitly excluded from 11.2's sequence |

Nothing above is implemented. This is the plan to discuss before any
of F1–F6, or 11.1's protocol, or 11.2's sequence, gets acted on.

## 12. Consolidated merge plan — Dispatch Core ↔ diag-server, one sequence (added 2026-08-15)

§5, §8, §10, and §11 each designed a real piece of this merge, but
each has its own staged plan (§8's 7 steps, §10.4's 6 steps, §11.2's 5
steps), written somewhat independently and covering overlapping
ground from different angles. This section is the single, ordered,
cross-component sequence — which side (Dispatch Core's `reference-impl/`
vs. diag-server's own code) does what, and in what order — for
actually merging the two, pointing back to the relevant section for
detail rather than re-explaining it.

### 12.1 End state, in one paragraph

Dispatch Core owns the public Parodus registration diag-server uses
today; diag-server keeps running as its own process, unmodified in its
core execution engine, reachable only over a new local, Dispatch-Core-
only endpoint. Every external request — cloud caller sending the
legacy msgpack `{tool,command}` shape, or (once §8 step 2 finishes) an
MCP `tools/call`/JSON-RPC caller — decodes to the same internal
`(toolset="diagnostics", method, params)` tuple, passes through
Dispatch Core's one ACL checkpoint (FR-4), and only then reaches
diag-server over the local protocol (§11.1: EXEC, DESCRIBE, HEALTH,
PUSH, CHANGED). `diag-server-nn.c` gains exactly two things it doesn't
have today: the local-protocol handlers, and F3's health-check-gated
catalog swap underneath PUSH. Everything else about it — catalog
format, `is_command_safe()`, timeout/kill logic, `validate_static_commands()`
— is unchanged.

### 12.2 Build sequence

**Phase A — decisions, no code, can happen immediately (blocks later phases if left open):**
1. ~~The "passes" bar for F3's candidate-catalog health check, and the
   version-comparison rule for F2/CHANGED~~ **Resolved 2026-08-15** —
   PUSH now carries a diff (`base_version`/`target_version`/
   `added`/`removed`/`modified`), not a full catalog: version
   comparison became a `base_version` match against the live catalog
   (compare-and-swap, not ordering), and the pass bar became "no tool
   in the diff's `added`/`modified` set ends up `_skipped`." See
   §11.1's PUSH entry and F2/F3 above for the full mechanics. No longer
   blocks B1.
2. ~~Exit-code convention for ACL "access denied" responses~~ (§10.5).
   **Resolved 2026-08-15: `exit_code = 126`** — see §14 item 2.
3. ~~Whether `adhoc_diagnostic` gets a stricter ACL policy entry~~
   (§10.5) — **Resolved 2026-08-15, see §14 item 3**: yes, plus
   mandatory encryption and a hash/signature, mirroring
   `toolset.push`'s existing encryption+signing precedent — carried by
   restricting the tool to MCP/JSON-RPC entry only, never the legacy
   msgpack shape, so no change to the frozen wire format was needed.
   One follow-on detail remains: whatever enforces the framing
   restriction needs to know which entry path a request arrived
   through, not just its ACL grant (§14 item 3's last paragraph).

**Moved 2026-08-15**: A1 (SAT token / caller-identity format) is no
longer in this near-term list — it's Phase 2 (see Phase E.3 below),
consistent with §7/§10's existing scoping of the whole ACL gap as
Phase 2, not something that needs to be true before `diagnostics` can
run under the A15 in-process exception. It no longer blocks any of
Phase A/B here; it blocks Phase C.3 specifically, which is itself
Phase 2 work as a direct consequence — see the note there.

**Phase B — diag-server's own code, self-contained, doesn't need Dispatch Core to exist first:**
1. F2's version field (§11.2 step 1) — do first, trivial, no
   dependencies beyond nothing.
2. F3's core swap mechanism (§11.2 step 2) — candidate parse, isolated
   `validate_static_commands()` run, atomic pointer swap, in-flight
   drain. Built and tested standalone, no local-protocol wiring yet.
3. Local protocol handlers on diag-server's side (§11.1, §11.2 step 3):
   EXEC (already designed, §10.3), DESCRIBE, HEALTH, PUSH (calls B2),
   CHANGED (fires after B2 promotes).
4. Re-point diag-server's registration from the public Parodus address
   to the new local endpoint (§10.3) — the literal point at which
   diag-server stops being directly reachable from Parodus. Held until
   Phase D's tests pass; diag-server can keep running standalone,
   registered with the real Parodus exactly as today, at any point
   before this step.

**Phase C — Dispatch Core's `reference-impl/` side:**
1. Finish toolset manifest derivation for `diagnostics` (§8 step 2,
   partially done — `type`/`plane` already in the catalog). Needed for
   F1/F5 to actually surface externally and for MCP/JSON-RPC
   reachability (§5/§9 question 2's follow-on).
2. Take `reference-impl/diag_legacy_framing.c` (§8 step 3, sketched
   2026-08-14) from illustrative stand-ins to a real build against
   msgpack-c/nanomsg.
3. Wire the ACL Policy Store query into that path (§10.4 step 3) —
   **Phase 2 work, not Phase 1**, per §7/§10's existing scoping;
   depends on Phase E.3's A1 resolution below, not on anything in
   Phase A. C.1, C.2, C.4, and C.5's non-ACL parts are unaffected and
   can proceed without this step being done first.
4. Implement the one-line dialect dispatch inside the real (not yet
   written) generic `toolset_ipc_forward()` — `if (toolset ==
   "diagnostics") return diag_toolset_ipc_forward(...)` — per §10.3's
   note that `dispatcher_command_path.c` itself needs zero edits.
5. Wire Dispatch Core's manifest-derivation (C.1), on-demand-spawn
   health check (A8), and `toolset.push` handling to call B.3's
   DESCRIBE/HEALTH/PUSH respectively, and to receive CHANGED.

**Phase D — integration tests — planned 2026-08-16, not yet started**

Superseded framing note: D.1's original wording below ("adapter's
re-encoded response") assumed the pre-§13 two-process split (a separate
Dispatch Core adapter re-encoding diag-server's replies). §13 retired
that split — diag-server is its own front door now, so there is no
adapter to compare against. D.1 is reframed below as a *regression*
test: does every catalog tool still behave identically after all of
B.1–B.5, §15 B.4, and §13.4's changes, compared to diag-server's
pre-merge-work behavior. Same underlying goal (nothing silently
changed), different mechanism now that the architecture moved.

**Sandbox constraint, checked 2026-08-16**: this environment has no
root/package-install access (`apt-get install` needs `sudo`, which is
blocked here) — confirmed by trying, not assumed. Every phase's
verification so far (B.1–B.5, §15 B.4, §13.4) has used hand-written
stub headers or minimal-but-functional reimplementations of
nanomsg/msgpack/cJSON, not the real libraries, for exactly this reason.
Phase D inherits the same limitation: true wire-fidelity against a real
Parodus instance, and a real link of `diag-server-nn.c` against the
genuine libraries, need the actual target build environment (or a CI
environment with package-install access), not this sandbox. What
follows is scoped to what's actually achievable here — the same
harness-based rigor already used throughout this project — with the
real-toolchain gap called out explicitly rather than silently assumed
closed.

1. **D.1 — Wire-fidelity regression, reframed — Implemented 2026-08-16.**
   For every catalog tool across all four planes, confirm the current
   code path (decode → `diag_acl_check()` → `catalog_lookup()` →
   `run_command()` → encode) still produces the same
   `{tool, exit_code, stdout}` shape and same catalog-driven behavior
   (timeout, safety checks, static-vs-dynamic override handling) as
   before this project's changes. Built as one consolidated harness
   (`/tmp/d1test/harness_d1.c`, scratch-only), copying
   `load_catalogs()` through `handle_request()` verbatim, linked
   against genuinely functional `msgpack_impl.c`/`cjson_impl.c` (real
   wire bytes, not typed stubs), driven against the real
   `diag-triage-catalog.json` (all 12 tools) plus synthetic
   management/control/config-apply fixtures (those three files don't
   exist in the repo yet). Only `run_command()` (recorder, not a real
   fork/exec — the real triage catalog's commands are genuine device
   diagnostics unsafe/meaningless to execute here) and
   `acl_policy_store_query()` (mock, same reason as every prior phase)
   were replaced; every response was decoded back through the real
   `decode_wrp()`/msgpack unpacker for a genuine round trip, not just
   input interception. **85/85 checks passed** — but two of those
   checks exist specifically to pin down two real findings surfaced
   during this pass, not to celebrate a clean run. Both are pre-existing
   behavior in `handle_request()`/`validate_static_commands()`, not
   something introduced by B.1–B.5/§15 B.4/§13.4 — D.1 is simply the
   first pass that exercised these particular paths.

   **FINDING 1 (serious — arbitrary command execution bypass) — Fixed
   2026-08-16, by direct instruction.** In
   `handle_request()`, when `catalog_lookup()` returns `NULL` — because
   the tool name plainly doesn't exist in any catalog, *or* because
   it's ambiguous across planes (§15 B.5's no-plane collision case) —
   the `if (!entry) { syslog(...); }` branch never clears `cmd`. Every
   safety check (`is_command_safe()`, and therefore `is_blocked()`) is
   only ever invoked inside the `else` branch, which requires `entry`
   to be non-NULL. So `if (cmd && *cmd)` further down reaches
   `run_command()` with the caller's raw override, completely
   unchecked, for *any* request naming a tool `catalog_lookup()` can't
   resolve. Confirmed two ways in the harness: a plainly nonexistent
   tool name with `"command":"rm -rf /tmp/proof-of-bypass"` dispatched
   verbatim; and the same bypass via `device_uptime`'s ambiguous
   cross-plane collision with `"command":"reboot"` — both blocklisted
   commands, both dispatched with zero checking. This is more severe
   than either of the two already-tracked open items (the ACL gate,
   §13.4; static-override removal, §9 Q3) — it requires no valid tool
   name, no catalog entry, and no ACL bypass, only a `tool` field
   `catalog_lookup()` fails to resolve plus any `command` field.
   Reported to the project owner before touching code, per this
   project's standing practice for security-relevant,
   execution-path-changing findings — confirmed fix-now.

   **Fix**: `handle_request()`'s `if (!entry) { syslog(...); }` branch
   now also does `free(cmd); cmd = NULL;` — the identical pattern the
   adjacent `is_skipped` branch already used, for the identical reason
   (an override must never reach `run_command()` for a tool this
   function has no safety-checked catalog entry for). No behavior
   change for any tool that *does* resolve. Verified: full-file
   `gcc -fsyntax-only` clean (same two pre-existing cosmetic warnings,
   nothing new); the D.1 harness's two bypass-proof cases were flipped
   to assert the bypass is now closed (`run_command` NOT called,
   response correctly reports "tool not in catalog") and re-run
   alongside all other D.1 cases — **85/85 checks still pass**, no
   regression in any tool's dispatch behavior.

   **FINDING 2 (narrower — validation gap, same root cause class) —
   Fixed 2026-08-16, by direct instruction.**
   `is_blocked()` compares a resolved command's first argv token
   against `BLOCKED_CMDS` by exact string match ("rm", "reboot", "dd",
   etc.). A full path to the same binary (e.g. `"/bin/rm -rf /"`) does
   not match and is *not* caught — neither by
   `validate_static_commands()` at startup (the tool is never marked
   `_skipped`) nor by `is_command_safe()` at request time for an
   override. Confirmed with a synthetic `config-apply` catalog entry:
   `"/bin/rm -rf /"` (full path) loads and runs normally; `"rm -rf /"`
   (bare name, otherwise identical) correctly gets skipped at startup
   exactly as designed. The real `diag-triage-catalog.json` doesn't
   currently trip this (its commands are a mix of bare names like
   `cat` and full paths like `/bin/ping`/`/sbin/ifconfig`/`/bin/ps`/
   `/bin/dmesg`, none of which happen to collide with a blocked name
   either way), so this hasn't caused a real incident — but it was a
   real gap in the safety net for any future catalog entry that
   happened to write a full path to a blocked binary.

   **Fix**: `is_blocked()` now also compares the command's *basename*
   (the part after the last `/`, if any) against `BLOCKED_CMDS`, not
   just the raw first token — `"/bin/rm -rf /"`'s basename `"rm"` now
   matches the same way the bare `"rm -rf /"` always did. A bare name
   is unaffected (no `/` means basename == first token, identical to
   the pre-fix check). No false positives against the real triage
   catalog's own full-path commands (`/bin/ping`, `/sbin/ifconfig`,
   `/bin/netstat`, `/bin/ps`, `/bin/dmesg` — none of their basenames
   collide with `BLOCKED_CMDS`), confirmed by the D.1 harness's
   unchanged, still-passing per-tool dispatch checks for every other
   triage tool. Verified: full-file `gcc -fsyntax-only` clean (same two
   pre-existing cosmetic warnings, nothing new); the D.1 harness's
   Finding 2 cases were flipped to assert the gap is now closed
   (`unsafe_marked` — full path — now gets `_skipped` at startup and is
   rejected at request time exactly like the bare-name case always
   was) and re-run alongside every other D.1 case — **86/86 checks
   pass**, no regression.

   Not achievable here: byte-level wire-format fidelity against a real
   running Parodus — needs the target environment (see the sandbox
   constraint noted above).
2. **D.2 — ACL denial.** An unauthorized caller is denied before
   reaching catalog lookup/execution — confirm via `diag_acl_check()`
   returning false and the exact `{"tool","exit_code":126,"stdout":"access
   denied"}` response, with zero calls into `catalog_lookup()`/
   `run_command()` for the denied case. **Still Phase 2, gated on a
   real `acl_policy_store_query()` implementation existing** (§14 item
   4) — the *call-site* behavior (§13.4) is already covered by the
   9-check ACL portion of `/tmp/b5test/harness5.c`'s existing
   allow/deny cases; a further Phase-D-specific pass only adds value
   once there's a real policy store to query against instead of a
   mock. D.1, D.3, and D.4 don't depend on this and can proceed
   without it.
3. **D.3 — Push/reload — Implemented 2026-08-16.** Test matrix: (a) a
   bad diff (a modified entry that fails `validate_catalog_tools()`) →
   `PUSH_ERR_VALIDATION_FAILED`, live catalog and version unchanged, a
   subsequent `EXEC` still dispatches the pre-push command, undisturbed;
   (b) a stale `base_version` → `PUSH_ERR_VERSION_MISMATCH`, no promote
   attempted; (c) a good diff → `PUSH_OK`, `DESCRIBE` immediately
   reflects the new tool set (including an untouched tool the diff
   never mentioned, still correctly present), a subsequent `EXEC` on
   the newly-added tool runs its new command, `CHANGED` fires exactly
   once on the local socket, and `capability_sync.updated` fires
   exactly once too, decoded and checked for its real JSON shape
   (`jsonrpc`/`method`/`params.version` as a string/`params.capabilities`
   listing all three live tools), over the public socket; (d) PUSH via
   the public path is still rejected regardless of content, confirming
   §15 B.4's transport restriction holds inside this fuller scenario
   too. Built as `/tmp/d3test/harness_d3.c` (scratch-only), extending
   D.1's verbatim-copy pattern with `catalog_apply_push()`,
   `handle_push_request()`, `decode_push_request()`/
   `msgpack_obj_to_cjson()`, `build_describe_response_payload()`, and
   §13.4's notification functions, all copied verbatim. Unlike D.1
   (which mocked `run_command()` since the real triage catalog's
   commands are genuine device diagnostics), `catalog_apply_push()`'s
   disk persistence ran for real against a scratch catalog file this
   harness owns — the fopen/fwrite/fsync/rename sequence genuinely
   executed, and a promoted catalog's presence on disk (not just in
   memory) was independently confirmed by re-reading the file after the
   push, not assumed from the in-memory state alone. **37/37 checks
   passed, no findings this pass** — two harness bugs were caught and
   fixed during development (an off-by-one string-length check in the
   harness's own `DESCRIBE` parser, and a miscounted expectation that
   forgot a pre-existing, diff-untouched tool stays in the catalog),
   both harness-side, confirmed by inspection before being written off
   as such — same "confirm before dismissing" discipline as every
   prior phase's harness-bug findings.

   **Deliberately scoped as sequential, not a second concurrency
   proof.** D.3 reuses B.2's already-TSan-verified swap logic (200
   concurrent pushes against 8 concurrent readers, zero races, `/tmp/b2test`)
   rather than re-running a concurrency pass — its own job, per the
   original plan text, is wiring the swap/notify pieces into one
   continuous functional scenario per catalog file, not re-proving
   thread safety already proven separately. "An EXEC in flight...
   completes undisturbed" is demonstrated here as "an EXEC dispatched
   immediately before and after a push sees the correct, respective
   catalog state at each point," which exercises the same
   extract-under-mutex-then-release discipline B.2's design relies on,
   without spinning up real concurrent threads a second time.
4. **D.4 — Flip §15 B.4 part 2 — Implemented 2026-08-16, then reverted
   the same day by direct instruction.** D.1 and D.3 both passed,
   unblocking the gate, and `REGISTER_WITH_PARODUS` was set to 0,
   gating the registration-send call site in `main()`. **Reverted
   2026-08-16**: registration is needed at diag-server startup again,
   so `REGISTER_WITH_PARODUS` is back to 1 and diag-server registers
   with Parodus directly, as it did before this step. `build_registration()`
   and the send logic were never removed either way — only the
   `#define` changed, both times. **Re-opened by this revert**: the
   §10.2 Option A goal this flip was working toward (all diagnostics
   traffic passing through Dispatch Core's ACL checkpoint before
   reaching diag-server) no longer holds — diag-server is directly
   reachable from Parodus again, and `acl_policy_store_query()` still
   has no implementation anywhere (Phase 2, transport unresolved), so
   there is no working ACL enforcement on that public path right now.
   The public PULL/PUSH socket pair was never unbound/disconnected by
   either state of this flag — outbound traffic (`diag_notify_capability_sync()`
   over `g_push_sock`) was unaffected throughout, since it doesn't
   depend on inbound registration at all. With registration back on
   (current state), Parodus again has a route to `CLIENT_URL`, so
   diag-server is reachable on both the public and local pairs — "the
   actual point of no return" was momentarily crossed, then walked
   back the same day.

   **Verification (as of the original 2026-08-16 flip to 0, still
   valid for the flag mechanism itself)**: full-file `gcc -fsyntax-only`
   clean (same two pre-existing cosmetic warnings, nothing new).
   Functional: an isolated check mirroring the exact gated block
   confirmed `REGISTER_WITH_PARODUS=0` never calls the registration
   send and `=1` always does — a deliberately lightweight check
   matching this change's actual risk level (a single compile-time
   constant gating
   one existing call site, not new business logic), rather than
   rebuilding the full harness infrastructure used for higher-risk
   pieces earlier in this project.

**Phase E — formalize (after the above is built and tested, not before):**
1. ~~Resolve §9's remaining open questions: 3 (override remove-vs-
   restrict, permanent model beyond program-pinning), 4 (source
   location), 5 (requirements numbering).~~ **Done, 2026-08-15** — all
   three resolved, see §14 items 7–9. This step's own precondition for
   step 2 is now met.
2. ~~Draft the OpenSpec change (§8 step 6) —
   `openspec/changes/merge-diag-server-as-diagnostics-toolset/` — now
   that the design spans §5/§6/§8/§10/§11 in enough concrete detail to
   write real proposal/design/tasks documents from, rather than
   drafting against an still-partial picture.~~ **Drafted 2026-08-16**:
   `proposal.md`, `design.md`, `tasks.md`, plus spec deltas for
   `toolset-lifecycle` (registers `diagnostics` as the named instance
   of the Phase 1 in-process exception, the static/dynamic override
   model, and the permanent legacy-msgpack-framing decision) and
   `dispatch-core` (a new, explicit, tracked, time-bounded exception
   for diag-server's interim self-hosted ACL checkpoint, since no real
   Dispatch Core process fronts it yet — see below). No delta needed
   for `capability-sync`/`acl-policy-store`: diag-server is a new
   caller of already-generic mechanisms there, not new behavior.
   **One genuine gap surfaced while drafting, not previously
   reconciled anywhere in this doc**: `dispatch-core/spec.md`'s
   "Single ACL checkpoint" requirement says access control is enforced
   "exactly once, in Dispatch Core... no other component... SHALL make
   an independent access-control decision." §13.4's `diag_acl_check()`
   runs inside diag-server itself, and there is currently no live
   Dispatch Core process fronting diagnostics traffic at all (B.4 part
   2 disabled the public registration; the local endpoint has no real
   consumer yet) — so, read strictly, diag-server is both "a toolset
   plugin" and "the only component deciding access," which the spec
   says shouldn't happen. Not silently glossed over: the new
   `dispatch-core` delta names this as a scoped, tracked, time-bounded
   exception (mirroring the pattern `add-phase1-command-execution-exception`'s
   `sandboxed-runtime` delta already used for containment), with an
   explicit retirement condition (a real Dispatch Core process actually
   forwarding `diagnostics` traffic). Not resolved by writing the
   exception down — resolved once that process exists.
3. **Phase 2 items**, explicitly after all of the above, per §7's
   existing Phase 1/2 boundary:
   - **A1 (SAT token / caller-identity format)** — moved here
     2026-08-15. Confirm `openspec/changes/define-sat-token-format/`
     (JWT, 5-minute expiry + refresh, no revocation list — currently
     "drafted, unarchived"). Unblocks Phase C.3 (ACL query wiring) and
     Phase D.2 (ACL denial test) once resolved.
   - F6 (RDM verified-install/rollback pipeline).

### 12.3 Status roll-up, as of 2026-08-15

| Piece | Status |
|---|---|
| Security fixes (timeout, shell removal, override safety, static-type gating, init-time validation) | **Done** — live in `diag-server-nn.c` |
| ACL design (§10) | Designed, Option A recommended; §13 reassigns it into diag-server's own deliverable; `diag_acl_check()` call site **implemented 2026-08-15** (§13.4) — code complete, but won't link until `acl_policy_store_query()`'s transport (Phase 2) exists |
| Legacy framing adapter (`diag_legacy_framing.c`) | Sketched (illustrative stand-ins), not a real build |
| Toolset manifest derivation (§8 step 2) | Partially done (`type`/`plane` in catalog); full conversion pending |
| Local control protocol (§11.1) | Designed, not built on either side |
| F3's catalog swap mechanism | Designed in detail (§11), not built |
| §9 open questions 1, 2 | Resolved (separate toolset; permanent msgpack framing) |
| §9 open questions 3, 4, 5 | **Update, 2026-08-16: all three now resolved, see §14 items 7-9. Item 3 (override model) additionally implemented in code, not just decided — see §14 item 7.** |
| OpenSpec change draft | **Update, 2026-08-16: drafted** — `openspec/changes/merge-diag-server-as-diagnostics-toolset/` (proposal.md, design.md, tasks.md, deltas for toolset-lifecycle/dispatch-core); see §12.2 Phase E step 2 for the full writeup including a genuine gap this drafting surfaced (the ACL-checkpoint deviation) |

Nothing in this section is new design — it's the existing design from
§5/§6/§8/§10/§11, reordered into one buildable sequence. Phase A's
three decisions are the only remaining blockers to starting Phase B,
which has no dependency on Dispatch Core at all and could start first.
A1 no longer sits in that near-term list — moved 2026-08-15 to Phase
E.3 as Phase 2 work, alongside the rest of the ACL gap's existing
Phase 2 scoping (§7/§10). Phase C's non-ACL steps (C.1, C.2, C.4, C.5)
and Phase D's non-ACL tests (D.1, D.3, D.4) are unaffected.

## 13. Ownership reassignment — Phase C moves into diag-server's own deliverable (added 2026-08-15)

Direct instruction: move all of Phase C's work (previously "Dispatch
Core's `reference-impl/` side") into diag-server's own deliverable —
build and ship it as part of `external/diag-server/`, not as a
separately-owned Dispatch Core component — **without changing
diag-server's core logic**, and **ask before any change to diag-server
that this reassignment turns out to require**, rather than deciding
that unilaterally. This section does the reassignment and the
classification; it does not, by itself, authorize touching
`diag-server-nn.c` — see the question at the end.

**"Core logic," precisely, for this purpose**: `load_catalog()`,
`is_command_safe()`, `tokenize_argv()`/`free_argv()`, `run_command()`
(fork/pipe/timeout/`execvp()`), `validate_static_commands()`, and
`handle_request()`'s existing internal decisions (skip-check, override
safety, catalog lookup) — the security-hardened execution engine built
and verified across §2/§6 of this plan. None of this is touched by
anything below.

### 13.1 What collapses away entirely under this reassignment

Two of Phase C's five steps stop being separate work, not because
they're deprioritized, but because the reason they existed at all was
the two-process split this reassignment removes:

- **C.2 (build `diag_legacy_framing.c` for real)** — that file's whole
  job was translating between diag-server's native msgpack shape and a
  *different* process's internal JSON-RPC representation. If
  diag-server is the front door itself, there is no second internal
  representation to translate to — diag-server already decodes its own
  wire format natively (`decode_request_payload()`,
  `build_response_payload()`, both already core logic, both already
  correct, neither touched by this). The translation layer's reason to
  exist disappears with the second process.
- **C.4 (dialect dispatch inside the generic `toolset_ipc_forward()`)**
  — that existed to let Dispatch Core forward to diag-server's own
  wire shape across a process boundary. With no second process
  fronting diag-server, there's no boundary to forward across, so
  nothing to dispatch a dialect for.

### 13.2 What's purely additive — new code, existing functions untouched

- **C.1's substance (self-description)** — already planned as new
  diag-server code under the old split (§11.1's DESCRIBE message,
  Phase B.3). Unaffected by this reassignment; DESCRIBE was always
  going to be diag-server answering for itself, not Dispatch Core
  reading a file.
- **C.5's substance (health-check and push/reload triggers)** —
  likewise already Phase B work (HEALTH, PUSH, CHANGED). Whatever used
  to call these across a local socket to a separate process now calls
  them some other way (direct function call, or a message on
  diag-server's own existing WRP receive path) — a routing detail, not
  a change to what these functions do internally.
- **F2's version field, F3's swap mechanism's internal logic** —
  candidate-parse, isolated `validate_static_commands()` run, pointer
  swap — unaffected in substance; see 13.3 for what changes about
  *how* it gets triggered.

### 13.3 What actually requires touching diag-server — **Implemented 2026-08-15** (see §13.4)

Two things don't collapse away and don't stay purely additive, because
the reason they lived in a separate process (Dispatch Core) was
specifically to keep them out of diag-server's own request path:

1. **The ACL Policy Store query.** §10.1 built the entire two-process
   design specifically so "a denied caller's request never reaches, or
   spawns, the toolset process at all" — the check ran *before*
   diag-server, in a different process, on purpose. Moving this into
   diag-server's own deliverable means diag-server itself now has to
   query the ACL Policy Store and act on a denial, before catalog
   lookup/execution — which means a new step added to diag-server's
   request path, even if implemented as one new function called early
   (not a rewrite of `handle_request()`'s existing logic). It also
   means diag-server needs a way to actually reach the ACL Policy
   Store at all — previously Dispatch Core's problem, not diag-server's.
2. **Capability-sync / manifest discovery's outbound side.** Under the
   old split, Dispatch Core read diag-server's DESCRIBE and re-published
   it through whatever generic mechanism every other toolset's
   capability-sync already uses. With no Dispatch Core process doing
   that translation, diag-server itself would need to speak whatever
   protocol capability-sync/manifest-discovery actually requires — new
   outbound integration surface diag-server doesn't have today, distinct
   from just answering DESCRIBE/CHANGED locally.

**Decided 2026-08-15, by direct instruction**: both are built as new,
additive functions in diag-server, called early in the existing
receive path, before catalog lookup/execution — `handle_request()`'s
own internals stay untouched; only the receive-loop call site changes
to call these first. Phase C is now fully retired as a separately-owned
track — everything in it either collapsed away (13.1) or moved into
diag-server's own deliverable (13.2, and now this).

### 13.4 Design sketch for the two new functions — **Implemented 2026-08-15**, by direct instruction

**Placement resolution**: §13's intro sentence ("only the receive-loop
call site changes to call these first") and this section's own text
("immediately after decode_request_payload()") read as two different
placements — the former suggests calling `diag_acl_check()` from
outside `handle_request()` entirely, the latter is only satisfiable
*inside* `handle_request()`, since `decode_request_payload()` only ever
runs there. Went with the more specific, more recently written passage:
the call sits inside `handle_request()`, as a guard clause added at the
top, immediately after the existing `!tool` early-return and before
`pthread_mutex_lock(&g_catalog_mutex)`. This is consistent with how §15
B.4 already added `handle_push_request()`'s transport-origin check —
also a new guard clause at the top of an existing function, not a
change to that function's core decision logic. "`handle_request()`'s
own internals stay untouched" is read as referring to the catalog-
lookup/skip-check/execution logic itself (§13's own explicit "core
logic" list), not as forbidding any addition to the function body at
all — a guard clause that returns before that logic runs doesn't touch
it.

**`diag_acl_check(caller_identity, tool)` → allow/deny.** Called from
diag-server's main receive path immediately after a WRP type-3
request is decoded (`decode_request_payload()`, unchanged) and before
`handle_request()`'s existing catalog-lookup/skip-check/execution logic
runs at all — mirroring exactly where §10.1 always intended the check
to sit ("after resolution... before dispatch"), just inside
diag-server's own process instead of a separate one. On deny, builds
the `{"tool", "exit_code": 126, "stdout": "access denied"}` response
directly (§6/§10's already-decided shape, `exit_code` value confirmed
2026-08-15 — §14 item 2) and returns without
ever calling `handle_request()` — so the "denied caller's request never
reaches execution" property from §10.1 still holds, just enforced one
function earlier in the same process rather than in a different one.

**Merged 2026-08-15, by direct instruction — not a new interface, a
thin wrapper around the existing one.** `diag_acl_check()` doesn't
invent its own query shape; it calls the same
`acl_policy_store_query(const caller_identity_t *caller, const char
*toolset, const char *method)` already declared, as an extern, in
`dispatcher_command_path.c` (line 45) — the identical function every
other toolset's ACL check already goes through. Internally, `tool` is
just `toolset="diagnostics"`, `method=<tool name>`:
`diag_acl_check(caller, tool) { return
acl_policy_store_query(caller, "diagnostics", tool); }` — essentially
that, plus the deny-response-building on top. `caller_identity_t`
(identity string + ACL group array, per
`dispatcher_command_path.c`'s existing definition) is reused as-is,
not redefined for diag-server. This resolves half of §14 item 4:
diag-server isn't building a second, parallel ACL-query contract
alongside the one `dispatcher_command_path.c` already declared — there
remains exactly one query interface project-wide, just called from an
additional place now. **What this does *not* resolve**: `acl_policy_store_query()`
itself was always declared `extern`, deliberately unspecified — merging
onto it means diag-server inherits the *same* still-open transport
question (a real Unix domain socket? RBUS/D-Bus? a linked library?)
rather than getting a second one to solve. That part of item 4 stays
open; only "should diag-server invent its own shape" is resolved.

**Confirmed 2026-08-15**: the `stdout` message stays the literal,
generic `"access denied"` — never a reason, grant name, or policy
detail — deliberately mirroring the precedent `run_command()` already
sets for timeout (`exit_code=124` paired with a descriptive
`"command timed out after %ds"` message in `stdout`), except here the
message is intentionally *not* descriptive: revealing which grant was
missing would tell a caller who was correctly denied more about the
ACL policy's shape than they should learn from a rejection. `exit_code`
stays the primary, stable value a caller checks programmatically;
`stdout`'s text is a human-readable companion only, never something
meant to be parsed. Needs: a way to reach the ACL Policy Store at all
(new dependency for diag-server — network/IPC target not yet chosen),
and A1's identity format (still Phase 2, per §12 Phase E.3 — this
function's design can
proceed, but its `caller_identity` parameter's real shape waits on A1
same as before).

**Capability-sync outbound integration.** Two directions, not one
function: (a) *inbound* self-description is already covered —
DESCRIBE (§11.1) answers a query, whoever asks it; (b) *outbound*,
new — after F3's PUSH successfully promotes a new catalog, in addition
to CHANGED (§11.1, already designed as a local notification), diag-server
now also needs to actually deliver that change to capability-sync's
real transport (per `openspec/specs/capability-sync/spec.md`: "same
transport used for commands," authenticated by device identity, not a
per-session token). Concretely: a new `diag_notify_capability_sync()`
call, fired from the same point CHANGED already fires from.

**Resolved 2026-08-15 — recommended design confirmed.** A JSON-RPC 2.0
*notification* (no `id`, no response expected), sent over WRP
`msg_type: 3` — the same message type commands already use — on
diag-server's existing outbound connection to Parodus, not a new
transport:

```
WRP msg_type: 3, content_type: application/json
{
  "jsonrpc": "2.0",
  "method": "capability_sync.updated",
  "params": {
    "toolset": "diagnostics",
    "version": "<new _catalog_version>",
    "capabilities": [ ...same per-tool list DESCRIBE already answers... ]
  }
}
```

Grounded in two precedents already in this project rather than
invented for the occasion: `build_registration()` already sends an
unsolicited WRP message (`msg_type: 9`) over diag-server's own outbound
channel today — `diag_notify_capability_sync()` is a second instance of
that exact mechanism, different `msg_type`/payload, same "send it
unprompted over the connection I already have" pattern. And
`notifications/tools/list_changed` (referenced in
`define-toolset-as-mcp-tool-model`/`define-synchronous-toolset-push`)
is this project's own existing example of a fire-and-forget JSON-RPC
notification shape — capability-sync is A14's durable, cloud-facing
sibling of that, carried over the device-to-cloud channel instead of a
live MCP session.

Authentication resolves close to automatically from this choice: since
diag-server initiates the message itself rather than answering a
caller's request, there's no per-session SAT token involved at all —
whatever already-authenticated connection it uses to reach Parodus
(the same one `build_registration()` uses) inherently satisfies
"device identity, not session token," with no new credential mechanism
needed. **Left as an implementation detail, not a design question**:
the exact `method` string and destination address.

Both functions are new additions, called from new call sites; neither
touches `load_catalog()`, `is_command_safe()`, `run_command()`,
`validate_static_commands()`, or `handle_request()`'s existing body.

**Implementation notes:**

- `caller_identity_t` and the `extern bool acl_policy_store_query(...)`
  declaration are duplicated in `diag-server-nn.c`, byte-for-byte
  matching `reference-impl/dispatcher_command_path.c`'s own definitions
  (no shared header connects the two builds today — flagged in a code
  comment so a future shared header doesn't leave two hand-synced
  copies). `diag_acl_check()` is exactly the thin wrapper the design
  specifies.
- `caller_identity` at the call site is genuinely best-effort:
  `{identity: req->source, groups: NULL, group_count: 0}`. This is a
  known, documented gap, not a considered design — A1 (the real
  caller-identity/SAT token format) is still open (§12 Phase E.3), so
  there's no real group/permission data to populate yet. The *shape* is
  final; what gets put in it waits on A1, exactly as this section
  already anticipated.
- **`acl_policy_store_query()` has no implementation anywhere in this
  codebase** — confirmed by search; it exists only as the `extern`
  declaration in `dispatcher_command_path.c`, with the ACL Policy
  Store's actual transport still unresolved (§14 item 4). This means
  `diag-server-nn.c` is syntactically complete and correct but will not
  *link* into a runnable binary until that transport is chosen and a
  real implementation exists somewhere in the build. This is a
  pre-existing, project-wide gap that diag-server now shares (it always
  would have, under either the old two-process split or this
  reassignment) — not a new problem introduced by this call site.
- `diag_notify_capability_sync()` needed a new WRP-envelope builder
  (`build_wrp_json_notification()`, a near-copy of `build_wrp_response()`
  with `content_type: application/json` instead of
  `application/msgpack`) rather than parameterizing the existing shared
  one — kept isolated to avoid touching the other five message kinds'
  send call sites. `version` is emitted as a JSON *string*, matching the
  design's own literal example exactly (not a number).

**Verification:** full-file `gcc -Wall -Wextra -fsyntax-only -pthread`
against the real file — clean, same two pre-existing cosmetic warnings
as every prior pass, nothing new (confirms `acl_policy_store_query()`'s
missing implementation is a *link*-time gap only, not a syntax/type
error — `-fsyntax-only` doesn't link). Functional: a harness copying
`diag_acl_check()`, the deny-path block, and
`diag_notify_capability_sync()`/`build_wrp_json_notification()`
verbatim, linked against a mock `acl_policy_store_query()` and a real
(not stubbed) cJSON implementation. 24/24 checks passed, confirming:
(a) a denied request builds `exit_code=126`/`stdout="access denied"`,
sends exactly one response on `req->reply_sock`, and never proceeds
further; (b) an allowed request passes the gate without sending
anything itself; (c) the capability-sync payload is valid, parseable
JSON with the exact designed shape — `jsonrpc`, `method`, no `id`,
`params.toolset`, `params.version` as a string, and a `capabilities`
array with real per-tool fields pulled from a fixture catalog — sent
over the public `g_push_sock`, not a local socket; (d) an unknown plane
name sends nothing and doesn't crash.

## 14. Open-questions register — every still-open decision, indexed (added 2026-08-15)

Consolidating everything scattered across §9/§10.5/§11.1/§12/§13 into
one list, since they accumulated across many separate decisions and
were never gathered in one place. Full reasoning for each stays where
it already is — this is an index, not a re-derivation.

**Superseded, no longer open**: §10.5's "confirm Option A over Option B"
— resolved by everything built on top of Option A since (§12's whole
sequence, §13's further reassignment). No longer a live choice.

### Blocks or informs Phase B (diag-server's own code) — the near-term set

1. ~~F3's health-check pass bar + F2/CHANGED's version-comparison
   rule~~ (§11.1, §12 Phase A.1). **Resolved 2026-08-15**: PUSH
   redesigned to carry a diff (`base_version`/`target_version`/
   `added`/`removed`/`modified`) instead of a full catalog.
   Version-comparison became a `base_version` match against the live
   catalog (compare-and-swap, no ordering scheme needed); the pass bar
   became "no tool in the diff's `added`/`modified` set ends up
   `_skipped`." One design change resolved both halves.
2. ~~Exit-code convention for ACL "access denied"~~ (§10.5, §12 Phase
   A.2, §13.4). **Resolved 2026-08-15: `exit_code = 126`.** The
   `stdout` message half was already confirmed — stays the literal,
   generic `"access denied"` — and the numeric value completes the
   pairing: diag-server's reserved values are `1` (generic failure),
   `124` (timeout, matching `timeout(1)`), and `127` (`run_command()`'s
   child `_exit(127)` when `execvp()` itself fails to launch, matching
   shell's "command not found" convention — already in the code,
   confirmed by re-checking `run_command()` directly). `126` is the
   standard shell-convention partner to that existing `127`: "found,
   but not permitted to execute" — exactly an ACL denial's shape, and
   unused anywhere in diag-server today. Not a new convention, the
   completion of one the code had already half-adopted.
3. ~~Whether `adhoc_diagnostic` gets a stricter ACL policy entry~~
   than the static tools (§10.3, §10.5, §12 Phase A.3). **Partially
   resolved 2026-08-15**: yes, and specifically — mandatory payload
   encryption plus a hash/signature for integrity verification, as two
   separate, deliberately non-overlapping protections (confidentiality
   vs. authenticity), not one covering the other. This directly
   mirrors a precedent this project already established for exactly
   this pairing: `define-synchronous-toolset-push/design.md`'s
   "payload encryption... required" decision explicitly requires both
   encryption *and* artifact signing for `toolset.push`, stating
   plainly that they "are answering two different questions (can
   anyone read this vs. did it really come from a trusted signer)."
   `adhoc_diagnostic` gets the same treatment, for the same reason: an
   arbitrary caller-supplied program is at least as sensitive as a
   pushed artifact, arguably more so since it executes immediately
   rather than going through `toolset.push`'s own verification gate.
   **Timing**: read as "mandatory before this tool can be granted to
   any caller," not "pull Phase 2 encryption forward project-wide" —
   `adhoc_diagnostic` specifically stays ungranted/unusable until its
   own encryption+hash prerequisite exists, whether that arrives via
   Phase 2's general payload encryption landing first, or as an
   earlier, narrowly-scoped exception just for this one tool.
   **Resolved 2026-08-15**: `adhoc_diagnostic` is reachable *only*
   through MCP `tools/call` / JSON-RPC 2.0 (once §8 step 2's manifest
   conversion lands), never through the legacy `{tool,command}` msgpack
   shape — those newer framings carry a hash/signature naturally in
   their `params`, the same way `toolset.push` already does, so §5's
   frozen legacy contract is never touched at all; no new field, no
   scoped exception to the wire format needed. Direct, useful
   consequence: this makes the earlier "unusable until its own
   prerequisite exists" timing note concrete rather than abstract —
   `adhoc_diagnostic` is mechanically ungrantable until §8 step 2 is
   actually done, since there's no other way to reach it. **New detail
   this creates, not yet designed**: whatever enforces this — `diag_acl_check()`
   (§13.4), the ACL Policy Store's own grant model, or a structural
   check in diag-server itself — needs to know *which entry framing* a
   given request arrived through, so a request for `adhoc_diagnostic`
   arriving via the legacy path can be rejected regardless of ACL
   grant, not just discouraged by policy. Flagged for whoever designs
   `diag_acl_check()`'s implementation, not resolved here.
4. **How diag-server reaches the ACL Policy Store** — **moved to Phase
   2, 2026-08-15**, see below. `diag_acl_check()`'s interface itself is
   settled (merged onto the existing `acl_policy_store_query()`
   extern, not a new one); only the transport question moved.
5. ~~What capability-sync's real wire protocol is~~ (§13.4). **Resolved
   2026-08-15**: a JSON-RPC 2.0 notification (`method:
   "capability_sync.updated"`, no `id`), sent over WRP `msg_type: 3` on
   diag-server's existing outbound Parodus connection — not a new
   transport. Grounded in two existing precedents: `build_registration()`'s
   already-working unsolicited-send pattern, and
   `notifications/tools/list_changed`'s already-established
   fire-and-forget notification shape. Authentication resolves for
   free (device-identity, since diag-server initiates it, no session
   token involved). Full message shape in §13.4.

### Deferred, Phase 2 — informs but doesn't block Phase B

4. **How diag-server reaches the ACL Policy Store** (§13.4). **Moved
   here 2026-08-15**, alongside A1 below, consistent with §7/§10's
   existing Phase 2 scoping for the whole ACL gap. `diag_acl_check()`'s
   interface is already settled (merged onto the existing
   `acl_policy_store_query(caller_identity_t*, toolset, method)`
   extern, not a new one). What's still open, and now explicitly Phase
   2: the transport underneath that shared function itself — it was
   always declared `extern`/illustrative, for every caller project-wide,
   not just diag-server's — plus how diag-server discovers the Store's
   address, and how a failed/timed-out query is detected (the
   fail-closed *policy* is already settled, §10.3; the *mechanism* for
   detecting a failure isn't, since that depends on the transport).
6. **A1 — SAT token / caller-identity format** (§12 Phase E.3).
   `openspec/changes/define-sat-token-format/` is drafted but
   unarchived. Blocks Phase C.3-equivalent work (the ACL query's
   `caller_identity` parameter) and the Phase D.2 ACL denial test —
   not Phase B.

### Needed before the OpenSpec change is drafted (§8 step 6 / §12 Phase E.1) — not blocking Phase B either

7. ~~Caller-override — permanent model~~ (§9 Q3). **Resolved
   2026-08-15**: static tools drop overrides entirely — reasoning is
   that the static/dynamic split already gives callers who need
   flexibility a secured path (a `"dynamic"` tool, now gated behind
   §14 item 3's ACL/encryption/framing requirements), so a static
   tool's override no longer serves a real use case. This fully closes
   the residual "same program, different arguments" gap, not just
   narrows it. **Implemented 2026-08-16**, by direct instruction ("Implement
   all the pending items one after another (all phase 1 items)"), treated
   as satisfying the standing separate-confirmation gate for touching
   `handle_request()`'s/`is_command_safe()`'s already-verified behavior.
   `handle_request()`'s override branch now checks `is_dynamic` first: if
   the tool is static (`!is_dynamic`), any caller-supplied `command` is
   discarded unconditionally (with a `syslog(LOG_INFO, ...)` note when one
   was actually sent) and the catalog's own command is always used —
   `is_command_safe()` is never called for static tools anymore. Dynamic
   tools are completely unchanged: they still take the
   `is_override`/`is_command_safe()` path exactly as before.
   `is_command_safe()` itself was left untouched — its `dynamic_type == 0`
   program-pinning branch is now unreachable from `handle_request()`'s
   single call site (which only ever passes `is_dynamic == 1` post-change)
   but was kept rather than deleted, both to minimize diff/risk and in
   case a future, narrower override mechanism needs it again; documented
   in a new comment on `is_command_safe()`'s header explaining exactly
   this. Verified against `test_init_validation.c` (pre-existing harness,
   predates §15's multi-plane rework but still exercises
   `validate_static_commands()`/`is_command_safe()` against the real
   catalog) — updated its three `device_uptime` override checks, which
   previously asserted "same-program override executes with the override's
   args" and "different-program/blocklisted override is rejected," to now
   assert the new behavior: all three override variants (same-program-diff-args,
   different-program, blocklisted) resolve to the *catalog's* command
   (`cat /proc/uptime`), not the override, and none are rejected — the
   override is simply never consulted for a static tool. Also added a
   `simulate_resolved_cmd()` helper (returns the resolved command string,
   not just a pass/fail) so the harness can distinguish "the override ran"
   from "the catalog command ran instead," which a boolean
   would-it-execute check couldn't tell apart. 17/17 checks pass, no
   regressions among the pre-existing skip-blocks-override/dynamic-type
   checks. Also re-ran the full-file `gcc -fsyntax-only -Wall -Wextra
   -pthread` check against the real `diag-server-nn.c` — clean, same two
   pre-existing cosmetic warnings as always (comment-nesting, pthread
   function-pointer cast), no new warnings from this change.
8. ~~Source location~~ (§9 Q4). **Resolved 2026-08-15, by direct
   instruction: `external/diag-server/` — stays exactly where it
   already is, not moved into `reference-impl/diagnostics/`.** Confirms
   the direction §13 had already been pointing: with Phase C fully
   retired and folded into diag-server's own deliverable — no separate
   Dispatch Core component for diagnostics at all anymore — there was
   no remaining architectural reason to relocate it. Every file this
   plan has built or sketched for diagnostics-side work
   (`diag-triage-catalog.json`, `diag-server-nn.c`'s planned extensions,
   §13.4's new functions) belongs under this same path going forward.
9. ~~Requirements numbering~~ (§9 Q5). **Resolved 2026-08-15**: no
   separate `DIAG-FR-###` prefix — reuse the master project's own
   `FR`/`NFR` sequence directly. `REQUIREMENTS.md` is renumbered
   `FR-15`…`FR-32`/`NFR-11`…`NFR-20`, continuing straight from the
   master's `FR-14`/`NFR-10`. See §9 Q5 above for the full mapping and
   which files were updated.

**Status as of 2026-08-15**: nine items tracked, all nine resolved or
explicitly deferred. Fully resolved: 1, 2 (`exit_code = 126`), 3, 5, 7,
8, 9 — seven of nine. Moved to Phase 2 (interface settled, remaining
piece deferred, not abandoned): 4, alongside 6. Nothing blocks Phase B
(diag-server's own code) any longer; 4 and 6 only matter once the ACL
work itself is built; the OpenSpec change (§8 step 6 / §12 Phase E.1)
can now be drafted against a fully resolved open-questions register.

## 15. Phase B, expanded — sub-steps and rationale (added 2026-08-15)

§12.2's Phase B lists four ordered items plus two flagged behavioral
changes. This section breaks each into concrete sub-steps, since
"buildable immediately" covers work of very different sizes and risk
levels — B.2 alone is a real concurrency-sensitive subsystem, while
B.1 is one field.

### B.1 — Catalog version field — **Implemented 2026-08-15**

1. Add a top-level `"_catalog_version"` field to
   `diag-triage-catalog.json` (F2, §11).
2. diag-server reads it into the in-memory catalog object at load
   time, alongside where `validate_static_commands()` already
   annotates `_program_token`/`_skipped` per tool.
3. Nothing consumes it yet — no behavior change beyond storing it.
   DESCRIBE, PUSH, and CHANGED (B.3) are what read and act on it.

**Why first**: zero dependencies, purely additive, and it's the one
piece every other Phase B item needs to already exist before it means
anything. **Worth noting, resolving an earlier open point**: since
§11.1's PUSH design settled on a `base_version` *match* rather than an
ordering comparison, the version value itself doesn't need to be
sortable — a monotonic integer is the simplest choice and reads
cleanly in logs, but a hash or UUID would satisfy the compare-and-swap
mechanics equally well. Recommend the monotonic integer for
readability; not a hard requirement of the design.

**Implementation notes**: `diag-triage-catalog.json` gained
`"_catalog_version": 1` as a top-level sibling of `"tools"`, plus a
`_catalog_version_comment` explaining the compare-and-swap semantics
(mirroring the existing `_comment` field's style). `diag-server-nn.c`
gained a new global `static long g_catalog_version = 0;` next to
`g_catalog`, and `load_catalog()` now reads `_catalog_version` via
`cJSON_GetObjectItem`/`cJSON_IsNumber` right after a successful parse,
defaulting to `0` with a `LOG_WARNING` if the field is absent (an
older or hand-edited catalog) rather than treating that as a parse
failure. The existing `"catalog loaded from %s"` syslog line now
includes the version. No other code path reads `g_catalog_version` yet
— purely additive, matching the plan; B.2/B.3 are the first real
consumers. Not build-verified in this sandbox (third-party headers —
nanomsg, msgpack, cJSON — aren't installed here); the change was kept
minimal and follows the exact `cJSON_GetObjectItem`/`cJSON_IsNumber`/
`->valueint`-style pattern already used elsewhere in this file (e.g.
the existing timeout-field read), so it should build cleanly against
the project's normal toolchain — worth a real build/lint pass before
this is considered done, not just reviewed.

**Follow-up, 2026-08-15 — catalog renamed, one file per plane.** Per
direct instruction: `diag-catalog-xb10.json` is renamed to
`diag-triage-catalog.json`, dropping the `xb10` device-model qualifier
(the catalog's contents were never actually device-specific — every
tool in it is a generic shell/proc-file command, and tying the
filename to one hardware model was misleading once this catalog is
meant to generalize). All 11 tool entries plus `adhoc_diagnostic`
already declare `"plane": "triage"` (added 2026-08-14), so the rename
is exact — nothing in the file belongs to a different plane today.

Going forward, catalogs are split one file per plane rather than one
catalog mixing planes, using the naming convention
**`diag-<plane>-catalog.json`**: `diag-triage-catalog.json` (this
file, existing), and — if/when tools for those planes are actually
added — `diag-management-catalog.json`, `diag-control-catalog.json`,
`diag-config-apply-catalog.json`, matching this project's established
plane model (config-apply/management/control/triage). No other plane's
catalog exists yet — this is a naming convention adopted now, not a
claim that those three files were created. `diag-server-nn.c` itself
is unaffected by the rename beyond the doc-comment references already
updated: it loads whatever single catalog path it's given via
`CATALOG_PATH`/`argv[1]` (§ Usage, top of file), so a deployment
targeting the triage plane points that path at
`diag-triage-catalog.json` — the runtime default path
(`/etc/diag-server/catalog.json`) is a generic deployment location, not
tied to the source repo's per-plane filename, and was intentionally
left unchanged here since multi-catalog *loading* (one diag-server
instance serving more than one plane's file at once) is not something
this codebase does today and wasn't asked for.

Every reference to the old filename across `diag-server-nn.c`,
`README.md`, `REQUIREMENTS.md`, and this document was updated to
`diag-triage-catalog.json`.

### B.2 — F3's core swap mechanism — plane-scoped — **Implemented 2026-08-15**

This is the one piece worth real scrutiny — the rest of Phase B is
built on top of it. **Revised** from its original single-catalog
phrasing now that B.5 replaced the one global `g_catalog` with
`g_planes[PLANE_COUNT]`: every step below operates on **one plane's
entry in that table at a time** — `g_planes[i].catalog` for whichever
plane a PUSH names — not a single global object. This was the whole
point of keeping catalogs as separate objects per plane in B.5 rather
than merging them (see B.5 point 1's rationale): a push to the
management catalog must never touch triage's live object or version,
and vice versa.

1. **Clone**, not mutate: build a candidate catalog object as a full
   copy of `g_planes[i].catalog` for the target plane `i`. That plane's
   live object is never touched until a promote actually happens; every
   *other* plane's entry in `g_planes[]` is untouched for the entire
   operation, not just left alone in spirit.
2. **Apply the diff** (`added`/`removed`/`modified`, per B.3's PUSH
   message) to the clone only: add new tool entries, remove named
   ones, replace modified ones wholesale (whole-tool replacement, per
   §11.1's PUSH design — not a field-level patch).
3. **Validate in isolation**: run `validate_static_commands()`'s
   per-tool logic (the inner loop it now runs once per plane, per B.5)
   against the candidate clone, not against `g_planes[i].catalog`
   itself. **Note**: this is deliberately just the per-tool validation
   loop, not a call into `detect_cross_plane_collisions()` — a
   candidate is checked against its own tools only; whether the
   *promoted* result introduces a new cross-plane collision is a
   separate concern, below.
4. **Decide pass/fail**: per §11.1/§14 item 1's resolution — no tool in
   the diff's `added`/`modified` set may end up `_skipped`. Tools the
   diff didn't touch aren't re-checked.
5. **On pass, promote**: atomically swap `g_planes[i].catalog` (and
   `g_planes[i].version` to `target_version`) to point at the
   candidate, so every *new* request sees it as plane `i`'s live
   catalog from this point on. No other `g_planes[]` entry changes.
6. **Concurrency model — implemented 2026-08-15, simpler than originally
   sketched.** The original draft of this step assumed a reference
   count or generation counter would be needed to track in-flight
   readers of the old object. Looking closely at `handle_request()`'s
   *actual* structure while implementing this shows that's more than
   necessary: every field `handle_request()` needs from a catalog entry
   (`cmd` via `strdup(catalog_cmd)`, `timeout_sec`, `suppress_stderr`,
   `count_lines_matching` via `strdup()`, `is_dynamic`,
   `plane_dup` via `strdup()`) is copied out of the cJSON tree into
   caller-owned locals **before** any blocking work (`run_command()`,
   which can take up to the tool's timeout) starts. No raw `cJSON*`
   pointer derived from a catalog entry is ever retained across a
   blocking call. That means the *only* danger window is the short,
   synchronous span from `catalog_lookup()` through the last
   `cJSON_GetObjectItem()`/`cJSON_GetStringValue()` call in
   `handle_request()`'s `if (entry) {...} else {...}` block — a handful
   of pointer dereferences and `strdup()`s, no I/O, no timeout wait.
   Given that, a single global mutex (`g_catalog_mutex`) covering
   exactly that span — acquired right before `catalog_lookup()`,
   released right after the last field extraction, **never held across
   `run_command()`** — is sufficient: a promote's swap step (below)
   takes the same mutex for the swap itself, so mutual exclusion
   guarantees no reader can be mid-extraction from the old object at
   the moment it's swapped out, and by the time the promote thread
   releases the mutex after swapping, every reader that started before
   the swap has already finished touching the old object. The old
   object can then be freed immediately after releasing the mutex, no
   deferred/refcounted cleanup required. One global mutex (not
   per-plane) is deliberate too — a no-plane lookup (§15 B.5) scans
   every loaded plane in one call, so a per-plane lock would mean
   locking up to four locks per no-plane request; a single mutex with a
   critical section this short makes that complexity unnecessary.
7. **Serializing concurrent pushes to the same plane.** A second,
   separate lock — one `pthread_mutex_t push_lock` per plane, distinct
   in purpose from `g_catalog_mutex` above — is held for a push's
   *entire* duration (clone through swap). This isn't for reader/writer
   safety (that's `g_catalog_mutex`'s job); it's so two competing pushes
   to the same plane can't race each other's `base_version` check
   against the final swap: holding `push_lock` for the whole operation
   means no other push to that plane can be in flight, so the
   `base_version` read at the start of the operation is still accurate
   at the swap step — the compare-and-swap semantics §11.1 specified
   hold for real, not just "usually."
8. **Free the old object once the swap's mutex is released** — per
   point 6's reasoning, this can happen immediately, not deferred.
9. **On fail, discard the candidate** and report the rejection reason;
   plane `i`'s live catalog was never touched, so there is no "revert"
   step to perform — nothing was ever taken offline, and no other
   plane was ever in scope. "Fail" now includes persistence failure —
   see the new step below, inserted between validation and promote.
10. **New consequence of B.5, not in the original design**: a promote
    can introduce a fresh cross-plane name collision that didn't exist
    before (e.g. plane `i`'s pushed catalog now declares a tool name
    that plane `j` already had). B.5's `catalog_lookup()` no-plane path
    already recomputes the collision check fresh on every request (by
    design, specifically so this case doesn't need a separate mechanism
    here) — so this is already handled correctly without B.2 needing
    its own collision-awareness. Worth stating explicitly rather than
    leaving as an implicit consequence: **no new step is needed in this
    pipeline for it**, the design already covers it.

**Why standalone, before any trigger exists**: this is genuinely a
small concurrency-sensitive subsystem inside a process that has
otherwise never needed one — the existing code has no precedent for
two versions of catalog state coexisting even briefly, per plane or
otherwise. This was verified empirically, not just by review — see
"Verification" below.

### B.2.5 — Disk persistence, per direct instruction ("a facility to provide a persistent path for catalog commands from cloud") — **Implemented 2026-08-15**

**The gap this closes**: everything above only ever changes the
*in-memory* `g_planes[i].catalog`. Without this step, a successful push
would be silently lost on the next restart — `load_catalogs()` would
re-read the original, un-pushed file from disk and the device would
revert to a stale catalog with no record anything had changed. For a
push arriving from the cloud to actually mean anything durable, the
promoted result has to reach disk, at the same path `load_catalogs()`
already reads at startup — the same four `diag-<plane>-catalog.json`
files, updated in place, not a new mechanism.

1. **Where in the pipeline this sits**: after validation passes (step
   4 above) and *before* the in-memory promote (the swap in step 6/7).
   This ordering is deliberate — see the failure policy below.
2. **What gets written**: the full candidate catalog object, serialized
   back to JSON text via `cJSON_PrintUnformatted()` — the whole object,
   not just the diff, so `_comment`/`_catalog_version_comment` and every
   untouched tool entry survive unchanged, exactly as they were cloned
   from the live catalog in step 1. `_catalog_version` in the written
   JSON is updated to `target_version`, matching what B.2's in-memory
   swap is about to set.
3. **Crash-safe write**: write to a temp file in the *same directory*
   as the plane's real catalog file (`<dir>/.diag-<plane>-catalog.json.tmp`),
   `fsync()` the file descriptor, close it, then `rename()` over the
   real path. POSIX `rename()` within the same filesystem is atomic —
   a concurrent `load_catalogs()` (there isn't one today, since loading
   only happens once at startup, but this makes the mechanism correct
   regardless) or a crash mid-write can never observe a half-written
   file; it's always either the complete old file or the complete new
   one. Recommended, not yet confirmed as mandatory: also `fsync()` the
   containing directory after the rename, since some filesystems don't
   guarantee a rename survives a power loss without it — cheap to add,
   relevant given this runs on CPE devices where an unclean reboot
   during a push is a real scenario, not a theoretical one.
4. **Failure policy — reject, don't diverge.** If the temp write,
   `fsync()`, or `rename()` fails for any reason (disk full, permission
   error, read-only filesystem), the *entire* push is rejected — the
   in-memory promote (step 6/7) never happens, `g_planes[i].catalog` and
   `.version` are untouched, and the response is
   `{"status":"rejected","plane","reason":"failed to persist catalog to disk: <detail>"}`.
   This was a deliberate choice over the alternative (promote in-memory
   anyway, report a "loaded but not persisted" degraded status): letting
   memory and disk diverge means a future restart silently reverts a
   push the cloud believes succeeded, with no signal at the time it
   happened — a rejected push is a clear, immediate signal instead of a
   deferred, confusing one discovered only after the next reboot.
5. **What doesn't change**: `load_catalogs()` itself is untouched — it
   already reads whatever's at the plane's file path, and a pushed
   catalog is now just what's sitting there the next time it runs. No
   separate "did we push something" flag or journal is needed; the
   catalog file *is* the durable record.

**Implemented 2026-08-15.** `catalog_apply_push(plane_name, base_version,
target_version, diff)` is the new function implementing both B.2 and
B.2.5 together — it takes an already-parsed `diff` (a plain `cJSON`
object shaped `{added, removed, modified}`, per §11.1), not a wire
message, so §15 B.3's future PUSH handler will be a thin parser in
front of it, matching the original intent that this piece be "built and
tested standalone before anything can trigger it." `validate_static_commands()`
was refactored to share its per-tool validation loop with this function
via a new `validate_catalog_tools(catalog, plane_name, &checked,
&skipped)` helper, so a candidate is checked with the exact same rules
the live catalog was checked with at startup, not a parallel
reimplementation. `plane_catalog_t` gained `loaded_path[512]` (the
real on-disk path each plane's catalog was loaded from, recorded by
`load_catalogs()` even for an absent plane, so a push can bootstrap a
not-yet-provisioned plane) and a per-plane `push_lock` that serializes
concurrent pushes to *that* plane for the whole operation, making the
`base_version` check race-free without extra bookkeeping. A new global
`g_catalog_mutex` was added and `handle_request()` was updated to hold
it from `catalog_lookup()` through the last field extracted off the
resolved entry — never across `run_command()` — per the concurrency
model point 6 above.

Verification, not just review: a scratch harness (extended cJSON-shim,
not committed) copied every one of these functions verbatim and ran, in
order: unknown-plane rejection, stale-`base_version` rejection (with
confirmation the live catalog was untouched), a valid push (new tool
added, promoted, immediately resolvable via `catalog_lookup()`), a push
containing a blocklisted command (rejected, candidate discarded,
**never** promoted or persisted), a durability check (re-parsed the
actual on-disk file after several pushes and confirmed it reflects only
the successful ones, still has an untouched original tool, and matches
the in-memory version), and — the highest-value check for this
piece — a concurrency stress test: 8 reader threads continuously
resolving tools against the live triage catalog while a 9th thread
performed 200 real pushes/promotes/frees, the whole binary built with
`-fsanitize=thread` (ThreadSanitizer). Over ~211,000 concurrent reader
iterations racing against 200 promote-and-free cycles, **zero data
races were reported against the actual swap/free logic** — the one
race TSan did catch was in the test harness's own loop-termination flag
(a plain non-atomic variable), fixed in the harness, not in
`diag-server-nn.c`. All 19 checks passed on a clean re-run. The real
repo's `diag-triage-catalog.json` was never touched by any of this —
every test ran against a copy in an isolated scratch directory. Not
build-verified against the real nanomsg/msgpack/cJSON toolchain in this
sandbox, same limitation as B.1/B.5 — worth a real build/lint pass, and
this is exactly the piece where that matters most given its
concurrency-sensitivity.

### B.3 — Local protocol handlers — plane-scoped — **Implemented 2026-08-15**

1. **EXEC** — already exists, unchanged (and already plane-aware as of
   B.5 — `handle_request()` already calls `catalog_lookup(tool,
   req_plane)`). Confirms the shared code path
   (`decode_request_payload()` → `handle_request()`) is what every
   local-protocol addition builds around, not something being replaced.
2. **DESCRIBE** — new handler. **Revised for B.5**: takes an optional
   `plane` argument in its own request, mirroring EXEC's request-level
   `plane` field. With `plane` given, returns that one plane's
   `{plane, version, tools: [{name, type, plane, timeout}]}` read from
   whichever catalog object is *currently marked live* for it (never a
   candidate mid-validation, by construction — B.2 step 5 is the only
   thing that changes what "live" points to, per plane). Without
   `plane`, returns an array of that same shape for every *loaded*
   plane (a plane with no catalog loaded is simply omitted, matching
   B.5's "absent means unserved, not an error" treatment) — this is the
   natural place a future caller discovers which planes are actually
   being served at all.
3. **HEALTH** — new handler. Side-effect-free liveness probe; doesn't
   walk any catalog at all, so it stays cheap regardless of catalog
   size or plane count. Answers a different question than B.2's
   validation step ("is the process alive" vs. "is this candidate
   acceptable") even though both live on the same channel. Plane-
   agnostic by nature — a single diag-server process is either up or
   it isn't, regardless of which planes it currently serves.
4. **PUSH** — new handler. **Revised for B.5**: the message shape gains
   a mandatory `plane` field — `{plane, base_version, target_version,
   diff}` — naming which of the four `g_planes[]` entries this push
   targets. An unrecognized `plane` value is rejected immediately (same
   "just a miss" treatment `catalog_lookup()` already gives an
   unrecognized plane name), before `base_version` is even checked.
   Given a recognized plane, checks `base_version` against
   `g_planes[i].version` *before* touching B.2's pipeline at all — a
   mismatch is rejected immediately, no clone or validation attempted.
   On a match, hands off to B.2 steps 1–9 for that one plane and
   responds synchronously with `{"status":"loaded","plane","version"}`
   or `{"status":"rejected","plane","reason"}`. **A push to one plane
   never affects any other plane's version or live catalog** — this is
   the direct payoff of B.5 keeping catalogs as separate objects rather
   than merging them.
5. **CHANGED** — new, unsolicited notification, sent immediately after
   B.2 step 5 succeeds for a given plane, carrying that plane's name
   (`{plane, version}`) so the receiver knows which one changed rather
   than having to re-DESCRIBE everything to find out. This is the
   *local* notification to whatever is on the other end of the local
   socket (Phase C, once built) — distinct from
   `diag_notify_capability_sync()` (§13.4), the separate, real
   cloud-facing notification that should fire from this same trigger
   point but travels over diag-server's outbound Parodus connection,
   not the local endpoint. Both fire together; they are not the same
   message.
6. **Routing**: the existing receive loop distinguishes WRP type-3
   (execute) from type-9/10 (registration/keepalive) already. The new
   local endpoint (§10.3) needs its own small dispatch — by message
   kind, not WRP type, since DESCRIBE/HEALTH/PUSH aren't WRP-framed at
   all, they're local-only — separate from, not layered onto, the
   existing Parodus-facing socket handling.

**Why after B.2**: every one of these either reads live catalog state
(DESCRIBE), or calls into B.2's pipeline directly for one plane (PUSH),
or fires as B.2's per-plane side effect (CHANGED) — none of them are
independently testable without B.2 already working correctly.

**Implemented 2026-08-15.** Concretely: every local-protocol message
still rides the exact WRP type-3 payload EXEC already uses (§10.3) —
the four new kinds are disambiguated by a "kind" string field EXEC
itself never carries, not a new envelope or a new socket. This lands
on the *existing* `g_pull_sock`/`g_push_sock` pair; it does not require
§15 B.4's new local-only bind to exist first, since B.4 only changes
*which address* diag-server is reachable at, not how an already-received
message gets dispatched. `handle_request()` itself is untouched, per
point 1 above — a new `handle_local_request()` wraps it: `peek_message_kind()`
reads the optional "kind" field first, and only "DESCRIBE"/"HEALTH"/"PUSH"
route elsewhere; absent or `"EXEC"` falls straight through to the
original, unmodified `handle_request()`. `main()`'s message loop now
dispatches to `handle_local_request()` instead of `handle_request()`
directly — its only change.

New pieces: `msgpack_obj_to_cjson()`, a small recursive converter for
PUSH's `diff` field — the wire is msgpack (matching everything else
this file speaks), the live catalog and B.2's candidate manipulation
are cJSON (matching the catalog file format), so this is the one place
those two representations cross. `decode_describe_request()`/
`pack_one_plane_describe()`/`build_describe_response_payload()`
implement DESCRIBE exactly as designed (plane-qualified or all-loaded-planes,
holding `g_catalog_mutex` for the read, an unknown/unloaded plane
answering with an empty object rather than an error).
`build_health_response_payload()` implements HEALTH, deliberately
touching no catalog state at all. `decode_push_request()`/
`build_push_response_payload()` are thin wrappers around the
already-implemented `catalog_apply_push()` (§15 B.2) — parse the wire
fields, convert `diff` via `msgpack_obj_to_cjson()`, delegate, encode
the outcome. `build_changed_notification_payload()`/
`send_changed_notification()` implement CHANGED for real, not as a
stub — it reuses `build_registration()`'s existing outbound
`g_push_sock` path (per §11.1's own justification: "the same shape of
thing, over the same kind of channel"), so no new channel was needed
for this to be genuinely working code today. `handle_push_request()`
sends CHANGED only after the synchronous PUSH response has already
gone out, and only on `PUSH_OK`.

Verification, in two layers. First, a full syntax/type check of the
*entire* real file (not a copied excerpt) against hand-written stub
headers matching the real nanomsg/msgpack-c/cJSON API surfaces this
file actually uses (checked by grepping every `msgpack_*`/`MSGPACK_*`/
`nn_*`/`cJSON_*` identifier referenced) — `gcc -Wall -Wextra -fsyntax-only`
over all ~2,100 lines came back clean, with only two pre-existing,
unrelated cosmetic warnings (a `/*`-inside-comment nit predating this
work, and the same pthread function-pointer cast style already used by
the original `handle_request()` dispatch). Second, functional
verification specifically for B.3's new logic: a scratch harness linked
the copied-verbatim new functions against a *real, working* (if
minimal) msgpack encoder/decoder and cJSON implementation — not typed-only
stubs — and drove genuine msgpack byte round trips: `peek_message_kind()`
correctly distinguishing EXEC (no `kind` key) from DESCRIBE/HEALTH/PUSH;
DESCRIBE's single-plane and all-planes shapes, including an exact
12-tool count matching the real catalog; HEALTH's trivial response;
and a full PUSH round trip — real msgpack bytes with a genuinely nested
`diff.added.<tool>` object, decoded, converted to cJSON, run through
the already-independently-verified `catalog_apply_push()`, promoted,
and the response bytes decoded back and checked — plus a stale-`base_version`
rejection through the same real decode path, and CHANGED's notification
content. All 34 checks passed (the first run caught a bug in the test
harness's own hand-built request bytes — a map declared with the wrong
key count — not in the production code; fixed and re-verified clean).
Not build-verified against the real toolchain in this sandbox, same
standing caveat as B.1/B.2/B.5.

### B.4 — Re-point registration — **Part 1 implemented 2026-08-15; part 2 held, gated on Phase D**

Unaffected by B.5's multi-plane rework — this step is about which
transport endpoint diag-server answers on, not which catalogs it
serves through it, so nothing here changes shape.

1. Bind the new local-only endpoint (§10.3) *alongside* the existing
   public `tcp://127.0.0.1:6666/6669` pair — both active at once,
   nothing removed yet. **Implemented.**
2. Only after Phase D's tests pass (wire-fidelity round-trip, push/reload
   — both require B.1–B.3 working correctly, across every plane
   actually under test, not just one): disable `build_registration()`'s
   WRP type-9 registration to the real Parodus, leaving the local
   endpoint as diag-server's only reachable address. **Not implemented
   — Phase D hasn't run yet.** `build_registration()` and its send in
   `main()` are untouched.

**Why last and gated**: this is the actual point of no return. Holding
it until Phase D passes means diag-server keeps working exactly as it
does in production throughout the entire rest of Phase B's development
and testing — nothing about today's real, working behavior is at risk
until this one step, and this one step only happens once everything
upstream of it is proven, not assumed.

**Part 1 implementation notes:**

- Two new ipc:// addresses, `DIAG_LOCAL_RECV_URL`
  (`ipc:///run/dispatcher/diagnostics-in.sock`, diag-server binds/
  receives) and `DIAG_LOCAL_SEND_URL`
  (`ipc:///run/dispatcher/diagnostics-out.sock`, diag-server connects/
  sends) — not the single `DIAG_LOCAL_ENDPOINT` address named in
  `reference-impl/diag_legacy_framing.c`. Deliberate: PUSH/PULL sockets
  are unidirectional, so a bidirectional local channel needs two
  addresses, mirroring the public pair's own `CLIENT_URL`/`PARODUS_URL`
  split, rather than switching to nanomsg's REQ/REP socket type (which
  would have kept one address but meant reworking every response-send
  call site's reply discipline, not just where it sends). **Flag for
  Phase C**: the reference-impl's illustrative single-address sketch
  needs updating to match these two addresses when Dispatch Core's real
  side is built — noted in a code comment at the new `#define`s so this
  doesn't silently diverge.
- The local bind/connect is best-effort, not fatal to startup: if
  `/run/dispatcher` isn't provisioned (true in every environment this
  project has tested in so far), diag-server logs a warning and falls
  back to serving only the public path — identical to today's
  production behavior. No retry-with-backoff loop for the local
  connect (unlike the public path's parodus connect): nanomsg's
  `nn_connect()` on a PUSH socket queues and reconnects in the
  background on its own, and blocking startup on an optional endpoint
  would undercut the whole "alongside, nothing removed" premise of part
  1.
- `main()`'s message loop changed from a single blocking `nn_recv()` on
  one socket to `nn_poll()` across up to two (public always, local only
  if `g_local_enabled`) — factored into a new `service_one_message()`
  helper so both sockets run through identical receive/decode/dispatch
  logic rather than two hand-maintained copies. When the local endpoint
  isn't active this degrades to the exact original single-socket
  behavior.
- `wrp_req_t` gained two fields, `reply_sock` and `from_local`, set by
  `service_one_message()` after `decode_wrp()` succeeds (transport
  metadata, not part of the WRP message itself). Every response-send
  call site across `handle_request()`/`handle_describe_request()`/
  `handle_health_request()`/`handle_push_request()` was changed from
  the hardcoded public `g_push_sock` to `req->reply_sock`, so a request
  received via the local endpoint is answered back over the local
  endpoint, not routed to Parodus.
- **PUSH transport restriction (explicit decision, confirmed with the
  project owner before implementing) — since removed, see below.**
  `handle_push_request()` originally rejected any PUSH where
  `req->from_local` was false, before decoding the request at all,
  returning the existing `push_outcome_t` rejection shape with a new
  `PUSH_ERR_FORBIDDEN_TRANSPORT` status. Reasoning at the time: once
  both endpoints were live at once, and no ACL check (§10, §13.4)
  existed yet on this path, an unrestricted PUSH would let any
  WRP-addressable external caller reaching the public path push a new
  catalog with zero authorization. DESCRIBE and HEALTH carried no such
  restriction — both are read-only/side-effect-free, so leaving them
  reachable on both sockets during the transition didn't reopen this
  gap. `send_changed_notification()` takes the outbound socket as a
  parameter (instead of hardcoding `g_push_sock`) and is always called
  with `req->reply_sock` — originally this meant CHANGED always went
  out over `g_local_push_sock` in practice, since a successful promote
  could only originate via the local endpoint; see the update below for
  why that's no longer true.

  **Removed 2026-08-16, by direct instruction.** The restriction is now
  gated behind `PUSH_REQUIRE_LOCAL_ONLY` (0 by default), so PUSH is
  accepted on either socket, unconditionally — the `req->from_local`
  check and `PUSH_ERR_FORBIDDEN_TRANSPORT` are left fully intact in the
  code, just unreached, matching the same single-flag-revert pattern as
  `REGISTER_WITH_PARODUS` (§12.2 D.4). The reasoning above no longer
  holds by design: with registration also re-enabled (§12.2 D.4's own
  revert), any WRP-addressable caller reaching the public pair can now
  push a new catalog to any plane — `catalog_apply_push()`'s own
  validation still runs (blocklist/program-pin checks against the
  diff's *content*), but nothing checks *who* is allowed to send a
  PUSH at all. `send_changed_notification()`'s "always
  `g_local_push_sock` in practice" note above is therefore also stale —
  CHANGED now goes out over whichever socket (`g_push_sock` or
  `g_local_push_sock`) the triggering PUSH itself arrived on.

**Verification:** full-file `gcc -Wall -Wextra -fsyntax-only -pthread`
against the real file with extended stub headers (added `nn_poll()`/
`struct nn_pollfd`/`NN_POLLIN` to the nanomsg stub, matching real
nanomsg's `nn.h` shape) — clean, only the same two pre-existing
cosmetic warnings from the B.3 syntax pass, no new warnings or errors.
Functional: a harness copying `handle_push_request()` and
`send_changed_notification()` verbatim, against recording stubs for
`nn_send()`/`decode_push_request()`/`catalog_apply_push()`, confirmed
(a) a PUSH with `from_local=0` is rejected before
`decode_push_request()`/`catalog_apply_push()` are ever called, sends
exactly one response (no CHANGED), on the public reply socket; (b) a
PUSH with `from_local=1` proceeds through the unchanged decode/apply
pipeline exactly as before B.4, and both the response and the CHANGED
notification go out on the *local* reply socket specifically, not the
public one. 9/9 checks passed. `main()`'s new socket-setup/poll-loop
code was verified by syntax check and code review, not a runtime
harness — its logic is a direct, low-branching translation of the
existing public-socket setup (bind/connect/best-effort-disable) plus a
poll() over one or two file descriptors, with no new decision logic of
its own beyond what's already covered by the `handle_push_request()`
test above.

### B.5 — Multi-plane catalog loading, with an explicit `plane` request field — **Implemented 2026-08-15**

**Question that prompted this**: with catalogs now split one file per
plane (`diag-<plane>-catalog.json`, the rename/convention above), can a
single diag-server process load and serve *all* planes' catalogs? First
design pass considered deriving the plane silently from wherever the
tool name was found (no wire change). **Superseded by direct
instruction**: mention the plane explicitly in the request instead, so
the caller states which plane a tool belongs to rather than diag-server
inferring it. This is implemented as the optional additive `plane` key
described in §5's amendment above — `{"tool": "<name>", "command": "",
"plane": "<optional>"}` — not a breaking wire change.

1. **Load, not merge.** Each `diag-<plane>-catalog.json` present at a
   configured location is parsed into its **own** `cJSON` object — not
   flattened into one big `tools` table. This matters beyond just code
   simplicity: keeping catalogs as distinct objects means each plane
   keeps its own `_catalog_version` (B.1) and can go through B.2's
   clone/validate/swap pipeline **independently** — a PUSH that updates
   the triage catalog doesn't touch the management catalog's version or
   trigger its own swap. Merging into one flat object would lose that
   per-plane independence and force every PUSH to reason about the
   whole merged blob.
2. **Global state changes shape.** `g_catalog` (currently a single
   `cJSON*`) becomes a small fixed-size table of `{plane_name,
   cJSON *catalog}` entries — one per loaded file — rather than a single
   pointer. `validate_static_commands()` (B.1/existing) runs once per
   catalog in this table, not once globally, and its existing per-tool
   log lines gain the plane name as context.
3. **Load-time discovery.** Rather than a single `CATALOG_PATH`/`argv[1]`
   override naming one file, `load_catalog()` needs to enumerate the
   set of catalog files to load. Simplest option, and the one this
   section recommends: a fixed, small list of known filenames checked
   at a configured directory (e.g. `diag-triage-catalog.json`,
   `diag-management-catalog.json`, `diag-control-catalog.json`,
   `diag-config-apply-catalog.json`) — each loaded if present, silently
   skipped (with a `LOG_INFO`, not a warning) if absent, since not
   every deployment necessarily populates every plane. This avoids a
   directory-glob dependency and keeps the set of possible planes
   explicit in code rather than "whatever files happen to be dropped in
   a directory."
4. **`decode_request_payload()` gains a third field.** One more
   `else if` branch, structurally identical to the existing `tool`/
   `command` extraction: `kl == 5 && memcmp(k, "plane", 5) == 0`,
   producing a new `plane_out` string (NULL if the caller omitted it —
   this is optional, not required, exactly like `command` already is).
5. **Lookup becomes plane-aware, with the caller's declaration
   authoritative when present.** `catalog_lookup(tool, plane)`:
   - If `plane` is supplied and non-NULL: look up `tool` **only** in
     that plane's catalog. Not found there → not found, full stop — it
     does **not** fall through and search other planes. A caller that
     explicitly names a plane and gets it wrong should see a clear
     "unknown tool" result, not a silent cross-plane resolution that
     masks the mistake.
   - If `plane` is omitted (every existing/legacy caller, until they're
     updated to send it): fall back to searching across all loaded
     catalogs in a fixed order (e.g. triage → management → control →
     config-apply), preserving today's caller-facing behavior
     unchanged. This is the one path where the name-collision question
     below still applies.
6. **The collision question, now narrower.** A same-named tool in two
   planes' catalogs is only ambiguous on the *no-plane-supplied*
   fallback path (point 5, second bullet) — a caller that supplies
   `plane` never hits it, since lookup is scoped to exactly one catalog.
   **Recommendation, unchanged from the earlier draft of this section**:
   treat a cross-plane name collision the way `validate_static_commands()`
   already treats a blocklisted/unparseable static command — log
   `LOG_ERR` at startup and mark both colliding entries unusable on the
   no-plane fallback path specifically (a `plane`-qualified request for
   either one still works normally, since that path never needs to
   choose between them). This still needs an explicit confirmation
   before being treated as final.
7. **What doesn't change**: `handle_request()`'s overall flow and
   `is_command_safe()` are unaffected beyond taking the resolved catalog
   entry from step 5 instead of a single global one — this is about
   *which* catalog object gets searched and how, not how a found entry
   is executed once located.

**Sequencing**: B.5 depends on B.1 (each catalog needs its own version)
and slots naturally alongside B.2/B.3 — the swap mechanism and PUSH
handler both need to operate per-plane-catalog once this lands, so B.5
is best done before B.2/B.3 are implemented for real, even though B.1
was implemented first as the simplest, most self-contained piece.

**Implemented 2026-08-15, collision policy confirmed as recommended
(point 6, option treating a collision like a bad static command).**

Implementation notes: `g_catalog`/`g_catalog_version` are gone, replaced
by `plane_catalog_t g_planes[PLANE_COUNT]` (`{plane, filename, catalog,
version}`, `PLANE_COUNT = 4`). `load_catalog()` is replaced by
`load_catalogs()`, which enumerates the fixed four filenames at
`catalog_dir_override`/`CATALOG_DIR` (default `/etc/diag-server`,
overridable via `argv[1]`, now a directory rather than a file path — a
deploy-facing CLI change, documented in the file's top-of-file usage
comment and README.md §2.1/§3.4/§4.1); a missing file logs `LOG_INFO`
and leaves that plane's `catalog` NULL (simply unserved), a file that
exists but fails to parse logs `LOG_ERR` and is treated the same way
rather than aborting startup. `validate_static_commands()` now loops
over `g_planes[]`, running its existing per-tool logic once per loaded
catalog with the plane name folded into its log lines. A new
`detect_cross_plane_collisions()` runs once at startup (called from the
end of `validate_static_commands()`) purely for log visibility.
`decode_request_payload()` gained the `plane_out` parameter exactly as
designed (point 4). `catalog_lookup(tool, plane)` implements point 5
as designed: plane-qualified lookups never fall through; the no-plane
path recomputes the collision check fresh on every call (rather than
trusting a startup-only flag) so a collision introduced by a future
per-plane PUSH (§15 B.2, not yet built) is still caught correctly, not
just whatever the catalogs looked like at process start. `handle_request()`
and `main()` were updated to the new signatures; the shutdown path now
frees every loaded plane's catalog instead of one global object.

Verification: `test_init_validation.c` (the existing standalone
harness referenced in REQUIREMENTS.md FR-25c/NFR-13a) still passes in
full against the renamed `diag-triage-catalog.json` — real execution,
not just review (`gcc` build + run, all prior checks green). A second,
new scratch harness (not committed — verification-only) copied B.5's
actual new functions verbatim against a temporary two-plane fixture
(the real triage catalog plus a synthetic management catalog sharing
one tool name, `device_uptime`, with triage on purpose) and exercised:
partial plane loading (2 of 4 planes present, the other two correctly
logged as absent rather than erroring), per-plane version reads,
plane-qualified lookups resolving to the *correct, distinct* catalog
entry for the same tool name in two different planes, a plane-qualified
lookup for the wrong plane correctly missing rather than falling
through, an unrecognized plane name behaving as a clean miss, the
no-plane path resolving unique names normally, and the no-plane path
correctly rejecting the deliberately-collided name with an `LOG_ERR`
mentioning "ambiguous." All 19 checks passed. Not build-verified against
the real nanomsg/msgpack/cJSON toolchain in this sandbox (same
limitation noted for B.1) — the network-facing pieces
(`decode_request_payload()`'s msgpack parsing, socket handling) weren't
exercised by either harness, only the cJSON-based catalog logic was;
worth a real build/lint pass before this is considered fully done.

### The two flagged behavioral changes

**Dropping static-tool overrides (§9 Q3's resolution) — Implemented
2026-08-16**, by direct instruction ("Implement all the pending items
one after another (all phase 1 items)"), treated as satisfying the
confirmation gate this section had been holding open. `handle_request()`
now checks `is_dynamic` before consulting any caller-supplied `command`
field: for `"static"`-type tools the override is discarded
unconditionally (regardless of whether one was sent, and regardless of
its content) and the catalog's own declared command runs every time;
`"dynamic"` tools are completely unaffected, still taking the
`is_override`/`is_command_safe()` path exactly as before. As predicted,
`is_command_safe()`'s program-pinning branch is now dead code from this
call site specifically (kept, not deleted — see the new comment on its
header) since the call site only ever passes `is_dynamic == 1` now.
`test_init_validation.c`'s three `device_uptime` override checks were
updated exactly as this section anticipated: all three (same-program-
diff-args, different-program, blocklisted) now assert the catalog
command runs and the override is ignored, rather than the old mix of
"executes with the override" / "rejected." A new `simulate_resolved_cmd()`
helper was added to the harness so it can assert *which* command
resolved, not just whether something executed. 17/17 checks pass, no
regressions. Full-file `gcc -fsyntax-only -Wall -Wextra -pthread` against
the real `diag-server-nn.c` stays clean (same two pre-existing cosmetic
warnings only). See §14 item 7 for the full implementation/verification
writeup.

**Inserting the ACL check (`diag_acl_check()`, §13.4) — Implemented
2026-08-15**, by direct instruction. Called from the receive path
immediately after decode, before catalog lookup — on deny, builds the
`{"tool","exit_code":126,"stdout":"access denied"}` response directly
and returns without reaching catalog lookup/execution. **Correction to
this section's own earlier prediction**: it anticipated this could
"only be scaffolded... returning a fixed placeholder result" since
`acl_policy_store_query()` has no real transport yet (§14 item 4, Phase
2). That's not what got built — the call site and `diag_acl_check()`
are real, complete code, still calling the genuine (if unimplemented)
`acl_policy_store_query()` extern, exactly the same way
`dispatcher_command_path.c` already does for every other toolset. The
gap this section predicted is real, but it's a *link*-time gap
(no implementation of `acl_policy_store_query()` exists anywhere in the
codebase yet), not something that required a placeholder inside
diag-server itself — `-fsyntax-only` compiles clean; only linking a
full runnable binary would fail until Phase 2's transport question is
answered. See §13.4's own "Implementation notes" and "Verification"
for the full detail.
