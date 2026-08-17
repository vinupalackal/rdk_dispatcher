// toolset_resolution.c -- illustrative sketch, not the reviewed production code.
//
// Answers dispatcher_command_path.c's resolution step: "does this
// toolset.method exist, and which toolset PROCESS do I need to reach
// (spawning it on demand if it isn't already running) to execute it."
//
// This is Plugin Manager's OUTER resolution layer -- it never dlopen()s
// anything itself. Once a target toolset process is confirmed reachable,
// the static-vs-dynamic merge happens INSIDE that toolset's own process,
// exactly as already shown for the Triage Toolset in
// triage_core_static.c / plugins/triage_wifi.c / triage_capabilities.c's
// registry_all_of_plane() -- unchanged by this file, generalized to every
// plane's toolset process per define-plane-vs-toolset-model.
//
// Note: dispatcher_core.c's dispatcher_load_plugins() dlopen()s plugin .so
// files directly into ITS OWN process -- that's the pre-A2/A3 model, known
// non-compliant (CLAUDE_CODE_WORKFLOW.md, "reopened, not resolved"; needs a
// rework pass tracked in define-plane-vs-toolset-model/tasks.md §3). This
// file's resolution logic does not depend on, or extend, that stale model.
//
// See docs/20 §7 for the design writeup this sketch accompanies.

#include "plugin_contract.h"

// What a resolved lookup returns -- enough to reach the right toolset
// PROCESS, not enough to call anything directly. Unlike the inner
// event-dispatch registry (registry_add()/registry_resolve() inside a
// single toolset process, dispatcher_core.c), this outer layer always
// crosses a process boundary, so there's no function pointer here.
typedef struct {
    const char *toolset;          // e.g. "wifi-triage-ext"
    const char *plane;            // "config" | "management" | "control" | "triage"
    const char *process_uds_path; // where to reach this toolset's process
                                   // once running, e.g. "/run/dispatcher/wifi-triage-ext.sock"
    bool process_is_running;      // Plugin Manager's current knowledge -- may
                                   // be stale by the time dispatch actually happens
} toolset_locator_t;

// Tier 1 -- install-time-written manifest, one JSON file per toolset
// (mirrors rpcd's own acl.d/*.json multi-file convention, e.g.
// /usr/share/dispatcher/toolsets.d/wifi-triage-ext.json). Written by RDM
// Client on install (FR-11/FR-12) or by the toolset.push handler
// (define-synchronous-toolset-push) on a synchronous push -- always derived
// from the toolset's own manifest block (toolset.push's params.manifest),
// never hand-authored. Answers "does this toolset.method exist at all"
// without requiring the toolset's process to be running -- essential under
// on-demand execution (A8), where most toolsets are idle most of the time,
// including right after boot before anything has been spawned even once.
extern bool manifest_lookup(const char *toolset, const char *method,
                             toolset_locator_t *out);

// Tier 2 -- ask a toolset process that's already live (spawned recently,
// still within its idle-timeout window) to confirm a method the manifest
// lookup missed. Covers the narrow race where a toolset was just pushed or
// reloaded and its manifest file write hasn't landed yet, or the manifest
// and the process's own self-description have drifted apart. This is the
// SAME describe()-style self-report a toolset process already gives Plugin
// Manager at its own startup handshake (mirroring capability-sync's "push
// on session establishment" pattern, generalized from device-to-cloud down
// to toolset-to-Plugin-Manager) -- not a new mechanism, just consulted here
// as a fallback instead of only at handshake time.
extern bool live_registration_lookup(const char *toolset, const char *method,
                                      toolset_locator_t *out);

// On a Tier-2 hit that Tier 1 missed, repair the manifest so the next
// lookup doesn't need Tier 2 again. Best-effort and asynchronous -- must
// not block the in-flight request this resolution call is servicing.
extern void manifest_repair_async(const toolset_locator_t *loc);

bool registry_resolve(const char *toolset, const char *method,
                       toolset_locator_t *out) {
    if (manifest_lookup(toolset, method, out)) {
        return true;                    // common case: fast, no live process needed
    }
    if (live_registration_lookup(toolset, method, out)) {
        manifest_repair_async(out);     // don't miss Tier 1 next time
        return true;
    }
    return false;                       // truly doesn't exist -> -32601 upstream
}

// -----------------------------------------------------------------------
// Coarse enumeration -- added 2026-08-16. registry_resolve() above answers
// "does this ONE toolset.method exist" (a targeted Tier-1/Tier-2 lookup);
// the three functions below answer "what toolsets exist AT ALL," a
// different question this file had no answer to before mcp_schema_discovery.c
// and capability_sync_emission.c needed one (`tools/list`'s full catalog,
// and capability-sync's session-establishment publish, both iterate every
// loaded toolset rather than resolving one named toolset.method). All three
// read the same Tier-1 manifest store manifest_lookup() already reads --
// no second toolset inventory is introduced here.
// -----------------------------------------------------------------------

// Lists every toolset the manifest store currently knows about, regardless
// of whether its process is running right now -- the same per-toolset JSON
// files manifest_lookup() reads one at a time, just walked in full here.
// Backs both mcp_schema_discovery.c's build_tools_list_response() (which
// then decides per-entry visibility) and toolset_list_snapshot() below
// (which only needs the names). toolset-lifecycle/spec.md's "Coarse-only
// plugin registry" requirement is what licenses this to be a flat toolset
// list -- not a device-wide table of every method, which is why this
// function's out param is toolset_locator_t (one per toolset), not
// anything method-grained.
extern int manifest_list_all_toolsets(toolset_locator_t *out, int max);

// Returns the cached schema snapshot for a toolset that is NOT currently
// running -- written alongside that toolset's manifest entry at
// install/push/last-reload time, same provenance as manifest_lookup()'s
// existence/routing data (RDM Client on install, or toolset.push's
// params.manifest on a synchronous push), just extended here to also carry
// the {name, description, inputSchema, methods} schema shape
// triage_build_schema_response() and every other toolset's schema()
// implementation produce -- so an idle toolset can still answer `tools/list`
// without being spawned. See mcp_schema_discovery.c's header comment for the
// live-vs-cached tradeoff this snapshot exists to make: a stale answer here
// is a deliberate, flagged tradeoff, not an oversight.
extern char *manifest_cached_schema(const char *toolset);

// Fills out_names with every currently loaded toolset's name (no locator
// detail, no schema -- just the flat name list capability_sync_emission.c's
// capability_sync_publish_on_session_establish() needs to push each one).
// Implemented in terms of manifest_list_all_toolsets() above rather than a
// separate walk, so `tools/list` and the session-establishment publish are
// guaranteed to enumerate the identical toolset set -- one enumeration path,
// two consumers, matching this project's general preference (see
// capability_sync_emission.c's plugin_manager_toolset_changed()) for a
// single shared source over independently maintained copies.
int toolset_list_snapshot(const char **out_names, int max) {
    toolset_locator_t locs[64];
    int count = manifest_list_all_toolsets(locs, max < 64 ? max : 64);
    int n = count < max ? count : max;
    for (int i = 0; i < n; i++) {
        out_names[i] = locs[i].toolset;
    }
    return n;
}
