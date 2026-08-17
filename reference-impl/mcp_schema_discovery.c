// mcp_schema_discovery.c -- illustrative sketch, not the reviewed production code.
//
// Builds Dispatch Core's `tools/list` response. Nothing in this codebase
// implemented this before 2026-08-16 -- dispatcher_command_path.c sketched
// `tools/call`-equivalent command dispatch (dispatcher_handle_command()),
// but the *discovery* half of the MCP tool surface
// (openspec/specs/dispatch-core/spec.md's "MCP tool method surface"
// requirement) had no code path at all. This file is that path.
//
// Two confirmed, applied requirements this file implements together:
//   1. openspec/specs/toolset-lifecycle/spec.md, "Toolset schema maps to
//      one MCP tool definition per toolset" -- one `tools/list` entry per
//      toolset, `inputSchema` a `oneOf` discriminated union.
//   2. openspec/specs/dispatch-core/spec.md, "`tools/list` visibility is
//      two-tier, scoped by the caller's existing ACL grant" -- every loaded
//      toolset named regardless of grant; per-toolset detail included only
//      if the caller has at least read access to it.
//
// Where a toolset's live schema comes from -- a real design tension, not
// silently resolved:
//
// toolset-lifecycle/spec.md's "Self-described schema" requirement says a
// schema query must reflect the *running* plugin directly, immediately, no
// stale copy. But `define-on-demand-toolset-execution` (still an unapplied
// draft as of this file) exists specifically because most toolsets should
// NOT be resident just in case something asks about them -- spawning every
// installed toolset merely to answer one `tools/list` call would blow the
// 300KB concurrently-active footprint budget (OPEN_QUESTIONS.md B3) for no
// real benefit to the caller. This file resolves that tension pragmatically,
// not authoritatively:
//   - A toolset already running (warm, inside its idle-timeout window) is
//     asked live, over the same IPC path an ordinary command uses
//     (toolset_ipc_forward()), with the reserved method name "schema" --
//     every toolset process is expected to answer that name generically,
//     the way triage_capabilities.c's triage_build_schema_response() does
//     for the Triage Toolset. No new IPC mechanism, no spawn cost paid.
//   - A toolset that is NOT currently running answers from
//     manifest_cached_schema() -- a schema snapshot written alongside the
//     toolset's manifest at install/push/last-reload time (see
//     toolset_resolution.c's manifest_lookup() for the existing precedent:
//     this project already accepts a manifest-cached answer as authoritative
//     enough for *existence*/routing; this extends that same acceptance to
//     *schema detail* for a currently-idle toolset specifically).
// This means a `tools/list` response for an idle toolset can be briefly
// stale relative to a schema change that hasn't triggered a reload yet --
// flagged here as a real, known divergence from "Self-described schema"'s
// literal wording, not something a future change should assume was
// overlooked. Worth a real decision (probably its own OpenSpec change) once
// `define-on-demand-toolset-execution` itself is applied and this tension
// becomes concrete rather than illustrative.

#include "plugin_contract.h"

// caller_identity_t / acl_policy_store_query() -- same shapes
// dispatcher_command_path.c already declares; redeclared here rather than
// shared via a common header, matching this directory's existing convention
// (see dispatcher_command_path.c's own header note on toolset_resolution.c's
// toolset_locator_t) of each file declaring the externs it needs, not
// assuming a shared internal header exists.
typedef struct {
    const char *identity;
    const char **groups;
    int group_count;
} caller_identity_t;

extern bool acl_policy_store_query(const caller_identity_t *caller,
                                    const char *toolset, const char *method);

typedef struct {
    const char *toolset;
    const char *plane;
    const char *process_uds_path;
    bool process_is_running;
} toolset_locator_t;

// Coarse enumeration -- Plugin Manager's own registry, per
// toolset-lifecycle/spec.md's "Coarse-only plugin registry" requirement
// ("which toolsets exist... not a device-wide table of every method").
// Defined in toolset_resolution.c, alongside manifest_lookup()/
// live_registration_lookup() -- same underlying manifest store, listing
// instead of looking up one entry.
extern int manifest_list_all_toolsets(toolset_locator_t *out, int max);

// A per-toolset schema snapshot, cached at install/push/last-reload time --
// see this file's header note on why an idle toolset answers from here
// instead of being spawned. Illustrative return shape: same serialized
// {name, description, inputSchema, methods} JSON this file itself builds
// for a live toolset, just read back instead of freshly queried.
extern char *manifest_cached_schema(const char *toolset);

