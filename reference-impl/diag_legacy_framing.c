// diag_legacy_framing.c -- illustrative sketch, not the reviewed production code.
//
// Implements docs/24_diag_server_merge_plan.md §8 step 3 (legacy framing
// adapter) and §10.3 (Phase 2 ACL design's transport section), for the one
// legacy entry this project has: diag-server's WRP type-3 requests, whose
// inner payload is msgpack -- {"tool": "<name>", "command": ""} in,
// {"tool", "exit_code", "stdout"} out -- not JSON, not JSON-RPC 2.0.
//
// The whole point of this file: diag-server-nn.c (external/diag-server/) is
// NOT touched, here or anywhere else. Its catalog, is_command_safe(),
// validate_static_commands(), timeout/kill logic, everything -- stays
// exactly as already staged. This file's decode/encode pair at the edge,
// plus one toolset-specific forward function, is the entire cost of
// integrating it: dispatcher_command_path.c's resolve/authorize/dispatch
// logic (already sketched, ACL-checked per FR-4) is reused completely
// unmodified -- this file never re-implements or bypasses any of it.
//
// Two hops, mirrored on purpose:
//   Hop 1 (external, cloud/Parodus -> Dispatch Core): decoded below by
//     diag_legacy_handle_request() / diag_decode_request().
//   Hop 2 (Dispatch Core -> diag-server's own process, §10.3's local
//     transport): re-encoded below by diag_toolset_ipc_forward() /
//     diag_build_response_msgpack(), speaking diag-server's OWN wire shape
//     unchanged, not a converted JSON-RPC dialect.
// Both hops' msgpack field layouts are mirrored field-for-field from
// diag-server-nn.c's decode_request_payload() / build_response_payload()
// (see external/diag-server/diag-server-nn.c ~L605-665) -- not copied code
// (this runs in Dispatch Core, a different binary), but deliberately
// byte-shape-identical, since the same bytes an external caller sends
// today must still parse the same way however they're read now.

#include "plugin_contract.h"
#include <msgpack.h>

// ===========================================================================
// Addressing: how a request is known to be this legacy shape, not guessed
// ===========================================================================

// Dispatch Core now owns the WRP destination diag-server used to register
// directly with Parodus (SERVICE_NAME "diag-server" in diag-server-nn.c) --
// see §10.3's "re-point diag-server's registration" step. A request
// arriving addressed here is *known* to be msgpack {tool,command} by
// destination, not sniffed from the bytes -- MCP tools/call and JSON-RPC
// 2.0 requests are addressed elsewhere (per A12) and never reach this file.
#define DIAG_LEGACY_WRP_DESTINATION "diag-server"

// diag-server's own local endpoint, once it stops registering with the
// public tcp://127.0.0.1:6666/6669 pair and is repointed to a
// Dispatch-Core-only address per §10.3. nanomsg is the natural choice
// since diag-server-nn.c already links and speaks it -- no new transport
// library needed on diag-server's side, only a different endpoint string.
#define DIAG_LOCAL_ENDPOINT "ipc:///run/dispatcher/diagnostics.sock"

// ===========================================================================
// Reused, unmodified -- declared here as an extern the same way
// dispatcher_command_path.c already treats every cross-component
// dependency, rather than including that file directly.
// ===========================================================================

extern char *dispatcher_handle_command(const char *request_id,
                                        const caller_identity_t *caller,
                                        const char *toolset,
                                        const char *method,
                                        const char *params_json);

// json_*()/msgpack_*() calls throughout are the same kind of illustrative
// stand-ins triage_capabilities.c already uses for its JSON library choice
// -- not a real library pin here either. msgpack_*() calls specifically
// mirror msgpack-c's real API (the library diag-server-nn.c already links),
// since that API shape is already real and grounded, not invented.

// stand-in for whatever local request/reply transport Hop 2 actually uses;
// nanomsg to match diag-server's own existing dependency is the expected
// answer, left unspecified here the same way toolset_ipc_forward() is left
// unspecified in dispatcher_command_path.c.
extern void *nn_request_reply(const char *endpoint, const void *req,
                               size_t req_len, size_t *resp_len);

