// triage_capabilities.c -- illustrative sketch, not the reviewed production code.
//
// Implements the `triage.capabilities` JSON-RPC method inside the Triage
// Toolset process. Dispatch Core has already: validated the SAT token,
// checked ACL Policy Store for (identity, "triage", "capabilities"), resolved
// "triage" to this process via Plugin Manager, and forwarded the JSON-RPC
// call over the UDS boundary (per RDK_Dispatcher_Architecture_and_Requirements.md
// §4.6). This file only builds the result payload -- it never touches ACL,
// transport, or WRP framing directly; Dispatch Core wraps this result back
// into a WRP msg_type:3 response.
//
// See openspec/changes/add-triage-skillset-mapping-phase1/design.md for the
// full WRP request/response shapes.

#include "plugin_contract.h"

// registry_all_of_plane() walks Triage Toolset's merged registry (both
// triage_static_registry_init()'s entries and dispatcher_load_plugins()'s
// dynamically loaded ones -- same registry_add() call site for both, see
// dispatcher_core.c and plugins/triage_core_static.c) and returns every
// descriptor tagged plane == "triage".
extern int registry_all_of_plane(const char *plane, plugin_descriptor_t **out, int max);

// Builds the JSON-RPC 2.0 "result" object for a triage.capabilities call.
// Caller (the JSON-RPC dispatch layer) wraps this in {"jsonrpc":"2.0","id":...,"result":...}
// and Dispatch Core wraps *that* in the WRP msg_type:3 envelope.
char *triage_build_capabilities_result(void) {
    plugin_descriptor_t *plugins[32];
    int count = registry_all_of_plane("triage", plugins, 32);

    // json_array_new()/json_object_new() etc. are illustrative stand-ins for
    // whatever JSON library the real implementation picks -- not specified here.
    json_t *capabilities = json_array_new();
    for (int i = 0; i < count; i++) {
        plugin_descriptor_t *p = plugins[i];
        json_t *entry = json_object_new();
        json_object_set(entry, "plugin", json_string_new(p->name));
        json_object_set(entry, "load_type", json_string_new(p->load_type));
        json_object_set(entry, "version", json_string_new(p->version));
        json_object_set(entry, "events", json_string_array_new(p->events, p->event_count));
        json_object_set(entry, "timeout_ms", json_int_new(p->timeout_ms));
        json_array_append(capabilities, entry);
    }

    json_t *result = json_object_new();
    json_object_set(result, "toolset_plane", json_string_new("triage"));
    json_object_set(result, "schema_version", json_string_new("1"));
    json_object_set(result, "capabilities", capabilities);
    json_object_set(result, "generated_at", json_string_new(iso8601_now()));

    return json_serialize(result);  // caller frees
}
