// capability_sync_emission.c -- illustrative sketch, not the reviewed
// production code.
//
// Implements two confirmed, applied requirements together, because they're
// two deliveries fed by the same trigger and this project deliberately
// closed the risk of them drifting apart by construction, not by convention:
//
//   1. openspec/specs/capability-sync/spec.md, "Distinct from MCP's live
//      tool-change notification, fanned out from one shared trigger point"
//      -- capability-sync's device-identity-authenticated push to the
//      cloud's Device Model Mapping/Tool Catalog, and MCP's
//      `notifications/tools/list_changed` to any live-connected session,
//      permanently separate deliveries, but both triggered from exactly one
//      internal call site.
//   2. openspec/specs/capability-sync/spec.md, "Device publishes its
//      toolset list on session establishment" -- push is the PRIMARY
//      discovery path, `tools/list` (mcp_schema_discovery.c) only a
//      secondary, on-demand confirmation of the same data.
//
// Before this file, nothing in reference-impl/ called either delivery
// mechanism at all -- capability-sync/spec.md's requirements existed only
// as spec text, with no code-level trigger point sketched anywhere.

#include "plugin_contract.h"

// Illustrative stand-ins for the two independent downstream deliveries.
// Neither is defined in this file -- this file's whole job is calling both,
// from one place, never letting a caller reach one without the other.
//
// capability_sync_push(): device-identity-authenticated, over the same
// Transport Adapter/XMiDT connection as command traffic
// (capability-sync/spec.md's "Shared transport, separate authentication"
// requirement), carrying the full merged capability descriptor set
// (capabilities()/schema() output) for `toolset`. Has its own retry/backoff
// against the Device Model Mapping/Tool Catalog backend -- durable,
// eventually-consistent delivery, may be slow or briefly failing.
extern void capability_sync_push(const char *toolset);

// mcp_notify_tools_list_changed(): best-effort, fire-and-forget
// `notifications/tools/list_changed` to any MCP session currently
// connected. No retry, no backoff -- a client that misses it can always
// re-poll `tools/list` (mcp_schema_discovery.c). Must never be delayed by
// capability_sync_push()'s retry state.
extern void mcp_notify_tools_list_changed(const char *toolset);

// The single internal emission point. Plugin Manager's load/unload/reload
// path calls this and only this -- no other code path may trigger either
// delivery directly (capability-sync/spec.md: "Both SHALL originate from a
// single internal emission point in Plugin Manager's load/unload/reload
// path... not two independently registered listeners"). This is what
// structurally prevents a future change from wiring up a new
// reload-triggering code path (a re-sandboxing event, say) that updates one
// delivery and simply forgets the other exists -- there is exactly one call
// site to find and extend.
//
// Deliberately NOT synchronous-then-synchronous: the two calls below share
// no return value, no shared retry loop, no ordering dependency on each
// other's success. A slow or currently-failing capability_sync_push() must
// not delay mcp_notify_tools_list_changed() reaching an already-connected
// client (capability-sync/spec.md's "A slow catalog backend does not delay
// the live notification" scenario) -- implemented here as two independent
// calls, not one waiting on the other's result. A real implementation would
// likely enqueue each onto its own independent worker/queue rather than
// call synchronously in-line as shown; this sketch keeps the call shape
// visible rather than hiding it behind a queue abstraction not otherwise
// defined in this codebase.
void plugin_manager_toolset_changed(const char *toolset) {
    capability_sync_push(toolset);
    mcp_notify_tools_list_changed(toolset);
}

// Called once, at session establishment (a fresh XMiDT/WRP session with the
// cloud coming up) -- not per-toolset, not per-reload. Publishes the
// device's CURRENT full toolset list before any cloud-initiated `tools/list`
// request is expected (capability-sync/spec.md: "the device publishes its
// current toolset list before any cloud-initiated tools/list request is
// expected or required"). This is what makes push primary and `tools/list`
// secondary in practice, not just in the spec's wording -- a cloud client
// that never sends `tools/list` at all still learns the device's toolsets,
// because this function ran before it could have asked.
//
// toolset_list_snapshot() is Plugin Manager's own coarse registry read (see
// toolset_resolution.c's manifest_list_all_toolsets(), the same enumeration
// mcp_schema_discovery.c's build_tools_list_response() uses for `tools/list`
// itself) -- not a separate toolset-tracking mechanism.
extern int toolset_list_snapshot(const char **out_names, int max);

void capability_sync_publish_on_session_establish(void) {
    const char *names[64];
    int count = toolset_list_snapshot(names, 64);
    for (int i = 0; i < count; i++) {
        // Reuses the same push primitive plugin_manager_toolset_changed()
        // calls per-reload -- session establishment is "every currently
        // loaded toolset changed, as far as this fresh session is
        // concerned," not a conceptually different kind of push.
        capability_sync_push(names[i]);
    }
    // Deliberately no mcp_notify_tools_list_changed() call here: there is no
    // MCP session connected yet at the moment a transport session is only
    // just establishing -- that notification is for an ALREADY-connected
    // client learning about a CHANGE, not for initial discovery, which this
    // function (and `tools/list`, on-demand) already cover.
}