extern char *jsonrpc_result(const char *id, const char *result_json);
extern char *jsonrpc_error(const char *id, int code, const char *message);

static void *diag_build_response_msgpack(const char *tool, int exit_code,
                                          const char *output, size_t *out_len);
static char *diag_decode_response_to_json(const void *resp, size_t resp_len);

// ===========================================================================
// Hop 1: WRP type-3 inbound -> internal (toolset, method, params) tuple
// ===========================================================================

// Field-by-field identical to diag-server-nn.c's decode_request_payload():
// same two fields ("tool", "command"), same MSGPACK_OBJECT_MAP walk, same
// strndup-into-caller-owned-strings convention.
static void diag_decode_request(const uint8_t *payload, size_t len,
                                 char **tool_out, char **cmd_out)
{
    msgpack_unpacked result;
    msgpack_unpacked_init(&result);
    if (msgpack_unpack_next(&result, (const char *)payload, len, NULL)
            == MSGPACK_UNPACK_SUCCESS
        && result.data.type == MSGPACK_OBJECT_MAP) {

        msgpack_object_map *map = &result.data.via.map;
        for (uint32_t i = 0; i < map->size; i++) {
            msgpack_object_kv *kv = &map->ptr[i];
            if (kv->key.type != MSGPACK_OBJECT_STR) continue;
            const char *k  = kv->key.via.str.ptr;
            uint32_t    kl = kv->key.via.str.size;
            if (kl == 4 && memcmp(k, "tool", 4) == 0
                    && kv->val.type == MSGPACK_OBJECT_STR)
                *tool_out = strndup(kv->val.via.str.ptr, kv->val.via.str.size);
            else if (kl == 7 && memcmp(k, "command", 7) == 0
                    && kv->val.type == MSGPACK_OBJECT_STR)
                *cmd_out = strndup(kv->val.via.str.ptr, kv->val.via.str.size);
        }
    }
    msgpack_unpacked_destroy(&result);
}

// Internal params representation is JSON, same as every other toolset's
// (per A12) -- diagnostics doesn't get a special internal shape, only a
// special edge decoder/encoder. This keeps dispatcher_handle_command()'s
// resolution/ACL logic below completely unaware anything about this
// request is msgpack-framed.
static char *diag_build_params_json(const char *command)
{
    json_t *params = json_object_new();
    json_object_set(params, "command", json_string_new(command ? command : ""));
    return json_serialize(params); // caller frees
}

// Single entry point for a WRP type-3 request addressed to
// DIAG_LEGACY_WRP_DESTINATION. caller has already been authenticated (SAT
// token validated) before this is called -- same precondition
// dispatcher_command_path.c's dispatcher_handle_command() already states;
// this function's job is framing translation only, nothing about
// authentication or authorization.
void *diag_legacy_handle_request(const char *request_id,
                                  const caller_identity_t *caller,
                                  const uint8_t *wrp_payload, size_t wrp_len,
                                  size_t *out_len)
{
    char *tool = NULL, *cmd = NULL;
    diag_decode_request(wrp_payload, wrp_len, &tool, &cmd);

    char *params_json = diag_build_params_json(cmd);

    // toolset is always "diagnostics" -- this decoder is wired to exactly
    // one legacy WRP destination (DIAG_LEGACY_WRP_DESTINATION above), so
    // there's nothing to branch on. method is whatever tool the caller
    // named; an unknown tool is already handled -- resolution inside
    // dispatcher_handle_command() returns "Method not found" the same way
    // it would for any other toolset, no special-casing needed here.
    char *jsonrpc_response = dispatcher_handle_command(
        request_id, caller, "diagnostics", tool ? tool : "", params_json);

    // Unwrap the JSON-RPC response back into (tool, exit_code, stdout) and
    // re-encode as msgpack -- byte-shape identical to what diag-server used
    // to send directly (its own build_response_payload()). A caller who
    // used to talk straight to diag-server cannot tell the difference.
    json_t *parsed = json_parse(jsonrpc_response);
    json_t *result_obj = json_object_get(parsed, "result"); // NULL on the error branch
    const char *resp_tool = result_obj ? json_get_string(result_obj, "tool") : tool;
    int  exit_code  = result_obj ? json_get_int(result_obj, "exit_code") : -1;
    const char *out = result_obj ? json_get_string(result_obj, "stdout")
                                  : json_get_string(parsed, "error.message"); // jsonrpc_error() shape

    void *msgpack_response = diag_build_response_msgpack(resp_tool, exit_code, out, out_len);

    free(tool); free(cmd); free(params_json); free(jsonrpc_response);
    json_free(parsed);
    return msgpack_response;
}

