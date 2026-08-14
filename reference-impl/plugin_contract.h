// plugin_contract.h
//
// Illustrative sketch, not the reviewed production header. Every dispatcher
// plugin implements this contract -- same idea as rpcd's `list`/`call` pair
// from the OpenWrt reference project (see ../docs/17_dispatcher_plugin_architecture_cross_check...).

typedef struct {
    const char *plane;          // "config" | "management" | "control" | "triage"
    const char *name;           // e.g. "wifi-radio-reset"
    const char **events;        // sysevent/Netlink names this plugin wants
    int event_count;
    int timeout_ms;             // core enforces this per-call, like rpcd's ubus timeout
    const char *load_type;      // "static" (compiled-in) | "dynamic" (dlopen'd) --
                                 // added for the Phase 1 triage.capabilities response,
                                 // see openspec/changes/add-triage-skillset-mapping-phase1
    const char *version;        // e.g. "1.2.0" -- surfaced in triage.capabilities responses
} plugin_descriptor_t;

// Mirrors rpcd's "list" -- describes what this plugin does, called once at discovery
plugin_descriptor_t *describe(void);

// Mirrors rpcd's "call" -- executes on a matching event
int handle(const char *event_name, const void *event_data, size_t len);
