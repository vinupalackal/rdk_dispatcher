// dispatcher_command_path.c -- illustrative sketch, not the reviewed production code.
//
// This is the path dispatcher_core.c never sketched: an externally initiated
// command (cloud MCP tools/call, or a local UDS client's plain JSON-RPC 2.0
// call) arriving at Dispatch Core and being resolved + authorized + spawned
// (if needed) + dispatched. dispatcher_dispatch_event() in dispatcher_core.c
// is the OTHER path -- an internally triggered sysevent/Netlink reaction --
// and correctly has no ACL check, because there's no external caller to
// authorize there. This file is where the check actually belongs. See
// docs/20 for the full review this sketch accompanies, and
// toolset_resolution.c for step 1's two-tier lookup design.

#include "plugin_contract.h"

// toolset_locator_t and registry_resolve() are defined in
// toolset_resolution.c (illustrative sketch, same directory) -- declared
// here as externs the way this file already treats every other
// cross-component dependency, rather than including a .c file directly.
typedef struct {
    const char *toolset;
    const char *plane;
    const char *process_uds_path;
    bool process_is_running;
} toolset_locator_t;

extern bool registry_resolve(const char *toolset, const char *method,
                              toolset_locator_t *out);

// Populated by Dispatch Core's SAT/JWT validation step (openspec/changes/
// define-sat-token-format/, still drafted/unarchived -- see OPEN_QUESTIONS.md
// A1). Groups are embedded in the token at issuance, so no session-store
// lookup is needed here -- unlike rpcd's session.access, which looks a
// session ID up against server-side state built at session.login time.
typedef struct {
    const char *identity;       // e.g. "cloud:skillset-mapper" or a device-local client id
    const char **groups;        // ACL Policy Store group names granted to this identity
    int group_count;
} caller_identity_t;

// acl_policy_store_query() mirrors rpcd's session.access(object, function) --
// same deny-first, write-implies-read semantics (openspec/specs/
// acl-policy-store/spec.md) -- but it's called unconditionally on every
// request through this one entry point, not opt-in per caller the way
// rpcd's session.access only gates callers that choose to ask it.
extern bool acl_policy_store_query(const caller_identity_t *caller,
                                    const char *toolset, const char *method);

// Ensures the toolset process named by loc is actually reachable before
// forwarding a call to it -- spawns it on demand if loc->process_is_running
// was false (define-on-demand-toolset-execution, A8), and runs that change's
// spawn-time health check before returning. Updates loc->process_uds_path if
// the spawn assigns a fresh socket. Left unspecified/illustrative here --
// this is Plugin Manager's job, not this routing layer's.
extern bool toolset_ensure_running(toolset_locator_t *loc);

// Forwards (method, params_json) over loc->process_uds_path to the already-
// running toolset process and returns its JSON-RPC result. Inside that
// process, resolution between compiled-in (load_type: static) and
// dlopen()'d (load_type: dynamic) code happens exactly as already shown for
// the Triage Toolset (triage_core_static.c / plugins/triage_wifi.c) --
// unrelated to, and unchanged by, this file's outer resolution step.
extern char *toolset_ipc_forward(const toolset_locator_t *loc,
                                  const char *method,
                                  const char *params_json);

// jsonrpc_error() / jsonrpc_result() build a JSON-RPC 2.0 response object;
// Dispatch Core wraps either in the WRP msg_type:3 envelope before sending,
// same as triage_capabilities.c's result does.
extern char *jsonrpc_error(const char *id, int code, const char *message);
extern char *jsonrpc_result(const char *id, const char *result_json);

// Single entry point for an external command, whether it arrived framed as
// an MCP tools/call (name = "<toolset>", arguments = {"method": "<method>",
// "params": {...}} -- one MCP tool per toolset, not per method, per
// openspec/specs/toolset-lifecycle/spec.md's "Toolset schema maps to one
// MCP tool definition per toolset" requirement) or as a plain JSON-RPC 2.0
// request (openspec/specs/dispatch-core/spec.md's "`tools/call` uses the
// standard command path" requirement -- not a second command path: both
// shapes reach here identically, already unpacked into separate
// toolset/method values by whichever framing produced them). caller has
// already been authenticated (SAT token validated, signature checked)
// before this function is called -- this function's job starts at
// authorization, not authentication.
char *dispatcher_handle_command(const char *request_id,
                                 const caller_identity_t *caller,
                                 const char *toolset,
                                 const char *method,
                                 const char *params_json) {
    // 1. Resolve -- does this toolset.method exist at all, and which
    // toolset process would serve it? Static manifest first (fast, no live
    // process required), live self-registration fallback second -- see
    // toolset_resolution.c / docs/20 §7 for why this is two-tiered rather
    // than one flat lookup.
    toolset_locator_t loc;
    if (!registry_resolve(toolset, method, &loc)) {
        return jsonrpc_error(request_id, -32601, "Method not found");
    }

    // 2. Authorize -- Dispatch Core's single ACL checkpoint (FR-4). This is
    // the check that was missing everywhere in reference-impl/ -- not
    // because it was skipped, but because no command path existed to put it
    // in. It sits here: after resolution (so the check names a real
    // toolset.method, not a guess) and before dispatch (so a denied caller's
    // request never reaches, or spawns, the toolset process at all -- an
    // unauthorized request should not even pay on-demand spawn cost).
    if (!acl_policy_store_query(caller, toolset, method)) {
        // Audit logging of this denial belongs in acl_policy_store_query()
        // itself (see ACL Policy Store's own "Audit logging" requirement) --
        // not duplicated here.
        return jsonrpc_error(request_id, -32000, "Access denied");
    }

    // 3. Ensure reachable -- spawn on demand if the resolved toolset process
    // isn't already running (A8), with its spawn-time health check. This
    // step is skipped for an already-warm toolset still inside its
    // idle-timeout window -- loc.process_is_running tells us which case
    // this is.
    if (!toolset_ensure_running(&loc)) {
        return jsonrpc_error(request_id, -32003, "Toolset unavailable");
    }

    // 4. Dispatch -- forward over IPC to the now-running toolset process.
    // Note this is a different call shape from plugin_contract.h's
    // handle(event_name, event_data, len) -- that contract is for the
    // internal event-dispatch path (dispatcher_dispatch_event(),
    // fire-and-log, no response value). A command has a request/response
    // shape instead (method + params in, a JSON-RPC result out).
    char *result_json = toolset_ipc_forward(&loc, method, params_json);
    return jsonrpc_result(request_id, result_json);
}