// Field-by-field identical to diag-server-nn.c's build_response_payload():
// same 3-entry map, same field order/names, "stdout" packed as msgpack
// bin (not str), matching diag-server's own choice there exactly.
static void *diag_build_response_msgpack(const char *tool, int exit_code,
                                          const char *output, size_t *out_len)
{
    msgpack_sbuffer sbuf;
    msgpack_packer  pk;
    msgpack_sbuffer_init(&sbuf);
    msgpack_packer_init(&pk, &sbuf, msgpack_sbuffer_write);

    const char *tv = tool   ? tool   : "unknown";
    const char *ov = output ? output : "";
    size_t tl = strlen(tv), ol = strlen(ov);

    msgpack_pack_map(&pk, 3);
    msgpack_pack_str(&pk, 4);  msgpack_pack_str_body(&pk, "tool", 4);
    msgpack_pack_str(&pk, tl); msgpack_pack_str_body(&pk, tv, tl);
    msgpack_pack_str(&pk, 9);  msgpack_pack_str_body(&pk, "exit_code", 9);
    msgpack_pack_int(&pk, exit_code);
    msgpack_pack_str(&pk, 6);  msgpack_pack_str_body(&pk, "stdout", 6);
    msgpack_pack_bin(&pk, ol);
    if (ol > 0) msgpack_pack_bin_body(&pk, ov, ol);

    void *data = malloc(sbuf.size);
    if (data) { memcpy(data, sbuf.data, sbuf.size); *out_len = sbuf.size; }
    msgpack_sbuffer_destroy(&sbuf);
    return data;
}

// ===========================================================================
// Hop 2: Dispatch Core -> diag-server's own process (§10.3's local
// transport). This is the ONE toolset-specific realization of
// toolset_ipc_forward() -- declared generic in dispatcher_command_path.c,
// left "unspecified/illustrative" there. Every other toolset speaks
// JSON-RPC over its UDS per that file's own comment on toolset_ipc_forward();
// "diagnostics" is the sole, deliberately narrow exception, wired in
// wherever the real toolset_ipc_forward() dispatches on loc->toolset --
// e.g. `if (!strcmp(loc->toolset, "diagnostics")) return
// diag_toolset_ipc_forward(method, params_json);` -- not shown here since
// that dispatch line lives in the generic function's own file, not this
// one; the point is dispatcher_command_path.c itself needs zero edits.
// ===========================================================================

