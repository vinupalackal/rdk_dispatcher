# RDK Dispatcher — Architecture & Requirements Specification

**Scope**: RDK-B (Broadband) and RDK-V (Video) profiles.
**Status**: Design — not yet implemented or reviewed by a security team.
**Depends on / supersedes**: This document consolidates the design work
in `study-docs/16_rpcd_on_rdk_case_study_video_broadband.md`,
`study-docs/17_dispatcher_plugin_architecture_cross_check_and_standalone_implications.md`,
`study-docs/18_rdk_dispatcher_standalone_architecture.md`, and
`rdk_dispatcher_architecture.svg`. Those documents remain the detailed
record of *how* each decision below was reached and what precedent
(`ubus`/`rpcd`, RBUS, Thunder) it's checked against; this document is
the standalone specification — what to build and why, without
requiring the reader to have followed the design conversation.

---

## 1. Background and motivation

`rpcd` and `ubus` are OpenWrt-specific: a message bus (`ubusd`) with
its own binary wire protocol, a plugin daemon (`rpcd`) with a C ABI for
native plugins and a `fork`/`exec` convention for script plugins, a
`uci`-backed config and session/ACL model. None of this has anything
to attach to on RDK: RDK-B's primary bus is RBUS (`rtrouted`, TR-181
data model, PSM-backed config); RDK-V uses IARM-Bus for low-level
events and Thunder/WPEFramework for its plugin-based, JSON-RPC/COM-RPC
service layer. `rpcd` cannot be "ported" to RDK in any meaningful
sense — there is no `ubusd` to register against.

The actual goal this specification serves is narrower and more
achievable: give RDK-B and RDK-V devices a **single, uniform
dispatcher and plugin framework** — modeled on the parts of `rpcd`'s
design that are genuinely good (a clean plugin/ACL/session model), but
implemented from scratch, with no `ubus`/`rpcd` code or wire protocol
— so that cloud-side tooling (AI agents, ops tooling, diagnostics) has
one consistent way to discover and invoke device capabilities across
both profiles, instead of needing profile-specific integration against
RBUS and Thunder directly for every new tool.

## 2. Scope

**In scope**: a device-resident RDK Dispatcher component; a plugin
("toolset") framework for exposing device capabilities; an ACL/session
model; sandboxed plugin execution; a capability-reporting path to the
cloud; adapters bridging toolset plugins into RBUS (broadband) and
IARM/Thunder (video) — the adapter *interface* and its identity/ACL
contract, not the full reimplementation of every RDK subsystem's data
model.

**Out of scope**: replacing RBUS, IARM-Bus, or Thunder; replacing
TR-069/TR-369 ACS-driven management; any code or protocol reuse from
`ubus`/`rpcd` (a deliberate decision — see doc 16, §4, "full
replacement — not realistic, not recommended," and the standalone
decision in doc 17/18).

## 3. Architecture overview

```
CLOUD/WAN
  AI/Ops Clients + MCP Gateway ──┐
  Cloud Tool & Skill Platform  ◀─┴── Device Model Mapping, Tool Catalog
            │  ▲
            │  │  (dashed: capability sync, device-identity auth)
            ▼  │
  XMiDT Transport (WRP / Parodus / WebSocket)
            │  ▲
────────────┼──┼──────────────────────────────────────── DEVICE (RDK-B / RDK-V)
            ▼  │
  Parodus Agent ── RDK DISPATCHER ── Local Clients (UDS, same ACL as cloud)
                     ├─ Dispatch Core        (transport, JSON-RPC, session,
                     │                        AuthN/AuthZ, ACL — single checkpoint)
                     ├─ Plugin Manager        (coarse: toolset→process, health)
                     ├─ Toolset Plugins       (each owns its own method table)
                     ├─ Schema & Discovery    (list/schema/version/capabilities)
                     └─ ACL Policy Store      (groups, deny rules, audit)
            │
            ▼
  Execution Framework (decode → execute resolved plugin → validate/limits)
            │
            ▼
  Sandboxed Toolset Plugin Runtime (namespaces, seccomp, cgroups, dropped caps)
            │
            ▼
  Platform Abstraction & Execution Adapters (forward caller identity)
     RBUS Adapter (RDK-B) │ IARM/Thunder Adapter (RDK-V) │ dmcli/CLI │ HAL/FS/Shell
            │
            ▼
  Linux / RDK OS

  Toolset Store (versioned packages + manifest) ── RDM Client (install/verify/rollback)
```

