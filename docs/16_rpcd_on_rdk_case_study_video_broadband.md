# Case Study: Enabling `rpcd` on the RDK Stack (Video + Broadband Profiles)

This case study looks at what it would actually take to bring `rpcd`
— and the `ubus` ecosystem it depends on — onto RDK (Reference Design
Kit), across both RDK-B (broadband/gateway) and RDK-V (video/set-top
box) profiles. The short version up front: `rpcd` is not a portable,
stack-agnostic RPC daemon that happens to run on OpenWrt — it is
*structurally* an OpenWrt component, built on assumptions (a specific
message bus, a specific config format, a specific plugin ABI) that RDK
does not share on either profile. This isn't a "recompile it for a
different libc" problem, the kind already solved for the container
demo elsewhere in this project — it's an architectural integration
problem, and the two profiles need genuinely different answers.

## 1. Why you'd even want this

Before the technical considerations, it's worth naming the actual
motivation, because it drives which integration strategy makes sense:
typically this comes up when an operator or ODM already has
OpenWrt/LuCI-based tooling — dashboards, automation scripts, a
management UI, monitoring agents — built against `ubus`'s wire
protocol and `rpcd`'s object/method/ACL model, and wants that *same*
tooling to work uniformly across a device fleet that includes both
pure-OpenWrt CPE and RDK-based CPE (gateways and/or STBs). The goal in
that scenario isn't "replace RDK's IPC with ubus" — it's "expose a
`rpcd`-compatible RPC facade on top of RDK's existing internals,"
which is a fundamentally different (and much more achievable) ask than
porting `rpcd` to actually manage RDK's real components.

## 2. What RDK actually uses today, and why it doesn't line up with `ubus`

### 2.1 RDK-B: RBUS (and legacy D-Bus/CCSP)

RDK-B's primary IPC/RPC bus is **RBUS**, which is explicitly described
as replacing D-Bus for performance reasons across all RDK profiles.
It has a three-layer design: an RBUS API layer exposing TR-181-aligned
properties, methods, events, and table operations; an RBUS-Core layer
handling RPC dispatch, event pub/sub, registration, and discovery; and
an `rtMessage` layer providing a lightweight binary protocol over Unix
domain sockets or TCP. A broker daemon, **`rtrouted`**, must be
running for any RBUS component to talk to any other — architecturally
the same *role* `ubusd` plays for `ubus`, but a completely different,
incompatible wire format, client library, and API surface
(`rbus_open()`, `dataElement` registration, etc., not `blob_attr` and
`ubus_add_object()`). RDK-B components historically lived on the CCSP
message bus (D-Bus-based); RBUS is the newer, progressively-adopted
replacement, and configuration/state is exposed as a TR-181 data
model, persisted via **PSM** (Persistent Storage Manager, with
in-memory caching and transactional updates) and legacy **SysCfg** for
system-level settings — not `uci`.

### 2.2 RDK-V: IARM-Bus and Thunder/WPEFramework

RDK-V's older, lower-level IPC layer is **IARM-Bus** — a
platform-agnostic mechanism for sending events or invoking RPCs
between processes, normally one instance per device, used heavily for
internal daemon-to-daemon signaling (power state, HDMI hotplug, and
similar). Layered on top since RDK-V 4 is **Thunder** (formerly
WPEFramework): an open-source, plugin-based device abstraction layer
where functionality is implemented as dynamically loaded/activated
plugins, exposed to clients via two protocols — **COM-RPC** (an
efficient, typically in-process or same-host binary RPC used
plugin-to-plugin) and **JSON-RPC**, recommended over a WebSocket
connection so it also supports server-pushed notifications/events.
Access is gated by a **security token**: a client requests one from a
**Security Agent** plugin over COM-RPC, then presents it as an HTTP
header or at WebSocket-connection time, and Thunder checks a
permissions file to decide whether that token's holder may reach a
given plugin.

Worth being explicit about: **Thunder already is, architecturally, an
answer to almost the exact problem `uhttpd` + `rpcd` solves in this
project's own container demo** — a plugin-loading RPC daemon,
reachable over HTTP/JSON-RPC from a browser, with its own token-based
session/ACL layer gating which plugin/method a given caller may
invoke. That parallel matters for section 4 below.

## 3. The architectural mismatch, side by side

