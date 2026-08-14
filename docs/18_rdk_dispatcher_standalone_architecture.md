# RDK Dispatcher — Standalone Architecture (No `rpcd` / `ubus`)

This document formalizes the architecture shown in
`rdk_dispatcher_architecture.svg`, incorporating the specific changes
requested against the original reference diagram: the IPC interfaces
stay separate, the Execution Framework becomes an explicit
decode-and-route step fed by the dispatcher, a device→cloud capability
reporting path is added on the same transport, and — critically — the
component previously drawn as "rpcd Core (Dispatcher)" is replaced by
a new, standalone **RDK Dispatcher** that does not depend on `rpcd` or
`ubus` at all. This builds directly on the conclusions of
`16_rpcd_on_rdk_case_study_video_broadband.md` (bridging is the only
sound way to reach real RDK functionality) and
`17_dispatcher_plugin_architecture_cross_check_and_standalone_implications.md`
(what it actually means to build the dispatcher piece as its own,
from-scratch component).

## 1. What the RDK Dispatcher actually is

Per the explicit grouping given: the RDK Dispatcher is the union of
four boxes that were drawn separately in the original diagram —
**rpcd Core** (relabeled as the dispatcher's own request-handling
internals: transport adapter, JSON-RPC 2.0 engine, session &
correlation, AuthN/AuthZ via SAT tokens, ACL & policy enforcement,
audit & metrics), **Plugin Manager** (load/unload/reload, health
checks, dependency management), **Toolset Plugins** (the actual
skill/tool implementations — common, network, Wi-Fi, DOCSIS, vendor),
and **Schema & Discovery** (`list()`/`schema()`/`version()`/
`capabilities()`). Grouping these four into one owned component is a
naming and ownership decision, not just cosmetic: it says one team/
codebase is responsible for the full path from "a JSON-RPC request
arrives" to "the right toolset plugin's schema is known and its method
gets dispatched" — with **no dependency on `ubus`'s wire format,
`ubusd` as a broker, or any of `rpcd`'s C ABI (`struct rpc_plugin`,
`dlsym(dlh, "rpc_plugin")`)** anywhere in that path. Everything from
this project's earlier `rpcd`/`ubus` work informs the *design* here
(the broker/owner dispatch split from doc 17, the native/script
plugin tradeoffs), but none of the actual OpenWrt code or protocol
carries over, exactly as decided in the prior turn.

## 2. The Execution Framework becomes an explicit decode-and-route step

In the original diagram, the Execution Framework sat below the
Toolset Plugins as a set of safety controls (timeouts, resource
limits, output size limits, command validation) — its *input* wasn't
drawn explicitly. This design makes that input explicit and gives it
three named stages, directly answering "expand the command executing
framework with dispatcher input and decoding":

```
RDK Dispatcher (Dispatch Core)
        │  decoded invocation: {toolset, method, args, session}
        ▼
┌─────────────────────────────────────────────┐
│           EXECUTION FRAMEWORK                 │
│                                                │
│  1. Decode dispatcher request                 │
│     (validate shape, resolve args against the  │
│      target toolset's schema from Schema &     │
│      Discovery)                                │
│                                                │
│  2. Resolve target plugin                      │
│     (look up the specific Toolset Plugin       │
│      instance the Plugin Manager already has   │
│      loaded/healthy for this request)          │
│                                                │
│  3. Validate & execute                         │
│     (timeouts, resource limits, output size    │
│      limits, command validation — then the     │
│      actual call into the plugin)              │
└─────────────────────────────────────────────┘
        │
        ▼
   Platform Abstraction & Execution Adapters
```

This mirrors the broker/owner dispatch split identified in doc 17
(§1.4): the RDK Dispatcher's Dispatch Core is the cheap, name-based
routing layer (which toolset, which method) — same role as `ubusd`'s
AVL-tree lookup or `rtrouted`'s dataElement routing — and the
Execution Framework is where the actual, more expensive work (schema
validation, resource-limited execution) happens, analogous to how
`rpcd` itself does method dispatch and script-plugin `fork`/`exec`
only after `ubusd` has already routed the message to it. Keeping this
split explicit avoids the "centralized dispatcher becomes a
bottleneck" trap flagged in that same document.

## 3. Device → cloud capability reporting, on the same transport

The requirement was to "send all the plugin support from the device to
cloud using the same dispatcher framework" — not a separate channel.
The design reuses the existing XMiDT/WRP/Parodus transport for a
second, distinct traffic pattern:

