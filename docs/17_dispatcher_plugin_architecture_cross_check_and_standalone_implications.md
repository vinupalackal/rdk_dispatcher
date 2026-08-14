# Dispatcher Mechanism & Plugin Architecture — Cross-Check, and the Implications of Building It Standalone

This follows directly from `16_rpcd_on_rdk_case_study_video_broadband.md`,
which concluded that bridging `rpcd` into RBUS/Thunder is the only
structurally sound integration path. This document zooms into the
piece being considered as a separate, standalone effort — the
**dispatcher** (how a call finds its way to the right handler) and the
**plugin architecture** (how a handler gets loaded into the runtime in
the first place) — cross-checked in detail against `ubus`/`rpcd`,
RBUS, and Thunder, then works through what it actually means to build
that piece on its own, decoupled from bridging into RDK's real
components.

## 1. Dispatcher mechanism, cross-checked

### 1.1 `ubus`/`rpcd`: a two-level split — the broker only routes, the owner dispatches

This is easy to get wrong, so it's worth being precise: **`ubusd`
itself does not dispatch by method name at all.** It maintains three
AVL trees (`obj_types`, `objects`, `path`) purely to route a call to
the right *owning process and object* — a call names an object (by
path, e.g. `example-script`, or by numeric object id once resolved),
and `ubusd` looks up which connected process registered that object
and forwards the raw message to it, verbatim. The actual **method
dispatch** — matching the method name string inside that message
against a specific C function — happens *inside the owning process*
(`rpcd`, in this project's case), against a method table that process
built itself at `ubus_add_object()` time. `ubusd` is a cheap,
O(log n), purely-routing broker; every daemon on the bus does its own
method-level dispatch.

### 1.2 RBUS/`rtrouted`: the same broker-routes/provider-dispatches split, plus a disk cache

RBUS's shape is structurally similar: `rtrouted` is the broker daemon
that must be running for any two RBUS components to talk, and routing
is driven by RBUS's own **discovery APIs** operating over its
hierarchical, TR-181-style dataElement names — conceptually the same
role as `ubusd`'s path AVL tree, just keyed on dot-separated data
model paths instead of ubus object names. Method invocation and
event subscription are then forwarded to whichever **provider**
registered that dataElement — the provider does its own internal
dispatch, the same broker/owner split as `ubus`. One genuine
difference: `rbus_open()` calls `deserializeFromDisk` to load a
per-component cache file at startup, meaning RBUS trades some
persistent, on-disk discovery state for faster reconnect/discovery —
`ubus` has no equivalent; its entire object tree is transient,
rebuilt from scratch in `ubusd`'s memory every time, with nothing
persisted to disk.

### 1.3 Thunder: three dispatch layers, not one

Thunder is the most layered of the three, and cross-checking it
against `rpcd`'s single flat method table is where the biggest design
difference shows up:

1. **Plugin selection**, at the HTTP/WebSocket (JSON-RPC) or COM-RPC
   transport layer — a request names a specific plugin (by "callsign")
   before anything else happens, roughly analogous to `ubus`'s
   object-name routing step.
2. **Method dispatch within a plugin**, for JSON-RPC — each plugin's
   C++ class explicitly calls something equivalent to `Register("methodName",
   callback)` to build its own local name→handler table. This is the
   same *conceptual shape* as `rpcd`'s per-object `UBUS_METHOD` table
   (a plugin owns and populates its own dispatch table; the framework
   just looks names up in it) — just declared imperatively in C++
   rather than as a static C array.
3. **Interface navigation, for COM-RPC** — every COM-RPC interface
   inherits from `IUnknown`, whose `QueryInterface()` method lets a
   caller ask at runtime "does this plugin support interface X?" and
   get back a typed pointer if so, or `nullptr` if not. This is a
   dispatch step with **no equivalent at all** in `ubus`/`rpcd` — ubus
   objects don't have "interfaces" a caller queries for; a method
   either exists on the object or it doesn't, discoverable only via
   `ubus -v list`.

### 1.4 Side-by-side

| | `ubus`/`rpcd` | RBUS | Thunder |
|---|---|---|---|
| Broker-level routing key | Object path/id (AVL trees in `ubusd`) | Hierarchical dataElement name (TR-181-style) | Plugin "callsign" |
| Who does method-name dispatch | The owning process's own method table | The provider's own handler | The plugin's own `Register()`-built table (JSON-RPC); or `QueryInterface` (COM-RPC) |
| Persistent discovery cache | None — fully transient, in-memory only | Yes — `rbus_open()` loads a disk cache file | Not applicable in the same sense (plugin config is static, loaded at startup) |
| Interface-level indirection | None | None | Yes, for COM-RPC (`QueryInterface`) |

The load-bearing takeaway: **all three systems keep the broker cheap
and dumb, and push the expensive, string-comparison-heavy dispatch
work into the process that owns the object/plugin.** Any standalone
dispatcher built for RDK should preserve that split deliberately — a
broker that tries to centralize full method-level dispatch for every
object on the device becomes both a performance bottleneck and a
single point of failure for all RPC traffic, which none of the three
real systems being cross-checked here actually do.

## 2. Plugin architecture, cross-checked

### 2.1 `rpcd`: a fixed, binary choice — in-process `dlopen`, or a fresh process per call

Already covered in depth elsewhere in this project's docs, restated
briefly for the cross-check: **native plugins** are `dlopen()`'d once
at startup, exporting one C symbol (`struct rpc_plugin rpc_plugin`),
and run forever after inside `rpcd`'s own address space, sharing its
fate (a crash in the plugin is a crash in `rpcd`). **Script plugins**
are `fork()`+`exec()`'d fresh, *every single call*, as a genuinely
separate process, communicating over pipes with a JSON-over-stdio
protocol. There is no third option and no per-plugin choice — the
mechanism is determined entirely by which directory the plugin file
lives in (`/usr/lib/rpcd/*.so` vs `/usr/libexec/rpcd/*`).

### 2.2 RBUS: no `dlopen` step at all — every provider is just a process

RBUS doesn't have a "plugin" concept that a broker loads. A
**provider** is simply any process, anywhere, that calls
`rbus_open()` and registers dataElements — there's no daemon that
`dlopen()`s a provider's code into itself. Structurally, this is
closest to `rpcd`'s **script plugin** model (a genuinely separate
process, not sharing address space with anything) but with an
important difference: RBUS providers are expected to be **long-running
daemons that register once and stay up**, not short-lived
processes spawned fresh per call the way `rpcd`'s script plugins are.
There's no `fork`+`exec`-per-call cost in the RBUS model at all — the
cost `rpcd` pays repeatedly for script plugins, RBUS avoids entirely
by design, at the cost of every provider needing to be its own
supervised, always-running service.

