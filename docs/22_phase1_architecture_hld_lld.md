# Phase 1 Architecture — High-Level and Low-Level Design

Companion to `docs/21_phase1_requirements_document.md`. This document
describes how Phase 1's requirements are actually structured into
components, processes, and message flows. See
`phase1_architecture.svg` for the accompanying diagram.

## 0. A discrepancy this document does not silently resolve

`add-triage-skillset-mapping-phase1/design.md` (drafted before the
Phase 1/Phase 2 out-of-process split was finalized) states the Triage
Toolset "must run out-of-process from Dispatch Core" per FR-13.
`ROADMAP.md`'s later, more authoritative statement of Phase 1's scope
says plainly: "Explicitly not required for Phase 1, by design:
out-of-process execution and sandboxing" — stated for the phase as a
whole, not just for the new command-execution slice
`add-phase1-command-execution-exception` later carved out explicitly.

This document takes the position that **Phase 1, in its entirety,
runs in-process** — the Triage Toolset included, not just the new
command-execution/`toolset.push` toolset(s) — on the grounds that
ROADMAP's "not required for Phase 1" statement is broader and later
than the Triage Toolset design's assumption, and that running two
different process models (one out-of-process, one in-process) inside
the same phase for no stated reason would be an unnecessary
inconsistency. **This is a documented assumption, not a re-confirmed
decision** — it should be treated as part of the still-pending
correction to `add-triage-skillset-mapping-phase1` (tracked in
`define-toolset-as-mcp-tool-model/tasks.md` §4), not as new,
independently-settled fact.

## 1. High-Level Design

### 1.1 Component inventory (Phase 1 subset)