// -----------------------------------------------------------------------
// MCP entry points -- added 2026-08-16, implementing openspec/specs/
// dispatch-core/spec.md's "MCP tool method surface" requirement. Nothing in
// this codebase had a `tools/call`/`tools/list` framing layer before this;
// dispatcher_handle_command() above already did all the actual work, it
// just had no MCP-shaped caller yet.
// -----------------------------------------------------------------------

// mcp_name_field / mcp_arguments_field are illustrative stand-ins for
// picking the `name` and `arguments` fields out of an already-JSON-RPC-2.0-
// parsed MCP `tools/call` request object -- request_json is the parsed
// object, not a raw string (unlike dispatcher_handle_command()'s
// params_json, which stays an opaque string all the way through to
// toolset_ipc_forward() since the toolset process re-parses it itself).
extern const char *mcp_json_get_string(const void *json_obj, const char *field);
extern const void *mcp_json_get_object(const void *json_obj, const char *field);
extern char *mcp_json_serialize(const void *json_obj);

// Unpacks an MCP `tools/call` request -- `{"name": "<toolset>", "arguments":
// {"method": "<method>", "params": {...}}}` -- into the same
// (toolset, method, params_json) triple a plain JSON-RPC caller would
// already produce, then calls dispatcher_handle_command() unchanged. Per
// openspec/specs/dispatch-core/spec.md's "`tools/call` uses the standard
// command path" requirement: this function's ENTIRE job is the unpacking
// above the line below -- everything below it is byte-for-byte the same
// ACL/resolution/dispatch path a non-MCP caller already goes through. This
// is deliberately not a rewrite or a parallel implementation of
// dispatcher_handle_command() -- introducing one here is exactly the kind
// of accidental second, inconsistent authorization path that requirement's
// own scenario ("`tools/call` is denied exactly like an equivalent direct
// call") exists to rule out.
char *mcp_handle_tools_call(const char *request_id,
                             const caller_identity_t *caller,
                             const void *tools_call_request_json) {
    const char *toolset = mcp_json_get_string(tools_call_request_json, "name");
    const void *arguments = mcp_json_get_object(tools_call_request_json, "arguments");
    const char *method = mcp_json_get_string(arguments, "method");
    const void *params = mcp_json_get_object(arguments, "params");
    char *params_json = mcp_json_serialize(params);

    return dispatcher_handle_command(request_id, caller, toolset, method, params_json);
}

// build_tools_list_response() -- defined in mcp_schema_discovery.c, this
// project's first implementation of the *discovery* half of the MCP tool
// surface (this file only ever sketched the *command* half, above). Kept as
// a separate file rather than appended here because it answers a
// structurally different question (what toolsets/methods exist, filtered by
// ACL visibility) from what this file answers (route one already-named
// command through resolve/authorize/dispatch) -- see that file's own header
// comment for the two-tier visibility model and the live-vs-manifest schema
// source tradeoff it documents.
extern char *build_tools_list_response(const caller_identity_t *caller);

// Single entry point for a `tools/list` request. Unlike
// dispatcher_handle_command()/mcp_handle_tools_call(), there is no
// (toolset, method) resolution step here -- `tools/list` is inherently
// cross-toolset, so it has no single toolset to resolve against. caller has
// already been authenticated the same as for any other request; per-toolset
// authorization happens INSIDE build_tools_list_response() (once per loaded
// toolset, via the same acl_policy_store_query() this file's
// dispatcher_handle_command() uses), not as a single up-front gate the way
// step 2 of dispatcher_handle_command() is -- `tools/list` deliberately has
// no request-level allow/deny outcome, only per-entry visibility tiers (see
// dispatch-core/spec.md's "tools/list visibility is two-tier" requirement).
char *dispatcher_handle_tools_list(const char *request_id,
                                    const caller_identity_t *caller) {
    char *result_json = build_tools_list_response(caller);
    return jsonrpc_result(request_id, result_json);
}
