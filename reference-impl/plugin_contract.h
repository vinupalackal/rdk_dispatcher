// plugin_contract.h
//
// Illustrative sketch, not the reviewed production header. Every dispatcher
// plugin implements this contract -- same idea as rpcd's `list`/`call` pair
// from the OpenWrt reference project (see ../docs/17_dispatcher_plugin_architecture_cross_check...).

typedef struct {
    const char *plane;          // "config" | "management" | "control" | "triage"
    const char *name;           // e.g. "wifi-radio-reset" -- also this plugin's
                                 // MCP method name once projected into tools/list
                                 // (openspec/specs/toolset-lifecycle/spec.md,
                                 // "Toolset schema maps to one MCP tool definition
                                 // per toolset": one `oneOf` branch per method)
    const char **events;        // sysevent/Netlink names this plugin wants
    int event_count;
    int timeout_ms;             // core enforces this per-call, like rpcd's ubus timeout
    const char *load_type;      // "static" (compiled-in) | "dynamic" (dlopen'd) --
                                 // surfaced in tools/list's sibling `methods` array,
                                 // see openspec/specs/toolset-lifecycle/spec.md's
                                 // "Descriptive per-method metadata is a sibling
                                 // field..." requirement (not embedded in inputSchema)
    const char *version;        // e.g. "1.2.0" -- same `methods` array as load_type
    const char *params_schema;  // Added for the MCP tools/list projection (see
                                 // mcp_schema_discovery.c). A JSON Schema object
                                 // (serialized string, illustrative -- not a real
                                 // JSON-library type here) describing this method's
                                 // `params` shape, e.g. `{"type":"object","properties":
                                 // {"target_mac":{"type":"string"}}}`. Becomes the
                                 // `params` sub-schema inside this method's `oneOf`
                                 // branch. NULL means "no arguments" (`{"type":
                                 // "object","properties":{}}` is assumed). This is
                                 // deliberately reused from the SAME descriptor the
                                 // event-dispatch path already returns from describe()
                                 // -- one plugin, one descriptor, two consumers
                                 // (dispatcher_dispatch_event()'s internal sysevent
                                 // path, and mcp_schema_discovery.c's external
                                 // tools/list path) -- not a second, parallel
                                 // schema declaration to keep in sync.
} plugin_descriptor_t;

// Mirrors rpcd's "list" -- describes what this plugin does, called once at discovery
plugin_descriptor_t *describe(void);

// Mirrors rpcd's "call" -- executes on a matching event
int handle(const char *event_name, const void *event_data, size_t len);
