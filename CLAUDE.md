# Project: RDK-B Dispatcher (Config / Mgmt / Control / Triage)

## Scope
This dispatcher is not just an event router. It owns four planes:
1. Config-apply: pushes PSM/UCI-style config down to HAL/drivers
2. Management: interface/component lifecycle (init, reconfig, teardown)
3. Control: reacts to sysevent/Netlink and drives state machines
4. Triage: on failure/anomaly, collects evidence (last N events, HAL return
   codes, relevant log window) and emits a structured triage record —
   this is what feeds the evidence-collection layer of the triage pipeline

## Stack
- C (C99), CCSP component architecture, HAL abstraction layer
- sysevent for inter-process signaling, Netlink sockets for kernel network events
- Triage records emitted as structured JSON over the existing MQTT/telemetry path
- Dispatcher core is a thin loader/router (rpcd-style); plane logic lives in
  plugins under /usr/libexec/dispatcher/, discovered at init — see plugin_contract.h
- Builds via yocto/bitbake, target is embedded Linux (RDK-B gateway)

## Hard rules
- Never call blocking syscalls on the sysevent notification thread — dispatch
  to the worker queue instead. A blocking call here stalls the whole event bus.
- Every Netlink socket read must check for NLMSG_ERROR before parsing payload.
- All HAL calls must check return codes — never assume RDK_SUCCESS. On failure,
  a triage record must be emitted before returning, not just a log line.
- Free every sysevent_set() allocated buffer in the same function scope. No
  cross-function frees — caused a double-free in the WAN dispatcher last quarter.
- New dispatcher event handlers go in dispatcher_handlers.c; new triage-record
  logic goes in dispatcher_triage.c — never mix the two in one function.
- Triage records must never block the control-plane path — enqueue and return;
  a slow triage pipeline must not stall config/control operations.
- Every plugin must implement both `describe()` and `handle()` from
  plugin_contract.h — a plugin missing either must fail discovery loudly,
  never load partially.
- Core must call every plugin's `handle()` with a timeout. A hung plugin must
  never be able to stall the dispatcher's event loop (mirrors rpcd's per-call
  isolation of plugin invocations).

## Preferences
- Use existing logging macros (CcspTraceInfo/Warning/Error), never printf.
- Match the existing naming convention: `dispatch_<component>_<event>()` for
  control handlers, `triage_capture_<component>()` for evidence capture.

## Common pitfalls (from past sessions)
- Forgetting to deregister a sysevent callback on component teardown leaves a
  dangling handler that fires after shutdown — always pair register/deregister.
- Netlink message alignment: use NLMSG_ALIGN, not manual padding.
- Triage records missing timestamps/correlation IDs are unusable downstream —
  always populate the shared trace_id used elsewhere in telemetry.