See `rdk_dispatcher_architecture.svg` for the full visual layout,
including box-level detail this ASCII summary compresses.

## 4. Component specifications

### 4.1 Dispatch Core

Owns: transport termination (the WRP/Parodus/WebSocket connection),
JSON-RPC 2.0 request/response framing, session correlation, AuthN/AuthZ
(SAT-style token validation), and ACL enforcement. This is the
**single, mandatory ACL checkpoint** for the entire device — no other
component (Toolset Plugin, Platform Adapter, Execution Framework) may
implement an independent access-control decision; all of them execute
under the assumption that Dispatch Core has already authorized the
request. Dispatch Core resolves a request only down to *which toolset*
it targets (via Plugin Manager) — it does not perform method-level
dispatch itself.

### 4.2 Plugin Manager

Owns **coarse** state only: which toolsets are currently installed,
which process/socket serves each one, load/unload/reload lifecycle,
and health status. Plugin Manager does **not** maintain a device-wide
table of every method every toolset exposes — that would recreate the
exact centralized-bottleneck problem `ubusd`, `rtrouted`, and Thunder's
transport-layer plugin selection all avoid by design (see doc 17,
§1.4). Plugin Manager's registry changes only on load/unload/reload
events — it is small and stable by construction.

### 4.3 Toolset Plugins

The actual capability implementations (common, network, Wi-Fi, DOCSIS,
vendor-supplied categories, extensible). Each Toolset Plugin process
owns **its own** method-name-to-handler dispatch table internally —
this is the "owner" half of the broker/owner split, exactly matching
how `rpcd` itself dispatches methods inside the owning process, how an
RBUS provider handles its own registered dataElements, and how a
Thunder plugin's `Register()`-built table handles its own JSON-RPC
methods. Toolset Plugins run **out-of-process** (never `dlopen()`'d
into Dispatch Core), for crash isolation, and **inside a sandbox**
(§4.7) for privilege isolation.

### 4.4 Schema & Discovery

Exposes `list()`, `schema()`, `version()`, and `capabilities()` for
every currently-loaded toolset. This is the self-description mechanism
both for local tooling and for the device→cloud capability-sync path
(§5.2) — a toolset's schema is authoritative from the toolset itself,
not duplicated into Plugin Manager's registry.

### 4.5 ACL Policy Store

A dedicated, queryable facility — not an implementation detail buried
inside Dispatch Core. Modeled directly on the two things `rpcd`'s own
ACL system got right: named, declarative permission groups
(`{scope: {toolset: [methods...]}}`, split into `read`/`write`), and a
separate mapping from an authenticated identity to which groups it
holds. Required behavior:

- Negative/deny rules evaluated **before** allow rules.
- "Write implies read" as the default fallback policy.
- Hot-reloadable group definitions, without restarting Dispatch Core.
- A runtime query API (`would identity X be allowed to call
  toolset.method?`) usable by Dispatch Core for its own enforcement
  and by Platform Adapters for identity-scoped decisions before
  forwarding a call into RBUS/Thunder.
- An audit trail of every access decision, feeding Dispatch Core's
  metrics/audit function.

### 4.6 Execution Framework

Receives an already-resolved plugin handle (never resolves one
itself). Three stages: decode the request against the target toolset's
schema; hand off `(method, args)` to the resolved out-of-process
plugin over a defined boundary (a length-prefixed JSON message over a
Unix domain socket per plugin process); enforce timeouts, resource
limits (backed by the cgroup described in §4.7), output size limits,
and command validation around that call.