```
Schema & Discovery            RDK Dispatcher              XMiDT              Cloud Tool & Skill Platform
(capabilities(), list())  ──▶  (Dispatch Core,        ──▶  Transport      ──▶  Device Model Mapping,
 per installed toolset          same transport             (WRP/Parodus)       Tool Catalog & Skills
 plugin                         adapter as commands)
```

Two things make this a *second pattern* rather than just another RPC
call: it's **dispatcher-initiated, not cloud-initiated** (the normal
command flow is cloud/ops asking the device to do something; capability
sync is the device proactively telling the cloud what it *can* do), and
it should be **triggered on a real event, not polled** — specifically,
whenever the Plugin Manager loads, unloads, or reloads a toolset
plugin (already one of its listed responsibilities), it's the natural
trigger to re-run Schema & Discovery and push an updated capability
manifest upward. This is also exactly the information the cloud side
already has a place for: "Device Model Mapping" and "Tool Catalog &
Skills" in the Cloud Tool & Skill Platform box existed in the original
diagram specifically to receive this kind of per-device capability
data — this design just makes explicit that the RDK Dispatcher, not
some separate agent, is what produces and sends it, over the same
Transport Adapter already handling command traffic.

## 4. The IPC interfaces stay separate — by design, not by omission

The explicit instruction was to keep RBUS, IARM/Thunder, and dmcli/CLI
as separate adapters rather than merging them into the RDK Dispatcher
or into each other. This is consistent with, not in tension with,
grouping the dispatcher-side components together in section 1: the
Platform Abstraction & Execution Adapters row is a different kind of
boundary — it's where a *decoded, already-validated* plugin call
finally reaches a real RDK subsystem, and each of those subsystems
(RBUS for broadband/TR-181, IARM/Thunder's COM-RPC/JSON-RPC for video,
`dmcli`/CLI as a lower-common-denominator fallback) has its own wire
protocol, its own ACL system, and its own failure modes, as detailed
in doc 16. Collapsing them into one generic adapter would hide exactly
the differences that matter for correctness — a Wi-Fi toolset plugin
talking to RBUS needs to handle TR-181's typed parameter tree; the
same plugin category on a video device talking through the IARM/
Thunder adapter needs to handle COM-RPC's interface/vtable model
instead. Keeping them as visibly separate boxes is what makes each
one's specific bridging logic (the plugin work identified as still
necessary in doc 16, regardless of which runtime hosts it) tractable
to review and test independently.

## 5. Design decisions this architecture must state explicitly

A review against `rpcd`/`ubus`'s own behavior (which earned its
clarity through years of being forced to answer these exact questions)
surfaced six places where the design above was implicit or
underspecified. Each is resolved here as an explicit decision, not
left to whoever implements it first.

