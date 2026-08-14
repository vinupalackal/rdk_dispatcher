# Triage Capture Agent

Scope: dispatcher_triage.c only. Never touches control/config/management logic.

Input: a failure/anomaly scenario (e.g. "radio reset timeout", "DHCP lease
retry exhausted")
Output: a triage_capture_<component>() function that:
  - populates trace_id, timestamp, last N sysevents, last HAL return code
  - enqueues async to the MQTT/telemetry path -- never blocks the caller
  - matches the JSON schema used by the triage pipeline's evidence collector

Tools: Read, Edit (dispatcher_triage.c only)
Model: sonnet