| | OpenWrt (`ubus`/`rpcd`) | RDK-B | RDK-V |
|---|---|---|---|
| Bus/broker daemon | `ubusd`, binary `blob_attr` wire format over a Unix socket | `rtrouted` (RBUS), binary `rtMessage` protocol over Unix socket/TCP | IARM-Bus (low-level events/RPC); Thunder's own COM-RPC/JSON-RPC dispatch (higher level) |
| Config/data model | `uci` flat config files (`/etc/config/*`) | TR-181 data model, backed by PSM + SysCfg | Largely non-uci: RFC (Remote Feature Control), plugin-specific config, no unified TR-181 layer |
| Plugin loading | `dlopen()` a `.so` exporting a C `struct rpc_plugin` symbol (native), or `fork`+`exec` an executable (script) | Each CCSP/RBUS component is typically its own long-running daemon process registering onto the bus — closer in spirit to rpcd's script-plugin model (separate process) but persistent, not exec-per-call | Thunder plugins are `dlopen()`'d `.so` files too, but against a C++ base-class/COM-RPC ABI, not a single C struct symbol |
| Session/auth model | `session.login`/`session.access`, AVL-tree sessions, JSON ACL groups in `/usr/share/rpcd/acl.d/` | RBUS has its own ACL configuration, independent of rpcd's | Thunder's Security Agent + token + permissions file |
| Browser-facing RPC | `uhttpd` + `uhttpd_ubus.so` bridging HTTP JSON-RPC to ubus (this project's Phase 2 work) | No default browser-facing bridge; would need building | Thunder's own JSON-RPC-over-WebSocket already fills this role natively |

Every row is a place where dropping `rpcd` onto RDK "as-is" simply has
nothing to attach to: there is no `ubusd` for it to register objects
against, no `uci` tree for its built-in `uci` object to read, and no
existing ACL/session store its `session` object would be authoritative
over. `rpcd` doesn't fail loudly here — it just becomes an RPC surface
floating disconnected from anything real on the device unless
something is built to connect it.

## 4. Two realistic integration strategies (and one unrealistic one)

**Full replacement — not realistic, not recommended.** Replacing
RBUS or Thunder/IARM with `ubus`/`rpcd` device-wide would mean
rewriting every CCSP component and every Thunder plugin's registration
and RPC-handling code, discarding RDK's own TR-181 data model tooling
(`dmcli`, ACS/TR-069 integration, RFC), and taking on an enormous,
ongoing merge burden against every future RDK release. This isn't a
serious option for an existing RDK deployment and isn't considered
further here.

**Strategy A — an isolated `rpcd`/`ubus` instance, with new plugins
that bridge into RDK's real bus.** Run a genuine `ubusd` + `rpcd` on
the device, exactly as built elsewhere in this project, but write new
**native rpcd plugins** whose `init()` handler doesn't do the work
itself — it opens an `rbus_open()` connection (RDK-B) or a Thunder
COM-RPC/JSON-RPC client connection (RDK-V) internally, and translates
incoming ubus calls into the equivalent RBUS `dataElement`
get/set/method-invoke or Thunder plugin call, and translates the
result back into a `blob_attr` reply. This is the same shape as
`rpcd`'s own built-in `uci` object (a thin bridge from ubus method
calls to `libuci` calls) — just bridging to RBUS or Thunder instead of
`uci`. `uhttpd` + `uhttpd_ubus.so` can then sit in front of this
exactly as already built and tested in this project's container demo,
giving the OpenWrt-style tooling a familiar HTTP/JSON-RPC surface
without touching RDK's actual components at all.

**Strategy B — go the other direction: extend RDK's own bridge instead
of adding `ubus`.** Since Thunder already has a JSON-RPC/WebSocket
bridge with its own token auth, and RBUS already has get/set/subscribe
semantics, an alternative is skipping `ubus`/`rpcd` on-device entirely
and instead teaching whatever *client* tooling currently speaks ubus
JSON-RPC to also speak Thunder's JSON-RPC dialect or an RBUS-facing
HTTP shim. This avoids adding a second bus and a second daemon to the
device at all, at the cost of pushing translation work into the
client/tooling side instead of the device side.

Which of these is right depends entirely on whether the constraint is
"the device must expose this exact ubus/rpcd wire protocol" (favors A)
or "the *tooling* must work against both platforms, however it gets
there" (favors B, and is usually the cheaper, lower-risk path).

## 5. RDK-B (broadband) specific considerations

- **What a bridge plugin actually has to translate.** RBUS's data
  model is a hierarchical tree of named parameters (TR-181-style,
  e.g. `Device.WiFi.Radio.1.Channel`), with typed get/set, method
  invocation, and event subscription — not ubus's flat
  object/method/argument-blob shape. A bridge plugin's `init()` needs
  to decide, per rpcd object it exposes, which RBUS
  parameters/methods/events it maps to, and handle RBUS's typed value
  system (string/int/bool/etc.) converting cleanly to/from
  `blob_attr`.
- **The `uci` builtin is close to meaningless here.** RDK-B's config
  reality is PSM + SysCfg + the TR-181 model, not `uci` files. Either
  disable rpcd's built-in `uci` object entirely on RDK-B builds, or
  replace it with a custom "psm" bridge plugin that speaks to PSM's
  actual API — leaving the real `uci` object registered but backed by
  nothing would be actively misleading to anyone calling it.