| Component | Phase 1 role | Process model |
|---|---|---|
| Parodus Agent | Existing XMiDT/WRP transport endpoint | Existing, unchanged |
| Dispatch Core | JSON-RPC parsing, SAT token validation, single ACL checkpoint, request resolution/routing | In-process (Dispatch Core's own process — this is its home process, not an exception) |
| ACL Policy Store | Named groups, deny-first, write-implies-read, audit log | In-process to Dispatch Core for Phase 1's query path (no separate process required by any Phase 1 requirement) |
| Plugin Manager | Coarse toolset registry: which toolset exists, where its code currently lives | In-process for Phase 1 (§0 assumption) |
| Triage Toolset (discovery) | Answers `triage.capabilities`, merging static + dynamic triage plugins | In-process for Phase 1 (§0 assumption) |
| Command-execution toolset(s) | Handle real commands (P1-4) and `toolset.push` targets (P1-5) | In-process, explicit tracked exception (A15 / P1-6) |
| RDM Client | Verified install/rollback for any `dlopen()`-able binary | Existing, unchanged; not exercised unless a Phase 1 toolset needs a compiled binary update |
| Cloud MCP Gateway / AI-Ops clients | Originates read and command requests | Cloud-side, out of this document's scope |

### 1.2 Data flow — three request shapes Phase 1 supports

**Shape A: read-only discovery (`triage.capabilities`, P1-3).**
Cloud → WRP → Parodus Agent → Dispatch Core (JSON-RPC parse → ACL
check, read-scoped) → Plugin Manager resolves `triage` → Triage
Toolset's `capabilities()` → response retraces the same path back.

**Shape B: real command execution (P1-4).**
Cloud → WRP → Parodus Agent → Dispatch Core (JSON-RPC parse → SAT
token validate → ACL check, write/execute-scoped) → Plugin Manager
resolves `toolset.method` → command-execution toolset runs the method
→ result retraces back. See §2.2 for the exact sequence.

**Shape C: `toolset.push` (P1-5).**
Build/release pipeline → WRP → Parodus Agent → Dispatch Core (JSON-RPC
parse → SAT token validate → ACL check, `toolset-publish` scope) →
inline signature/manifest verification → Plugin Manager loads/updates
the target in-process toolset → health check → synchronous
accept/reject response. See §2.3.

### 1.3 What's explicitly not in this picture

Sandboxing (namespaces, seccomp, cgroups), payload encryption, RBUS/
Thunder Platform Adapters, on-demand process spawning, and the
independent security review are all absent from this diagram on
purpose — they are Phase 2 concerns. Anywhere this document shows a
component running "in-process," that is the current, temporary state,
not the target end-state.

## 2. Low-Level Design

### 2.1 Data structures (from `reference-impl/`, unchanged by this document — cited here for LLD completeness)

```c
// plugin_contract.h
typedef struct {
    const char *plane;
    const char *name;
    const char **events;
    int event_count;
    int timeout_ms;
    const char *load_type;   // "static" | "dynamic"
    const char *version;
} plugin_descriptor_t;

// dispatcher_command_path.c
typedef struct {
    const char *identity;
    const char **groups;
    int group_count;
} caller_identity_t;

// toolset_resolution.c
typedef struct {
    const char *toolset;
    const char *plane;
    const char *process_uds_path;   // Phase 1 note: for an in-process
                                     // toolset (§0), this is a local
                                     // in-process call target, not a
                                     // real UDS path -- see §2.5 below
    bool process_is_running;        // Phase 1 note: always true for an
                                     // in-process toolset; A8's spawn
                                     // logic is not exercised
} toolset_locator_t;
```

### 2.2 Sequence — real command execution (Shape B, P1-4)

1. Dispatch Core receives the WRP envelope, extracts the JSON-RPC
   `payload`.
2. Dispatch Core validates the SAT token (P1-14) — **blocked pending
   A1's confirmation**; this step's exact validation logic cannot be
   finalized until then.
3. `registry_resolve(toolset, method, &loc)` — `toolset_resolution.c`'s
   two-tier lookup: static manifest first, live self-registration
   fallback second (`docs/20` §7). For Phase 1, both tiers resolve
   against in-process registrations, not separate processes.
4. `acl_policy_store_query(caller, toolset, method)` — Dispatch Core's
   single ACL checkpoint (P1-8, FR-4). A denial returns a JSON-RPC
   error (`-32000`) here and stops; nothing below this line runs for a
   denied caller.
5. `toolset_ensure_running(&loc)` — for Phase 1, this is a no-op that
   always succeeds immediately, since the target is already loaded
   in-process. A8's spawn-on-demand logic is not invoked.
6. `toolset_ipc_forward(&loc, method, params_json)` — for Phase 1,
   this is a direct in-process function call, not a real IPC hop over
   a UDS socket (that distinction matters for Phase 2's hardening,
   not for Phase 1's behavior).
7. Result wrapped as a JSON-RPC response, then a WRP envelope, and
   returned along the same path.

### 2.3 Sequence — `toolset.push` (Shape C, P1-5, with Phase 1 rollback)

1. Dispatch Core receives `toolset.push`, validates the caller's SAT
   token and the dedicated `toolset-publish` ACL scope (distinct from
   any `tools/call`-style grant on the toolset's own methods).
2. Inline verification: signature checked against a trusted signer,
   manifest-declared capabilities checked against policy — identical
   standard to RDM Client's own (`define-synchronous-toolset-push`),
   just performed inline rather than via RDM's async pipeline.
3. RDM boundary check (P1-9, A6): if the artifact were a
   `dlopen()`-able binary, this request is rejected outright —
   `toolset.push` never carries one, regardless of size. (What the
   artifact field *does* legitimately carry is still open — C8.)
4. On successful verification, Plugin Manager loads the new version
   in-process **alongside** the currently-serving prior version (both
   resident briefly — feasible only because Phase 1's toolsets are
   small and in-process; this would not scale the same way once
   Phase 2 sandboxing adds per-process overhead).
5. Health check runs against the new version.
   - **Pass:** Plugin Manager switches which version handles
     subsequent calls; the prior version is discarded. Response:
     `{"status": "loaded", ...}`.
   - **Fail:** the prior version keeps serving, unchanged; the new
     version is discarded. Response: a JSON-RPC error naming the
     health-check failure.
6. Dispatch Core responds synchronously with accept/reject — this
   does not wait for a separate, later confirmation signal the way a
   fuller notification-based model might.

### 2.4 Error handling — JSON-RPC error codes used in Phase 1

| Code | Meaning | Raised by |
|---|---|---|
| `-32601` | Method not found | `registry_resolve()` miss on both tiers |
| `-32000` | Access denied | ACL Policy Store query returns false |
| `-32003` | Toolset unavailable | `toolset_ensure_running()` failure (Phase 1: should not occur, since nothing is spawned — a non-trivial failure here indicates a real bug, not an expected on-demand-spawn miss) |
| *(TBD)* | Invalid/expired SAT token | Blocked on A1 — no code assigned yet |
| *(TBD)* | `toolset.push` signature/manifest rejection | Existing `define-synchronous-toolset-push` design specifies a JSON-RPC `error` with a reason but doesn't fix a numeric code |
| *(TBD)* | `toolset.push` health-check failure | Same as above |

### 2.5 A Phase 1-specific note on `toolset_locator_t.process_uds_path`

`toolset_resolution.c` was written generically enough to describe
both Phase 1's in-process reality and Phase 2's eventual
out-of-process model with the same struct shape. For Phase 1,
`process_uds_path` should be read as an internal dispatch key (e.g. a
function-pointer table lookup key), not a literal filesystem path to
a socket — no UDS socket is actually created for an in-process
toolset. This is a naming carryover worth flagging as a minor
low-level inconsistency for implementers, not a design contradiction:
the field's *meaning* ("how do I reach this toolset") is unchanged
between phases, only its *literal contents* differ.

### 2.6 State model — Phase 1 in-process toolset lifecycle

```
[Not loaded] --(Plugin Manager init / toolset.push accepted)--> [Loaded, serving]
[Loaded, serving] --(toolset.push received)--> [New version loading, prior still serving]
[New version loading, prior still serving] --(health check passes)--> [Loaded, serving] (new version now active)
[New version loading, prior still serving] --(health check fails)--> [Loaded, serving] (prior version unchanged, new version discarded)
```

There is no "spawning," "idle-timeout," or "process crashed, awaiting
respawn" state in this model — those belong to Phase 2's on-demand,
out-of-process model (`define-on-demand-toolset-execution`), not to
Phase 1.

## 3. Component-to-file mapping

| Component/behavior | Reference-impl file |
|---|---|
| Plugin loading (static + dynamic merge) | `dispatcher_core.c`, `plugins/triage_core_static.c`, `plugins/triage_wifi.c` |
| Plugin contract | `plugin_contract.h` |
| Read-only discovery response building | `triage_capabilities.c` |
| Command resolution, ACL, dispatch | `dispatcher_command_path.c` |
| Two-tier toolset lookup | `toolset_resolution.c` |
| Housekeeping (referenced, not yet implemented) | `dispatcher_handlers.c`, `dispatcher_triage.c` (P1-13) |

## 4. Deviations from the project's general architecture, listed explicitly

1. Command-execution toolset(s) run in-process, not out-of-process/
   sandboxed (P1-6, A15) — tracked, time-bounded exception.
2. Per §0, this document also treats the Triage Toolset as in-process
   for Phase 1, pending formal reconciliation of an existing
   documentation discrepancy — flagged, not silently assumed away.
3. No on-demand spawning (A8) is exercised — everything Phase 1 needs
   is already loaded.
4. No payload encryption (A7) — plain JSON-RPC, verified, unencrypted.
5. ACL Policy Store and Plugin Manager both run in-process to Dispatch
   Core for Phase 1 — the project's spec doesn't actually require
   these to be separate processes even in the target architecture, so
   this is not itself a deviation, just noted for completeness.
