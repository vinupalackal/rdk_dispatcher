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
// define-toolset-as-mcp-tool-model's revised tools/list granularity) or as
// a plain JSON-RPC 2.0 request (same change -- "tools/call is not a second
// command path": both shapes reach here identically, already unpacked into
// separate toolset/method values by whichever framing produced them).
// caller has already been authenticated (SAT token validated, signature
// checked) before this function is called -- this function's job starts at
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
