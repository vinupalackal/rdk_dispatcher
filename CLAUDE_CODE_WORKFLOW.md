# Building the RDK-B Dispatcher with Claude Code — Workflow Guide

This is the day-to-day *coding* workflow for this project, complementing
`USER_GUIDE.md` (which covers the OpenSpec side — deciding *what* to
build). This guide covers *how* to actually build it in Claude Code,
using the scaffold already committed in this folder: `CLAUDE.md`,
`.claude/commands/`, `.claude/settings.json`, `.claude/agents/`,
`.claude/skills/`, and `reference-impl/`.

A note on scope: this guide describes the dispatcher as an **embedded
C, CCSP/HAL-based RDK-B component** (sysevent, Netlink, Yocto/bitbake)
— a real, concrete answer to next-step #2 in `USER_GUIDE.md`
("decide the implementation language and runtime for Dispatch Core"),
distinct from and narrower than the profile-agnostic
`RDK_Dispatcher_Architecture_and_Requirements.md` spec (which covers
both RDK-B and RDK-V, and stays deliberately silent on implementation
language). Treat this as one concrete build-out — the RDK-B side
specifically — of that broader spec, not a replacement for it. The
"four planes" framing here (config / management / control / triage)
maps onto that spec's components roughly as: config-apply and
management ↔ `toolset-lifecycle`, control ↔ `dispatch-core` +
`platform-adapters`, and triage is new — a capability the OpenSpec
domains don't yet cover and worth adding as its own `spec.md` if this
build-out proceeds (see "Open threads" at the end of this guide).

## 1. `CLAUDE.md` — already in place at the repo root

`CLAUDE.md` (this folder) captures the project's real hard rules: never
block the sysevent notification thread, always check HAL return codes,
free `sysevent_set()` buffers in the same function scope, keep event
handlers and triage-record logic in separate files, enforce a timeout
on every plugin call, and require every plugin to implement both
`describe()` and `handle()` or fail discovery loudly. Claude Code reads
this automatically on every session in this repo — it doesn't need to
be re-explained per conversation.

## 2. `/clear` between unrelated handlers

Context from one handler (e.g. WAN link-down retry logic) can bleed
into and confuse work on an unrelated one (e.g. DHCP lease renewal).
When you notice that happening, `/clear` and restate the next task
plainly:

```
You: Add dispatch_dhcp_lease_renew() following the pattern in dispatcher_handlers.c,
     per CLAUDE.md
```

## 3. Plan Mode before touching dispatcher files

For anything nontrivial, let Claude Code draft a plan before editing
any file — this is where you catch a missing HAL confirmation call or
a triage capture that would incorrectly block the handler, before any
code exists:

```
PLAN:
1. Add dispatch_wifi_radio_reset() in dispatcher_handlers.c
2. Register it against sysevent "wifi-radio-reset" in dispatcher_init()
3. Call T2_event_s() to push a telemetry marker
4. Start a completion-timeout timer; on timeout, call
   triage_capture_wifi(trace_id, last_hal_rc, event_window)
5. Triage capture is enqueued async -- does not block the control-plane handler
6. No new sysevent names needed -- reuses existing "wifi-radio-reset" event

Files touched: dispatcher_handlers.c, dispatcher_init.c, dispatcher_triage.c
HAL/CCSP components touched: none -- T2 telemetry + triage capture only
```

## 4. Custom command — `.claude/commands/explain-handler.md`

Run `/explain-handler` instead of retyping the same review prompt every
time you want to know what the most recently edited handler does: what
triggers it, what it calls downstream, its failure modes, and whether
it's registered/deregistered correctly — under 120 words, read-only.

## 5. Hooks — `.claude/settings.json`

Two post-tool hooks run automatically on every `Edit`/`Write`:
`cppcheck` plus the dispatcher test target on any change to
`dispatcher_handlers.c`/`dispatcher_triage.c`, and a schema validator
specifically on `dispatcher_triage.c` — catching a triage record
missing `trace_id`, or one that's drifted from the pipeline's expected
JSON shape, immediately rather than after it silently breaks the
evidence collector downstream. One pre-tool hook blocks Claude Code
from ever kicking off a full multi-hour `bitbake rdk-generic` image
build on its own.

## 6. Git worktrees for comparing approaches in parallel

When comparing strategies — e.g. three ways to debounce a flapping
WAN-link-down event — build each in an isolated worktree and compare
results rather than iterating on one branch:

```sh
git worktree add -b debounce-timer ./disp-timer
git worktree add -b debounce-counter ./disp-counter
git worktree add -b debounce-none ./disp-baseline
```

Each builds against real sysevent test fixtures in its own terminal;
compare flap counts and pick a winner.

## 7. Sub-agents — `.claude/agents/hal-agent.md`, `.claude/agents/triage-agent.md`

Two scoped sub-agents keep risky or independently-evolving code
reviewable in isolation:

- **HAL Interface Agent** — touches only `hal_*.c` files, never
  dispatcher event-routing logic. Keeps the "talks to hardware"
  boundary isolated.
- **Triage Capture Agent** — touches only `dispatcher_triage.c`.
  Changes to the evidence schema/format can't accidentally touch
  control-plane logic.

## 8. Skills — `.claude/skills/`

Two skills, split by layer:

- **`ccsp-component-conventions`** — CCSP sysevent-handler conventions
  (register in `_init()`/deregister in `_deinit()`, always check HAL
  return codes, naming conventions, telemetry via `T2_event_s()`) plus
  when and how to add triage capture (shared `trace_id`, async
  enqueue, what evidence to include).