### 4.7 Sandboxed Toolset Plugin Runtime

Every Toolset Plugin process launches inside, at minimum:

- Its own mount and PID namespace (no visibility into the Dispatcher's
  or other plugins' filesystem/process tree).
- A `seccomp-bpf` syscall allowlist, defined **per plugin**, not
  globally — a DOCSIS toolset's required syscalls differ from a
  stateless "common" toolset's.
- A `cgroup` enforcing the CPU/memory limits Execution Framework
  declares — turning "resource limits" into an actually-enforced
  kernel mechanism rather than a stated intention.
- A non-root UID with Linux capabilities dropped to the minimum the
  plugin's manifest declares needing.
- Read-only access to everything except an explicitly granted scratch
  path.

A plugin's required capabilities, device-node access, and target
adapter(s) are declared in its **manifest**, checked by RDM Client at
install time (§4.9) and enforced by the sandbox at every launch — not
renegotiated per call.

### 4.8 Platform Abstraction & Execution Adapters

Four adapters, kept structurally separate (not merged into one generic
adapter, and not merged into the Dispatcher): **RBUS Adapter**
(RDK-B, TR-181 parameter tree get/set/method-invoke), **IARM/Thunder
Adapter** (RDK-V, COM-RPC and/or JSON-RPC), **dmcli/CLI Adapter**
(lowest-common-denominator fallback), **HAL/FS/Shell** (vendor
libraries, `/proc`, a shell-executor fallback). Each adapter is
**required** to forward or re-derive the original caller's identity
into the underlying subsystem's own native ACL check (RBUS's own ACL,
Thunder's Security Agent/permissions file) — an adapter must never run
under one fixed, privileged service identity that only the
Dispatcher's upstream check gates. This requirement exists specifically
to close the confused-deputy risk identified in doc 16 as a blocking
concern, not an optional hardening step.

### 4.9 Toolset Store and RDM Client

Toolset Store holds versioned toolset packages plus their manifests
(sandbox/capability requirements, target platform, schema). RDM Client
handles download, signature verification, install, and rollback,
feeding Plugin Manager's load/reload actions when a new or updated
toolset goes live. RDM Client is the enforcement point for manifest
capability checks — a toolset requesting capabilities beyond policy
must fail installation, not fail silently at runtime.

### 4.10 Local Clients (UDS)

Local, on-device callers (daemons, scripts, system services) connect
over a Unix domain socket and are routed through the **same** Dispatch
Core ACL check as any cloud-originated command. This is a deliberate
divergence from `rpcd`, where a plain local `ubus call` bypasses ACL
entirely unless the caller explicitly invokes `session.access` — that
gap is not reproduced here.

### 4.11 Transport and cloud-side components

Parodus Agent and the XMiDT transport (WRP/WebSocket) are existing RDK
infrastructure, reused unmodified. On the cloud side, the MCP Gateway
and Cloud Tool & Skill Platform (skills library, tool catalog, device
model mapping) are existing/assumed infrastructure this specification
treats as the consumer of both the command-execution and
capability-sync flows.

## 5. Data flows

### 5.1 Command execution (cloud/ops → device)

1. Cloud/ops client sends a JSON-RPC request over XMiDT to the device's
   Parodus Agent, which hands it to Dispatch Core.
2. Dispatch Core validates the SAT token, checks ACL Policy Store once
   for `(identity, toolset, method)`.
3. On allow, Dispatch Core asks Plugin Manager which process serves
   the target toolset, and passes the resolved handle to Execution
   Framework.
4. Execution Framework decodes/validates the request against the
   toolset's schema and forwards `(method, args)` to the sandboxed
   plugin process over the UDS/JSON boundary.
5. The plugin's own internal method table resolves and executes the
   call, reaching RBUS/Thunder/CLI/HAL through the relevant adapter,
   which forwards the caller's identity into that subsystem's own ACL.
6. The result retraces the path back to the cloud client.
7. Every ACL decision along the way is recorded to the audit trail.

### 5.2 Capability sync (device → cloud)

1. Triggered on a Plugin Manager load/unload/reload event (not polled).
2. Plugin Manager notifies Schema & Discovery to refresh the affected
   toolset's `capabilities()`/`schema()`.
3. Dispatch Core sends the updated manifest over the same Transport
   Adapter and XMiDT connection used for commands, but authenticated
   by **device identity** (e.g., mTLS), not the per-session SAT token.
4. The Cloud Tool & Skill Platform updates Device Model Mapping and
   the Tool Catalog for that device.

## 6. Requirements

### 6.1 Functional requirements

| ID | Requirement |
|---|---|
| FR-1 | The Dispatcher shall accept JSON-RPC 2.0 requests over the existing XMiDT/WRP/Parodus transport, with no new cloud-facing transport introduced. |
| FR-2 | The Dispatcher shall support loading, unloading, reloading, and health-checking toolset plugins without a full device restart. |
| FR-3 | Each toolset plugin shall expose its own schema via `list()`/`schema()`/`version()`/`capabilities()`, independent of any central registry. |
| FR-4 | The Dispatcher shall provide a single, centralized ACL enforcement point; no other component may independently gate access. |
| FR-5 | The ACL Policy Store shall support named permission groups, `read`/`write` scoping, deny rules evaluated before allow rules, and a write-implies-read fallback. |
| FR-6 | ACL group and identity-mapping changes shall take effect without restarting Dispatch Core. |
| FR-7 | The Dispatcher shall report installed toolset capabilities to the cloud automatically on load/unload/reload, over the same transport used for commands. |
| FR-8 | Local (UDS) clients shall be subject to the same ACL enforcement as cloud-originated requests. |
| FR-9 | The RBUS Adapter, IARM/Thunder Adapter, and dmcli/CLI Adapter shall remain structurally independent components, each owning its own subsystem-specific translation logic. |
| FR-10 | Every adapter shall forward or re-derive the original caller's identity into its underlying subsystem's native ACL check. |
| FR-11 | Toolset packages shall declare required capabilities, device-node access, and target adapter(s) in an install-time manifest. |
| FR-12 | RDM Client shall verify a toolset's signature and manifest-declared capabilities against policy before install, and support rollback to a prior version. |
| FR-13 | Each toolset plugin shall execute in its own OS process, never loaded into the Dispatcher's own address space. |
| FR-14 | Each toolset plugin process shall execute inside a sandbox enforcing, at minimum, mount/PID namespace isolation, a per-plugin seccomp-bpf profile, cgroup-enforced resource limits, and least-privilege capabilities per its manifest. |

### 6.2 Non-functional requirements

| ID | Requirement |
|---|---|
| NFR-1 (Portability) | The Dispatcher, Plugin Manager, Execution Framework, and ACL Policy Store shall be identical across RDK-B and RDK-V; only the Platform Adapter layer shall differ per profile. |
| NFR-2 (Isolation) | A crash or resource exhaustion in any single toolset plugin shall not affect Dispatch Core, Plugin Manager, or any other toolset plugin. |
| NFR-3 (Least privilege) | No toolset plugin shall run with more Linux capabilities, syscall access, or filesystem visibility than its manifest declares needing. |
| NFR-4 (No confused deputy) | No Platform Adapter shall be able to complete a call to RBUS or Thunder using a fixed, elevated identity that bypasses that subsystem's own native ACL for the original caller. |
| NFR-5 (Auditability) | Every ACL allow/deny decision, at every enforcement point (Dispatch Core, and any adapter-level re-check), shall be logged with the requesting identity, target, and outcome. |
| NFR-6 (Availability of coarse routing) | Plugin Manager's toolset→process registry shall remain small and cheap to query regardless of the number of methods any individual toolset exposes. |
| NFR-7 (Independent evolution) | A toolset plugin shall be able to add, remove, or version its own methods without requiring a change to Plugin Manager's registry schema or implementation. |
| NFR-8 (No upstream dependency) | The Dispatcher shall not depend on `ubus`, `ubusd`, or any `rpcd` code, headers, or wire protocol. |
| NFR-9 (Session resilience) | Command-session validity shall not depend on Dispatch Core retaining in-memory state across a restart (i.e., stateless token validation, not a server-side session table requiring freeze/thaw). |
| NFR-10 (Revocation) | Given NFR-9's stateless tokens, the design shall provide either short token expiry with refresh, or an explicit revocation list, to bound the impact of a compromised token. |

## 7. Design decisions log

| Decision | Rationale | Source |
|---|---|---|
| No `rpcd`/`ubus` code or protocol reuse; standalone implementation | RDK has no `ubusd` broker to attach to on either profile; a genuine port has nothing to register against | Doc 16 §2–3 |
| Bridge into RBUS/Thunder via adapters, don't replace them | Full replacement means rewriting every CCSP/RBUS component and Thunder plugin — not viable | Doc 16 §4 |
| Broker/owner dispatch split: Plugin Manager coarse, each plugin owns its own method table | Matches `ubusd`/`rtrouted`/Thunder precedent independently; avoids centralizing method-level dispatch into a single bottleneck | Doc 17 §1.4, Doc 18 §5 |
| Toolset plugins run out-of-process | Crash isolation; matches RBUS's always-running-provider model over `rpcd`'s in-process native model | Doc 17 §2.3, Doc 18 §5 |
| Toolset plugins additionally sandboxed (namespaces/seccomp/cgroups/cap-drop) | Out-of-process alone gives isolation from crashes, not from privilege abuse; toolsets may be vendor-supplied via Toolset Store | This document §4.7 |
| ACL enforced exactly once, in Dispatch Core, against a dedicated ACL Policy Store | Matches `rpcd`'s single `session.access` checkpoint; a dedicated store avoids duplicating policy logic across adapters/RDM Client | Doc 18 §5 |
| Adapters forward caller identity into RBUS/Thunder's own ACL | Prevents a Dispatcher-level bug from silently bypassing RBUS's/Thunder's independently-audited ACL (confused deputy) | Doc 16 §7, Doc 18 §5 |
| Local UDS clients get the same ACL check as cloud clients | `rpcd` has a known, real gap here (local `ubus call` bypasses ACL); intentionally not reproduced | Doc 18 §5 |
| Stateless SAT/JWT token for commands; device-identity auth for capability sync | Avoids `rpcd`'s session-loss-on-restart weakness; separates two different trust relationships | Doc 18 §5 |
| Capability sync is event-triggered, not polled, and reuses the command transport | Matches Plugin Manager's existing load/unload/reload lifecycle hooks; avoids a second channel | Doc 18 §3 |

## 8. Open questions / follow-on work

- **Token format and revocation mechanism** for the stateless SAT model
  (NFR-10) is not yet chosen — JWT with short expiry + refresh, versus
  an explicit revocation list, has different operational tradeoffs not
  yet evaluated against RDK's existing device-identity infrastructure.
- **RBUS and Thunder bridge implementations** are specified here only
  at the adapter-interface/identity-propagation level (FR-9, FR-10,
  NFR-4); the actual per-toolset translation logic (which TR-181
  parameters or Thunder plugins each toolset maps to) is unscoped and
  will need to be designed per toolset.
- **Sandbox profile authoring workflow** — who writes and reviews a new
  toolset's seccomp/capability manifest, and what happens when a
  legitimate plugin needs a profile change post-install — is not yet
  defined.
- **Footprint budget** for running Plugin Manager, Dispatch Core, and
  N sandboxed toolset plugin processes alongside RDK's existing
  CCSP/Thunder process load has not been sized against real target
  hardware.
- **Security review**: this specification has not yet been reviewed by
  a dedicated security team; NFR-4 (no confused deputy) and NFR-3
  (least privilege) in particular should be independently verified,
  not just design-reviewed, before any production deployment.
