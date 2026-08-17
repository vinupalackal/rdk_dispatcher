// triage_capabilities.c -- illustrative sketch, not the reviewed production code.
//
// Implements the Triage Toolset's own `schema()`/`capabilities()` response --
// NOT a bespoke `triage.capabilities` JSON-RPC method. That method was this
// file's original design (see openspec/changes/archive/add-triage-skillset-mapping-phase1/
// for that history); it was corrected 2026-08-16 to go through the generic
// `tools/list` mechanism instead, once `openspec/specs/toolset-lifecycle/spec.md`'s
// "Toolset schema maps to one MCP tool definition per toolset" requirement
// existed to reuse. See openspec/specs/triage/spec.md, "Triage Toolset is
// discoverable through the generic `tools/list` mechanism, not a dedicated
// method" -- this file is the code-level implementation of that requirement.
//
// Called by mcp_schema_discovery.c's build_tools_list_response(), over the
// same toolset_ipc_forward() IPC path used for ordinary commands, using the
// reserved method name "schema" every toolset process implements generically
// (see mcp_schema_discovery.c's header comment for why this reuses that path
// instead of a second IPC mechanism). This file only builds the result
// payload -- it never touches ACL, transport, or WRP framing directly.

#include "plugin_contract.h"

// registry_all_of_plane() walks Triage Toolset's merged registry (both
// triage_static_registry_init()'s entries and dispatcher_load_plugins()'s
// dynamically loaded ones -- same registry_add() call site for both, see
// dispatcher_core.c and plugins/triage_core_static.c) and returns every
// descriptor tagged plane == "triage". Unchanged by the 2026-08-16
// correction -- the merge logic was always right; only the outer discovery
// *method* around it was wrong (see this file's header note).
extern int registry_all_of_plane(const char *plane, plugin_descriptor_t **out, int max);

// Builds this toolset's MCP tool-list projection: one `name`/`description`
// entry with an `inputSchema.oneOf` branch per internal plugin/method, plus
// the sibling `methods` array carrying `load_type`/`version`/`timeout_ms`
// per openspec/specs/toolset-lifecycle/spec.md's two requirements ("Toolset
// schema maps to one MCP tool definition per toolset" and "Descriptive
// per-method metadata is a sibling field..."). Caller (mcp_schema_discovery.c,
// standing in for Dispatch Core's schema-query IPC hop) wraps this in
// whatever transport envelope the query arrived in -- this function only
// returns the toolset's own self-description, same contract every other
// toolset's `schema()` implementation follows.
char *triage_build_schema_response(void) {
    plugin_descriptor_t *plugins[32];
    int count = registry_all_of_plane("triage", plugins, 32);

    // json_array_new()/json_object_new() etc. are illustrative stand-ins for
    // whatever JSON library the real implementation picks -- not specified here.
    json_t *one_of = json_array_new();
    json_t *methods = json_array_new();

    for (int i = 0; i < count; i++) {
        plugin_descriptor_t *p = plugins[i];

        // inputSchema's oneOf branch: argument shape ONLY (method + params).
        // Never descriptive metadata here -- that's the whole point of the
        // sibling `methods` array below (toolset-lifecycle/spec.md's
        // "Descriptive per-method metadata is a sibling field, not embedded
        // in a tools/list entry's inputSchema" requirement).
        json_t *branch = json_object_new();
        json_t *props = json_object_new();
        json_t *method_const = json_object_new();
        json_object_set(method_const, "const", json_string_new(p->name));
        json_object_set(props, "method", method_const);
        json_object_set(props, "params",
                         p->params_schema
                             ? json_parse(p->params_schema)
                             : json_parse("{\"type\":\"object\",\"properties\":{}}"));
        json_object_set(branch, "properties", props);
        json_object_set(branch, "required", json_string_array_new1("method"));
        json_array_append(one_of, branch);

        // Sibling methods[] entry: descriptive metadata only, correlated to
        // the oneOf branch above by `name` == that branch's `method` const.
        json_t *entry = json_object_new();
        json_object_set(entry, "name", json_string_new(p->name));
        json_object_set(entry, "load_type", json_string_new(p->load_type));
        json_object_set(entry, "version", json_string_new(p->version));
        json_object_set(entry, "timeout_ms", json_int_new(p->timeout_ms));
        json_array_append(methods, entry);
    }

    json_t *input_schema = json_object_new();
    json_object_set(input_schema, "oneOf", one_of);

    json_t *result = json_object_new();
    json_object_set(result, "name", json_string_new("triage"));
    json_object_set(result, "description",
                     json_string_new("Triage-plane capability discovery and "
                                      "evidence-capture entry points"));
    json_object_set(result, "inputSchema", input_schema);
    // methods[] is optional per spec ("A toolset with nothing descriptive to
    // add omits methods entirely") -- Triage always has load_type/version to
    // report, so it's always included here, but a simpler toolset legitimately
    // may not populate it at all.
    json_object_set(result, "methods", methods);

    return json_serialize(result);  // caller frees
}