### 2.3 Thunder: `dlopen` against a C++ ABI, with a genuine in-process/out-of-process choice

Thunder plugins are `dlopen()`'d `.so` files, like `rpcd`'s native
plugins — but against a **C++ base-class/vtable ABI**
(`IPlugin`/`IUnknown`-derived interfaces), not a single flat C struct
symbol, and loaded by the Thunder host process rather than by `rpcd`
directly (obviously — this is RDK-V's own framework). The genuinely
notable difference worth taking seriously if designing something new:
**Thunder plugins can be configured to run either in-process (inside
Thunder's own address space, like `rpcd` native plugins) or
out-of-process (in their own supervised process, like `rpcd` script
plugins) — as a per-plugin choice**, not a fixed architectural split
baked into two separate directories the way `rpcd` does it. `rpcd`'s
model forces the tradeoff (shared-fate speed vs. process-isolation
safety) at plugin-*type* granularity; Thunder allows it at
plugin-*instance* granularity.

### 2.4 Side-by-side

| | `rpcd` native | `rpcd` script | RBUS provider | Thunder plugin |
|---|---|---|---|---|
| Loading mechanism | `dlopen()` once at startup | `fork()`+`exec()` every call | No loading — an independent, always-running process | `dlopen()` once at startup |
| ABI contract | One C struct symbol (`rpc_plugin`) | `argv`/stdio JSON convention, no ABI | None — just RBUS's own client API calls | C++ base class + vtables |
| Process model | Shared with the host, fixed | Fresh process, fixed, every call | Always its own separate process, fixed | **Either**, chosen per plugin |
| Crash blast radius | Takes the host down | Contained | Contained (own process) | Depends on the chosen mode |

## 3. Implications of developing the dispatcher + plugin architecture standalone

This is the part worth sitting with before starting: building *just*
this piece — a dispatcher and plugin-loading runtime, modeled on
`rpcd`'s design but not literally `rpcd`, developed and proven on its
own — is a meaningfully different undertaking than the bridge strategy
in the earlier case study, not a smaller version of it.

**It starts with zero functionality, not partial functionality.** The
bridge strategy (case study §4, Strategy A) gets you, immediately,
access to whatever RBUS/Thunder already exposes, mediated through new
plugin code. A standalone dispatcher+plugin runtime has nothing
registered on it at all until *you* write a plugin for it — including,
eventually, the same RBUS/Thunder bridge plugins the other strategy
needed anyway. Building the dispatcher standalone doesn't remove that
bridging work; it just changes which runtime ends up hosting it later.
Be explicit with yourself about this: it's not "bridge work vs.
dispatcher work," it's "dispatcher work, and then still the bridge
work on top of it."

**This is closer in scope to building a new RPC framework than to an
integration project.** `ubus`+`rpcd` together represent a real, if
small, piece of infrastructure that the OpenWrt project maintains on
an ongoing basis. Recreating the dispatcher-and-plugin-loading core of
that — even a simplified version — for RDK is comparable in kind of
effort, not a weekend adapter. Plan and communicate expectations
accordingly.

**You get real design freedom, and should use it deliberately rather
than defaulting to copying `rpcd`.** Three choices worth making on
purpose rather than by default:

- *Transport*: nothing requires reusing `ubus`'s exact `blob_attr`
  wire format if you're not trying to be wire-compatible with real
  `rpcd`/`ubus` clients. RBUS's own `rtMessage` layer — a lightweight
  binary protocol already running over Unix domain sockets/TCP on
  every RDK-B device — is a plausible transport to build directly on
  top of, instead of importing a whole separate `ubus` stack purely to
  get its wire format.
- *Broker vs. owner dispatch split*: preserve it. Section 1.4's
  takeaway — keep the broker's routing cheap and centralized, push
  method-level dispatch into whichever process/plugin owns the
  object — is validated by all three systems studied here
  independently arriving at it. Deviating from this (e.g., a broker
  that tries to dispatch by method name itself) risks turning the
  broker into a bottleneck none of the systems it's modeled on
  actually have.
- *Plugin process model*: `rpcd`'s fixed native-or-script dichotomy is
  not obviously the right choice to copy. Thunder's per-plugin
  in-process/out-of-process option (§2.3) is a more flexible, already
  field-proven design for exactly this kind of dispatcher — worth
  adopting instead of `rpcd`'s binary split unless there's a specific
  reason not to.

**Verification burden is entirely yours, with nothing upstream to lean
on.** Every claim this project made about `ubus`/`rpcd`'s real
behavior was checked directly against upstream source and live
processes precisely because none of it could be safely assumed.
Building a new dispatcher means there is no upstream project's track
record to inherit at all — every dispatch-correctness, plugin-loading,
and ACL-enforcement-placement decision needs the same first-principles
verification this whole project applied to `rpcd`, from scratch,
before it can be trusted.

**A concrete recommendation for sequencing, if this goes ahead.**
Prototype the dispatcher and plugin loader in isolation first, on a
throwaway harness, the same way this entire project first verified
`dlopen`/`dlsym`/`fork`/`exec` behavior live before wiring anything
into a real container — confirm the broker/owner dispatch split, the
chosen plugin process model, and basic ACL enforcement placement work
correctly with a couple of trivial fake plugins, with no RDK
dependency at all yet. Only after that foundation is proven should a
single, real RBUS or Thunder bridge plugin be wired in end-to-end, to
validate the design against one genuine piece of RDK functionality
before assuming it generalizes to the rest.

---

Sources consulted for RBUS/Thunder dispatch and plugin-loading facts
in this document:
- [RBUS — RDK Central Wiki](https://wiki.rdkcentral.com/spaces/RDK/pages/364216711/RBUS)
- [rdkcentral/rbus — GitHub](https://github.com/rdkcentral/rbus)
- [Interface-Driven Development — Thunder](https://rdkcentral.github.io/Thunder/plugin/interfaces/interfaces/)
- [Thunder: Architecture Overview](https://rdkcentral.github.io/Thunder/introduction/architecture/overview/)
- [Thunder: JSON-RPC](https://rdkcentral.github.io/Thunder/introduction/architecture/rpc/jsonrpc/)
- [ubusd_obj.c source, buildroot mirror](https://sources.buildroot.net/ubus/git/ubusd_obj.c)
