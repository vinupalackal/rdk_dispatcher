// plugins/triage_core_static.c -- illustrative sketch, not the reviewed production plugin.
//
// A STATIC (compiled-in) triage plugin, contrasted with triage_wifi.c's
// dynamic (.so, dlopen()'d) counterpart. Same contract, no dlopen() involved --
// this file is linked directly into the Triage Toolset binary, and
// triage_static_registry_init() (below) calls describe() directly instead
// of going through dispatcher_load_plugins()'s scan-and-dlopen loop.
//
// See openspec/specs/triage/spec.md, "Static and dynamic plugin capability
// merge" (formerly documented under the now-archived
// openspec/changes/archive/add-triage-skillset-mapping-phase1/design.md,
// applied into that base spec 2026-08-16).

#include "plugin_contract.h"

static const char *events[] = { "dispatcher-self-check" };

// params_schema: this method takes no arguments -- a self-check sweep needs
// nothing from the caller beyond the `tools/call` itself. NULL means "no
// arguments" per plugin_contract.h's own comment on the field
// (mcp_schema_discovery.c/triage_build_schema_response() fill in the
// `{"type":"object","properties":{}}` default), so this is left unset
// rather than spelled out redundantly.
plugin_descriptor_t *describe(void) {
    static plugin_descriptor_t desc = {
        .plane = "triage",
        .name  = "core-triage",
        .events = events,
        .event_count = 1,
        .timeout_ms = 100,
        .load_type = "static",   // compiled into the Triage Toolset binary
        .version = "1.0.0",
        .params_schema = NULL
    };
    return &desc;
}

int handle(const char *event_name, const void *event_data, size_t len) {
    triage_record_t rec = triage_build_record(event_name, event_data, len);
    return triage_enqueue_async(&rec);
}

// Called once at Triage Toolset process init, before the dynamic .so scan.
// Registers every compiled-in triage plugin the same way dispatcher_load_plugins()
// registers a dynamic one -- registry_add() doesn't care which path produced
// the descriptor.
void triage_static_registry_init(void) {
    registry_add(describe(), handle);
}