- **Some RDK-B reference platforms genuinely do carry OpenWrt-derived
  network components underneath** (a real `ubus`/`netifd` instance
  handling Wi-Fi/network state on certain SoC BSPs, wrapped by a CCSP
  agent above it). Worth checking, per target platform, whether a real
  `ubusd` might *already* be present for network-layer objects — in
  that specific case, `rpcd` could attach to that existing instance
  directly for network state, while still needing the RBUS bridge
  approach above for everything at the TR-181/CCSP level.
- **ACL duplication and WAN exposure risk.** RDK-B devices are
  typically WAN-managed via TR-069/TR-369 by an ACS, with RBUS's own
  ACL gating cross-component access. Adding a second, independent
  ACL system (`rpcd`'s JSON groups) that a bridge plugin enforces on
  top of — or worse, *instead of* — RBUS's own ACL creates two
  separate sources of truth for "who can touch what." Any such bridge
  should be scoped to loopback/LAN-only reachability (never WAN-facing)
  as a hard requirement, and its ACL should be a strict subset of, or
  explicitly re-check, RBUS's own permissions rather than assuming
  its own JSON grants are sufficient on their own.

## 6. RDK-V (video) specific considerations

- **Be honest about the value proposition here being weaker.** Thunder
  already provides a plugin-based, HTTP/JSON-RPC-over-WebSocket,
  token-authenticated RPC surface — the same category of thing
  `uhttpd` + `rpcd` provides for OpenWrt. Introducing `rpcd`/`ubus` on
  the video side mostly duplicates a problem RDK-V has already solved
  natively, rather than filling a gap. The strongest case for doing it
  anyway is tooling uniformity (section 1) — not filling a technical
  hole in RDK-V itself.
- **IARM-Bus bridging is lower-level and more special-purpose.** A
  native rpcd plugin here would link against `libIARM` directly and
  translate specific IARM events (power state changes, HDMI hotplug,
  etc.) into ubus object methods/events — narrower in scope than a
  full RBUS bridge, since IARM isn't a general data-model bus the way
  RBUS is.
- **No ABI-level bridge exists between rpcd's plugin contract and
  Thunder's.** `rpcd` native plugins are C, exporting one symbol
  (`struct rpc_plugin rpc_plugin`); Thunder plugins are C++, built
  against Thunder's own base classes and COM-RPC vtables. There's no
  way to make one directly satisfy the other's loader — any bridge
  here is a standalone process (structurally similar to
  `uhttpd_ubus.so`: a separate binary speaking both protocols) rather
  than a plugin loaded by either side's native mechanism.
- **Two independent auth/session systems, again.** Thunder's
  token-based Security Agent model and rpcd's `session.login`/ACL
  model would coexist, not merge — a bridge either needs to require
  and validate a real Thunder token *before* trusting a ubus session
  at all, or accept that it's introducing a second, separately-secured
  path into the same underlying plugins Thunder already gates.

## 7. Cross-cutting engineering considerations (both profiles)

- **Build system mismatch.** RDK builds on Yocto/OpenEmbedded
  (bitbake recipes), not OpenWrt's buildroot/opkg feeds. Every
  package this project's container demo builds from source
  (`libubox`, `ubus`, `uci`, `rpcd`) would need new `.bb` recipes
  rather than reusing anything OpenWrt-specific — genuinely
  straightforward given all four are portable C already verified to
  build cleanly on plain glibc (proven earlier in this same project),
  but it's real, uncounted work, not a checkbox.
- **Footprint on already-loaded devices.** RDK-B and RDK-V devices
  already run many long-lived daemons — every CCSP/RBUS component as
  its own process on RDK-B, Thunder plus each active plugin as
  separate processes on RDK-V. Adding `ubusd`, `rpcd`, and one or more
  bridge processes is additional RAM/CPU/flash on hardware that is
  frequently tightly budgeted; a real sizing pass (not just "it built
  and ran") belongs in any actual proposal.
- **Security review and confused-deputy risk.** Both RDK-B and RDK-V
  typically go through an operator-mandated security certification
  process for any new IPC surface. The key threat-model question for
  any bridge plugin here: does it run privileged enough to reach
  RBUS/Thunder/IARM on the caller's behalf, and if so, does it
  *faithfully re-derive and re-check* the caller's actual permissions
  on every call (matching rpcd's own real-time `session.access`
  semantics), or does it risk becoming a trusted, always-privileged
  path that silently bypasses RBUS's or Thunder's own ACL because the
  bridge process itself is treated as authorized? This is the same
  class of question this project already had to answer honestly for
  its own `uhttpd_ubus.so` bridge (verified live to re-check
  `session.access` before forwarding, never after) — it doesn't get
  easier crossing into RDK's buses, it gets harder, because there are
  now two independent ACL systems whose semantics must be reconciled
  rather than one.
- **Ongoing maintenance, not a one-time port.** `libubox`/`ubus`/
  `uci`/`rpcd` are maintained upstream independently of RDK. A vendored
  or forked copy needs an explicit plan for tracking upstream security
  fixes and re-validating against each new RDK release's kernel/
  toolchain — this is a standing commitment, not a one-time
  integration task.
- **Verification methodology has to be redone from scratch per bus.**
  Every claim this project verified about `ubus`/`rpcd` (via `nm -D`,
  `strace`, live ACL tests, live HTTP captures) was verified *for
  ubus specifically*. None of it transfers automatically to RBUS's or
  Thunder's actual runtime behavior — a real case study execution
  would need the equivalent live verification against `rtrouted`/RBUS
  and against Thunder's COM-RPC/JSON-RPC dispatch before trusting any
  claim about how a bridge behaves under real load, restart, or
  failure conditions.

## 8. Decision checklist

| Question | If "no" → | If "yes" → |
|---|---|---|
| Is the actual goal "make existing ubus/rpcd-based tooling work against RDK devices too," rather than "replace RDK's IPC"? | Reconsider the whole approach — this is the only motivation that justifies the added complexity | Proceed to Strategy A/B comparison |
| Can the bridge be strictly loopback/LAN-scoped, never WAN-reachable? | Do not proceed without redesigning scope — WAN exposure of a second, less-battle-tested ACL system is a real security regression | Continue |
| Can the bridge re-derive and re-check permissions against RBUS's/Thunder's *own* ACL on every call, rather than trusting its own separate grant? | High confused-deputy risk — treat as blocking until resolved | Continue |
| Does the target platform's footprint budget have headroom for `ubusd` + `rpcd` + a bridge process (RDK-B) or a standalone bridge binary (RDK-V)? | Needs a real sizing study before committing | Continue |
| Is there a concrete plan for tracking upstream `ubus`/`rpcd` security fixes going forward? | Treat as a one-time port and expect drift/security debt | Reasonably positioned to proceed |

## 9. Summary

`rpcd` cannot be "enabled" on RDK in the sense of flipping a build
flag — it has no bus to register objects on, no config tree to read,
and no ACL/session model that's authoritative over anything RDK
actually enforces, on either profile. The only structurally sound path
is treating `rpcd`/`ubus` as an *optional, isolated compatibility
facade* — bridged into RBUS on the broadband side and into Thunder/
IARM on the video side via new plugin code, not a wholesale platform
swap. RDK-B is the stronger case for doing this at all, since RDK-B
doesn't already have a browser-facing JSON-RPC/token-auth story the
way RDK-V's Thunder does; on RDK-V, the realistic question is whether
the tooling-uniformity benefit is worth duplicating something Thunder
already does natively. In both cases, the hard engineering risk isn't
compiling `rpcd` for a new target — this project already proved that's
the easy part — it's correctly reconciling two independent
authorization systems without opening a privilege-escalation path
between them.

---

Sources consulted for RDK-B/RDK-V architecture facts in this document:
- [RDK-B Architecture — RDK Central Wiki](https://wiki.rdkcentral.com/spaces/RDK/pages/175114230/RDK-B+Architecture)
- [CCSP Message Bus — RDK Central Wiki](https://wiki.rdkcentral.com/display/RDK/CCSP+Message+Bus)
- [RBUS — RDK Central Wiki](https://wiki.rdkcentral.com/spaces/RDK/pages/364216711/RBUS)
- [rdkcentral/rbus — GitHub](https://github.com/rdkcentral/rbus)
- [IARM Bus — RDK Central Wiki](https://wiki.rdkcentral.com/display/RDK/IARM+Bus)
- [WPEFramework (Thunder) — RDK Central Wiki](https://wiki.rdkcentral.com/spaces/RDK/pages/90117335/WPEFramework+Thunder)
- [Thunder: Architecture Overview](https://rdkcentral.github.io/Thunder/introduction/architecture/overview/)
- [Thunder: JSON-RPC](https://rdkcentral.github.io/Thunder/introduction/architecture/rpc/jsonrpc/)
- [Thunder Security — RDK Central Wiki](https://wiki.rdkcentral.com/display/RDK/Thunder+Security)
- [Thunder Access Control — RDK Central Wiki](https://wiki.rdkcentral.com/display/RDK/Thunder+Access+Control)
- [CcspDmCli — RDK Central Wiki](https://wiki.rdkcentral.com/spaces/RDK/pages/479888525/CcspDmCli)