**Plugin resolution is a hybrid, split the same way `ubus`/RBUS/Thunder
all split it: coarse routing centralized, fine dispatch owned locally.**
An earlier pass of this fix said "Plugin Manager is the single
authoritative registry mapping a toolset/*method* name to a handle" —
that overcorrects. `ubusd` never does method-level dispatch at all; it
only routes by object path, and the *owning process* keeps its own
method table and does the string-compare lookup itself. RBUS's
`rtrouted` and Thunder's plugin selection follow the identical pattern
one level up. This design mirrors that precisely instead of
centralizing everything: **Plugin Manager** is authoritative only over
*coarse* state — which toolsets exist, which process/socket currently
serves each one, load state, health — small, stable data that changes
only on load/unload/reload. **Each Toolset Plugin process** is
authoritative over its *own* method-name-to-handler table, exactly
matching what its own `Schema & Discovery` `list()`/`schema()`
response already implies is self-described, not centrally tracked.
Dispatch Core resolves `(toolset, method, args)` down to "which
process serves `toolset`" via Plugin Manager, checks ACL once, and
Execution Framework forwards `(method, args)` to that process, where
its own internal dispatch resolves the method. Plugin Manager never
touches method names; no plugin touches ACL; Dispatch Core never does
method-level dispatch — three non-overlapping authorities instead of
either the original three-way ambiguity or an accidental single
bottleneck.

**ACL enforcement happens exactly once, and everything downstream
trusts it.** `rpcd` checks `session.access` centrally, before either
plugin type runs, and no plugin re-implements its own auth check.
Same rule here: Dispatch Core's ACL & Policy Enforcement is the
**only** checkpoint. Toolset Plugins and the Execution Framework must
not add a second, independent check — that would create two sources
of truth for "is this allowed," the exact failure mode `rpcd` avoids
by checking once and having every plugin type trust the result.

**Toolset Plugins run out-of-process, deliberately, not by default.**
Plugin Manager's own listed responsibilities — health check,
dependency management — only make sense if plugins are separate,
supervisable processes; a `dlopen()`'d-in plugin sharing the
Dispatcher's address space can't be "health checked" independently of
the Dispatcher itself. This design chooses out-of-process explicitly
(closer to RBUS's always-running-provider model, or `rpcd`'s script
plugins, than to `rpcd`'s native/`dlopen()` model) precisely for the
crash-isolation property doc 17 called out: a failing Wi-Fi toolset
plugin must not take the Dispatcher down with it.

**That choice requires a defined serialization boundary.** Given
out-of-process plugins, Execution Framework needs an actual protocol
to hand off a call and get a result back — analogous to `rpcd`'s
JSON-over-stdio-pipe convention for script plugins. This design
specifies a local, length-prefixed JSON message over a Unix domain
socket per plugin process (chosen for simplicity and debuggability;
revisit only if profiling shows it's a bottleneck) rather than leaving
this hop unspecified.

**Platform Adapters must forward the original caller's identity, not
run as a blanket-trusted service account.** This is the single most
important fix, because doc 16 named it a *blocking* risk, not a
nice-to-have: an RBUS or Thunder adapter call must carry (or
re-derive) the original request's identity into RBUS's/Thunder's own
native ACL check. An adapter that runs under one fixed, privileged
identity and trusts only the Dispatcher's upstream check would let a
Dispatcher-level bug or misconfiguration silently bypass RBUS's/
Thunder's own, independently-audited ACL — a textbook confused-deputy
path. This design requires per-call identity propagation through
every adapter as a hard constraint, not an optimization.

**Command auth and capability-sync auth are different trust
relationships, and use different mechanisms on purpose.** Dispatch
Core's AuthN/AuthZ layer uses a SAT-style token for command execution
— a user/ops session acting *through* the Dispatcher. The
capability-sync path is the device itself asserting facts *to* the
cloud, a different relationship, authenticated instead by device
identity (e.g., mTLS over the XMiDT transport), not a per-session
token. Separately: this design adopts a **stateless** SAT/JWT-style
token for command sessions rather than `rpcd`'s stateful, in-memory
AVL-tree session store — deliberately trading `rpcd`'s known weakness
(sessions vanish on restart unless explicitly frozen/thawed to disk)
for the classic stateless-token tradeoff instead (revocation requires
short expiries plus a refresh flow, or an explicit revocation list,
since there's no server-side session to simply delete).

**Local Clients (UDS) get the same ACL check as everyone else — this
does not inherit `rpcd`'s known gotcha.** A real, documented `rpcd`
behavior: a plain local `ubus call` bypasses ACL entirely, because
nothing forces a local caller to go through `session.access` the way
`uhttpd`'s bridge does. This design closes that gap on purpose: Local
Clients (UDS) route through the same Dispatch Core ACL & Policy
Enforcement as any cloud-originated command — no local-socket
exemption.

**Toolset Plugins execute inside a sandbox, not just "a separate
process."** Deciding plugins run out-of-process (above) gets crash
isolation for free, but a bare `fork`/`exec` — which is all `rpcd`
itself does for script plugins — gets nothing else: a script plugin
runs with the same filesystem view, same network namespace, same
Linux capabilities, and same UID as `rpcd` itself. This design goes a
step further than `rpcd`'s own model, since a toolset plugin here can
originate from the Toolset Store (i.e., from outside the immediate
build, potentially vendor-supplied) rather than only from a trusted
local package feed. Each Toolset Plugin process is launched inside a
sandbox with, at minimum: its own mount and PID namespace (no visibility
into the Dispatcher's or other plugins' filesystem/process tree), a
`seccomp-bpf` syscall allowlist (a DOCSIS toolset needs very different
syscalls than a stateless "common" toolset — profiles should be
per-plugin, not one global list), a `cgroup` enforcing the CPU/memory
limits Execution Framework already declares (turning "resource limits"
from a stated intent into an actually-enforced kernel mechanism), a
non-root UID with Linux capabilities dropped to the minimum the plugin
declares needing, and read-only access to everything except an
explicitly granted scratch path. The plugin's *declared* requirements
(which capabilities, which device nodes, which adapter it needs to
reach) become part of its manifest in the Toolset Store, checked by
RDM Client at install time and enforced by the sandbox at every launch
— not re-negotiated per call.

**ACL is a managed, queryable facility — not just a rule buried inside
Dispatch Core.** The single-checkpoint decision above (ACL enforcement
happens exactly once) says *where* the check happens; it doesn't yet
say how policy gets authored, stored, updated, or audited. This design
adds an explicit **ACL Policy Store**, directly modeled on the two
pieces `rpcd`'s own ACL system got right (already verified in this
project's earlier work): named, declarative permission groups (the
`acl.d/*.json` pattern — `{scope: {toolset: [methods...]}}`, split into
`read`/`write`), and a separate mapping from an authenticated identity
to which groups it holds (the `uci` `login` section's role, now backed
by whatever the SAT/JWT token's claims resolve to instead of a local
password). Concretely, the facility provides: a versioned store for
group definitions, hot-reloadable without restarting the Dispatcher;
support for negative/deny rules evaluated before allow rules (`rpcd`'s
`!`-prefixed pattern, confirmed by live testing earlier in this project
to take priority); a "write implies read" fallback, since `rpcd`
proved that's the natural default policy authors expect; a runtime
query API (`Dispatch Core` calls something equivalent to `rpcd`'s
`session.access` — "would this identity be allowed to call
`toolset.method`?" — both for its own enforcement and for Platform
Adapters needing to reason about scope before forwarding); and an
audit log of every decision, feeding the Audit & Metrics block that
was already part of Dispatch Core. Making this its own named facility,
rather than an implementation detail of Dispatch Core, is what lets
Platform Adapters and RDM Client's install-time capability checks both
query the same policy source rather than each growing their own,
inevitably inconsistent, copy of "who can do what."

**RDM Client is restored, not dropped.** An earlier pass of this
diagram merged Schema & Discovery, Plugin Manager, and Toolset Plugins
into the RDK Dispatcher and lost track of **RDM Client**
(download/verify/install/rollback) in the process. It belongs
alongside Toolset Store, outside the Dispatcher's own request-handling
path: Schema & Discovery advertises what's currently installed, Toolset
Store holds versioned packages, and RDM Client is the actual
install/rollback mechanism that feeds Plugin Manager's load/reload
actions when a new or updated toolset needs to go live.

## 6. Summary of what changed, mapped back to the original diagram

| Original diagram element(s) | This design |
|---|---|
| `rpcd Core (Dispatcher)` box | Renamed/reframed as the RDK Dispatcher's "Dispatch Core" sub-block — same responsibilities (transport, JSON-RPC, session, AuthN/AuthZ, ACL, audit), zero `ubus`/`rpcd` code |
| Plugin Manager + Toolset Plugins + Schema & Discovery (green) | Folded into the same RDK Dispatcher component as Dispatch Core; Plugin Manager owns only coarse toolset routing/health, each Toolset Plugin owns its own method dispatch table (§5, hybrid split) |
| Execution Framework (implicit input) | Fed an already-resolved plugin handle from Plugin Manager; decode → validate → execute, over a defined UDS/JSON boundary, launching each call inside a per-plugin sandbox (§5) |
| Toolset Store + RDM Client | Both restored, sitting outside the Dispatcher's request path: Toolset Store holds versioned packages with a declared capability/permission manifest, RDM Client verifies and installs, feeding Plugin Manager's load/reload actions |
| (not present before) | **ACL Policy Store**: a named facility for group definitions, identity→group mapping, negative/deny rules, write-implies-read fallback, and a runtime query API — queried by Dispatch Core, Platform Adapters, and RDM Client's install-time checks alike (§5) |
| (not present before) | **Sandboxed plugin runtime**: each Toolset Plugin launches inside its own mount/PID namespace, `seccomp-bpf` profile, `cgroup` limits, and dropped capabilities, per its declared manifest (§5) |
| (not present before) | New dashed capability-sync path: Schema & Discovery → Dispatch Core → same Transport Adapter → Cloud Tool & Skill Platform's Device Model Mapping/Tool Catalog, authenticated by device identity rather than the per-session SAT token used for commands |
| Platform Abstraction & Execution Adapters (RBUS / IARM-Thunder / dmcli-CLI / HAL / FS / Shell) | Unchanged in kind — kept as separate, independent adapters; each now required to forward the original caller's identity into RBUS's/Thunder's own ACL check (§5), not run as a blanket-trusted service account |
| Local Clients (UDS) | Explicitly routed through the same Dispatch Core ACL check as cloud-originated commands — does not inherit `rpcd`'s known local-bypass gotcha |

See `rdk_dispatcher_architecture.svg` for the visual layout matching
this table.