// Same IPC path dispatcher_command_path.c's dispatcher_handle_command() uses
// to forward a real command -- reused here with the reserved method name
// "schema", which every toolset process answers generically (see this
// file's header note). params_json is "{}" -- a schema query takes no
// arguments.
extern char *toolset_ipc_forward(const toolset_locator_t *loc,
                                  const char *method,
                                  const char *params_json);

// Does this caller have at least read access to ANY method of this toolset?
// The two-tier visibility requirement is toolset-grained ("include that
// toolset's inputSchema... only if the caller has at least read access to
// that toolset"), but acl_policy_store_query() is (toolset, method)-grained
// -- there's no toolset-only query in this project's ACL contract. This
// function bridges the two: a toolset is visible in full if the caller is
// granted on at least one of its methods (parsed out of whichever schema
// source answered), not required to be granted on every method. This is a
// real, illustrative design choice this file makes, not something the
// confirmed spec text settled explicitly -- flagged here rather than
// treated as an obvious reading. See also the "Write access implies full
// listing detail" / "A read-only discovery grant sees full detail" scenarios
// in dispatch-core/spec.md, both of which describe a caller ending up with
// the WHOLE toolset's detail from a partial grant, consistent with this
// choice.
static bool caller_has_any_read_access(const caller_identity_t *caller,
                                        const char *toolset,
                                        const char **method_names,
                                        int method_count) {
    for (int i = 0; i < method_count; i++) {
        if (acl_policy_store_query(caller, toolset, method_names[i])) {
            return true;
        }
    }
    return false;
}

// json_t / json_*() calls below are illustrative stand-ins for whatever JSON
// library the real implementation picks, same convention as
// triage_capabilities.c.

// Builds the full `tools/list` JSON-RPC 2.0 "result" object: `{"tools": [...]}`.
// Caller (the JSON-RPC dispatch layer, alongside dispatcher_handle_command()'s
// existing jsonrpc_result()/jsonrpc_error() helpers in dispatcher_command_path.c)
// wraps this in `{"jsonrpc":"2.0","id":...,"result":...}` and Dispatch Core
// wraps *that* in the WRP msg_type:3 envelope -- identical wrapping
// convention to every other response this codebase builds.
char *build_tools_list_response(const caller_identity_t *caller) {
    toolset_locator_t toolsets[64];
    int toolset_count = manifest_list_all_toolsets(toolsets, 64);

    json_t *tools = json_array_new();

    for (int i = 0; i < toolset_count; i++) {
        toolset_locator_t *loc = &toolsets[i];

        // Schema source: live IPC if already running (no spawn cost paid --
        // see this file's header note), manifest-cached snapshot otherwise.
        char *schema_json = loc->process_is_running
                                 ? toolset_ipc_forward(loc, "schema", "{}")
                                 : manifest_cached_schema(loc->toolset);

        json_t *parsed = json_parse(schema_json);
        json_t *one_of = json_object_get(json_object_get(parsed, "inputSchema"), "oneOf");

        // Pull method names out of the parsed oneOf branches for the ACL
        // check -- the schema response is the only place this file learns
        // what a toolset's methods are actually called.
        int method_count = json_array_size(one_of);
        const char *method_names[64];
        for (int m = 0; m < method_count && m < 64; m++) {
            json_t *branch = json_array_get(one_of, m);
            json_t *method_const = json_object_get(
                json_object_get(json_object_get(branch, "properties"), "method"),
                "const");
            method_names[m] = json_string_value(method_const);
        }

        json_t *entry = json_object_new();
        json_object_set(entry, "name", json_string_new(loc->toolset));

        if (caller_has_any_read_access(caller, loc->toolset, method_names,
                                        method_count < 64 ? method_count : 64)) {
            // Full detail: description, inputSchema, methods[] carried
            // through verbatim from whichever schema source answered.
            json_object_set(entry, "description",
                             json_object_get(parsed, "description"));
            json_object_set(entry, "inputSchema",
                             json_object_get(parsed, "inputSchema"));
            json_t *methods = json_object_get(parsed, "methods");
            if (methods) {
                json_object_set(entry, "methods", methods);
            }
        } else {
            // Two-tier visibility: name only, existence not hidden, detail
            // withheld. dispatch-core/spec.md's "Unauthorized caller sees
            // the toolset exists but not its schema" scenario, verbatim.
            json_object_set(entry, "access_restricted", json_bool_new(true));
        }

        json_array_append(tools, entry);
    }

    json_t *result = json_object_new();
    json_object_set(result, "tools", tools);
    return json_serialize(result);  // caller frees
}
