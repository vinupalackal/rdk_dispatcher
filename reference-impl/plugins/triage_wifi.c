// plugins/triage_wifi.c -- illustrative sketch, not the reviewed production plugin.
//
// A triage-plane plugin. A control-plane plugin (plugins/control_wan.c) would
// have the same shape, just .plane = "control" and handle() driving a state
// transition instead of capturing evidence.

#include "plugin_contract.h"

static const char *events[] = { "wifi-radio-reset", "wifi-radio-reset-timeout" };

plugin_descriptor_t *describe(void) {
    static plugin_descriptor_t desc = {
        .plane = "triage",
        .name  = "wifi-triage",
        .events = events,
        .event_count = 2,
        .timeout_ms = 200,   // triage capture must be fast -- enqueue, don't block
        .load_type = "dynamic",  // loaded from /usr/libexec/dispatcher/triage/*.so
        .version = "1.2.0"
    };
    return &desc;
}

int handle(const char *event_name, const void *event_data, size_t len) {
    triage_record_t rec = triage_build_record(event_name, event_data, len);
    return triage_enqueue_async(&rec);  // never blocks; matches CLAUDE.md hard rule
}
