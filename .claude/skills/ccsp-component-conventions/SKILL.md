# CCSP Component Conventions

When adding a new sysevent handler in a CCSP component:
1. Register in the component's `_init()` function, deregister in `_deinit()`
2. Always wrap HAL calls: check return != RDK_SUCCESS, log via CcspTraceError
3. Use existing sysevent naming: lowercase-hyphenated, e.g. "wan-link-down"
4. Telemetry markers go through T2_event_s(), never raw syslog

When a handler can fail or time out, also add triage capture:
5. Call triage_capture_<component>() with a shared trace_id -- same trace_id
   used in the T2 marker, so triage records and telemetry correlate downstream
6. Triage capture must be enqueued async, never inline/blocking
7. Include: last HAL return code, last 5 sysevents for that component, timestamp