- **`dispatcher-protocol-conventions`** — Dispatch Core's own request
  path: decrypt-before-route, message-kind classification
  (`definition`/`command`, explicitly not the same axis as a plugin's
  `load_type`), `tools/list`/`tools/call` sourcing and ACL rules,
  `toolset.push`'s verification/rollback/ACL-scope requirements, and
  the plane-vs-toolset boundary for what must run out-of-process and
  sandboxed. Sourced from the four dispatcher-core-level OpenSpec
  changes drafted in this project — read it before touching anything
  in Dispatch Core, Plugin Manager, or the MCP surface.

Either way, "add a handler for X" or "wire up method Y" follows these
conventions automatically instead of needing to be restated every
time.

## 9. MCP servers for live protocol docs

Configuring an MCP server (e.g. Context7) in `.claude.json` lets a
question like "what's the correct NLMSG parsing pattern for
RTM_NEWLINK" pull current Netlink API docs live, rather than relying on
training data that can drift stale against kernel networking APIs
across versions. Verify a configured server is active with `/mcp`.

## 10. Plugin architecture (rpcd-style) — `reference-impl/`

The core loader and example plugins are sketched in `reference-impl/`
(`plugin_contract.h`, `dispatcher_core.c`, `plugins/triage_wifi.c`,
`plugins/triage_core_static.c`, `triage_capabilities.c`) —
**illustrative sketches, not reviewed production code.** The static
(`triage_core_static.c`) vs. dynamic (`triage_wifi.c`) pairing and the
`triage_capabilities.c` merge/response builder were added for Phase 1
— see `openspec/changes/add-triage-skillset-mapping-phase1/`. Dispatcher core does discovery and routing only;
each of the four planes is an independently loadable `.so`, discovered
at init from `/usr/libexec/dispatcher/`. The contract mirrors `rpcd`'s
`list`/`call` pair almost exactly: `describe()` is `list`, `handle()`
is `call`.

Why this maps well to `rpcd`'s model (see `docs/17_dispatcher_plugin_architecture_cross_check...`
for the deeper comparison this project already did against `ubus`/RBUS/Thunder):

- **Discovery over static registration** — core never hardcodes what
  handlers exist, same as `rpcd` never hardcodes plugin methods.
- **Per-call isolation** — `run_with_timeout()` gives each plugin call
  a bounded-time guarantee without a full process boundary. Trade-off,
  stated plainly: less isolation than `rpcd`'s real `fork`/`exec` for
  script plugins, but lower overhead — a reasonable choice for an
  embedded gateway's event-loop budget, not a free equivalent.
- **Plane-based ACL equivalent** — which plugins may write to the
  triage/MQTT path vs. only touch HAL is the same kind of gate `rpcd`'s
  ACL files apply to which `ubus` methods a plugin may expose.
- **Fail loud, not partial** — a plugin missing `describe()` or
  `handle()` is skipped with a logged error, never half-loaded —
  mirrors `rpcd` refusing to register a malformed plugin `list` output.

**Trade-off to flag explicitly**: `dlopen()`-based plugin loading adds
indirection and a small runtime cost versus statically linking
everything in. Worth it once several teams are independently shipping
plane-specific logic (triage-capture plugins evolving separately from
control-plane plugins); probably not worth it for one team, one
binary, low plugin churn — the compiled-in approach is simpler to
statically audit and debug in that case.

## Suggested build order

1. `CLAUDE.md` capturing the team's real hard rules across all four
   planes — already done here; revise as real bugs surface.
2. Plan Mode → sketch each new handler and its triage-capture path
   before touching `dispatcher_handlers.c`/`dispatcher_triage.c`.
3. Set up the cppcheck + unit-test + triage-schema hooks (§5) early —
   already configured in `.claude/settings.json`.
4. Add `/explain-handler` (§4) once you're re-explaining a handler's
   flow more than once — already scaffolded.
5. Split into the HAL and triage sub-agents (§7) once control-plane
   and evidence-capture logic are big enough to review separately —
   already scaffolded.
6. Reach for worktrees (§6) when comparing debounce/retry strategies
   or triage-capture windows in parallel.
7. Decide whether the `dlopen()`-based plugin loader (§10) is worth it
   yet: start compiled-in if it's one team/one binary; migrate to
   dynamic plugin discovery once multiple teams ship plane-specific
   logic independently and static linking becomes the bottleneck.

## Open threads (worth resolving before this build-out goes further)

- **Triage as a fifth OpenSpec domain — Phase 1 started, not complete.**
  `openspec/changes/add-triage-skillset-mapping-phase1/` now scopes and
  drafts a `triage` domain, but deliberately only for capability
  discovery (`triage.capabilities` over WRP) — not evidence capture,
  `trace_id` correlation, or async enqueue, which is what
  `triage_wifi.c`'s `handle()` actually does. That behavior still has
  no OpenSpec coverage and remains open.
- **Isolation trade-off vs. the broader spec's sandboxing requirement —
  reopened, not resolved.** `openspec/changes/define-plane-vs-toolset-model/`
  was revised on 2026-08-13: there is no first-party exemption for
  plane logic after all — every plane (config-apply, management,
  control, triage) is implemented *as* a toolset, uniformly governed by
  FR-13/FR-14 like any other. That means this guide's `run_with_timeout()`
  + in-process `dlopen()` model is **not** compliant as written and
  needs rework toward the out-of-process, sandboxed, Plugin-Manager-supervised
  model — not a documentation fix, an actual code-shape change. The
  real, unresolved tension this creates (IPC/latency overhead on a
  sysevent thread that must never block, per this file's own hard
  rules) is tracked against `OPEN_QUESTIONS.md` B3 (footprint budget)
  and `define-plane-vs-toolset-model/tasks.md` §3 — treat this
  reference-impl as needing a rework pass, not as a finished sketch.