char *diag_toolset_ipc_forward(const char *method, const char *params_json)
{
    // params_json is {"command": "..."} (diag_build_params_json() above) --
    // pull "command" back out and re-pack into diag-server's own
    // {tool, command} request shape untouched. This round-trip
    // (msgpack -> JSON -> msgpack across the two hops) is deliberate, not
    // an oversight: it keeps both hops going through the SAME internal
    // (toolset, method, params) representation every other toolset uses,
    // so dispatcher_handle_command()'s resolution/ACL logic never has to
    // know diagnostics is special -- the cost is a few extra allocations
    // per request, not a design compromise.
    json_t *parsed = json_parse(params_json);
    const char *command = json_get_string(parsed, "command");
    const char *cv = command ? command : "";

    msgpack_sbuffer sbuf;
    msgpack_packer  pk;
    msgpack_sbuffer_init(&sbuf);
    msgpack_packer_init(&pk, &sbuf, msgpack_sbuffer_write);

    size_t ml = strlen(method), cl = strlen(cv);
    msgpack_pack_map(&pk, 2);
    msgpack_pack_str(&pk, 4);  msgpack_pack_str_body(&pk, "tool", 4);
    msgpack_pack_str(&pk, ml); msgpack_pack_str_body(&pk, method, ml);
    msgpack_pack_str(&pk, 7);  msgpack_pack_str_body(&pk, "command", 7);
    msgpack_pack_str(&pk, cl); msgpack_pack_str_body(&pk, cv, cl);

    size_t resp_len = 0;
    void *resp = nn_request_reply(DIAG_LOCAL_ENDPOINT, sbuf.data, sbuf.size, &resp_len);
    msgpack_sbuffer_destroy(&sbuf);
    json_free(parsed);

    if (!resp) {
        // diag-server unreachable (process down, local-transport timeout)
        // -- §10.3's distinct failure path. Deliberately NOT the "access
        // denied" shape: that's an ACL-layer failure that never reaches
        // this function at all (dispatcher_handle_command() returns
        // before dispatch on a denial, per its own step 2/step 4 split).
        return jsonrpc_error("", -32003, "diagnostics backend unavailable");
    }

    // Decode diag-server's real {tool, exit_code, stdout} response
    // (its own build_response_payload()'s shape) into the JSON-RPC result
    // object dispatcher_handle_command()'s generic jsonrpc_result()
    // wrapping expects -- diag_legacy_handle_request() above unwraps this
    // right back out again on the way to the external response, but that
    // round-trip is what lets this function return through the exact same
    // "JSON-RPC result" contract every other toolset's forward already
    // returns through.
    char *result_json = diag_decode_response_to_json(resp, resp_len);
    free(resp);
    return jsonrpc_result("", result_json);
}

// Mirrors diag_decode_request() above, but for the RESPONSE map shape
// (build_response_payload()'s 3 fields, not the request's 2) -- decodes
// diag-server's own {tool, exit_code, stdout} into a small JSON object.
static char *diag_decode_response_to_json(const void *resp, size_t resp_len)
{
    msgpack_unpacked result;
    msgpack_unpacked_init(&result);
    json_t *out = json_object_new();
    if (msgpack_unpack_next(&result, (const char *)resp, resp_len, NULL)
            == MSGPACK_UNPACK_SUCCESS
        && result.data.type == MSGPACK_OBJECT_MAP) {

        msgpack_object_map *map = &result.data.via.map;
        for (uint32_t i = 0; i < map->size; i++) {
            msgpack_object_kv *kv = &map->ptr[i];
            if (kv->key.type != MSGPACK_OBJECT_STR) continue;
            const char *k  = kv->key.via.str.ptr;
            uint32_t    kl = kv->key.via.str.size;
            if (kl == 4 && memcmp(k, "tool", 4) == 0 && kv->val.type == MSGPACK_OBJECT_STR)
                json_object_set(out, "tool", json_string_new_n(kv->val.via.str.ptr, kv->val.via.str.size));
            else if (kl == 9 && memcmp(k, "exit_code", 9) == 0 && kv->val.type == MSGPACK_OBJECT_POSITIVE_INTEGER)
                json_object_set(out, "exit_code", json_int_new((int)kv->val.via.i64));
            else if (kl == 6 && memcmp(k, "stdout", 6) == 0 && kv->val.type == MSGPACK_OBJECT_BIN)
                json_object_set(out, "stdout", json_string_new_n(kv->val.via.bin.ptr, kv->val.via.bin.size));
        }
    }
    msgpack_unpacked_destroy(&result);
    char *json_str = json_serialize(out); // caller frees
    json_free(out);
    return json_str;
}
