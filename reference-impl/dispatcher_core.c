// dispatcher_core.c -- illustrative sketch, not the reviewed production loader.
//
// Core does discovery + routing only. Plane logic (config/management/control/
// triage) lives entirely in plugins under /usr/libexec/dispatcher/*.so,
// loaded here and never hardcoded -- see plugin_contract.h.

// At init: scan /usr/libexec/dispatcher/*.so
void dispatcher_load_plugins(const char *plugin_dir) {
    DIR *d = opendir(plugin_dir);
    struct dirent *entry;
    while ((entry = readdir(d)) != NULL) {
        if (!ends_with(entry->d_name, ".so")) continue;

        void *handle = dlopen(path_join(plugin_dir, entry->d_name), RTLD_NOW);
        if (!handle) {
            CcspTraceError(("Plugin %s failed to load: %s\n", entry->d_name, dlerror()));
            continue;  // never partial-load -- skip loudly, don't crash core
        }

        plugin_descriptor_t *(*describe_fn)(void) = dlsym(handle, "describe");
        int (*handle_fn)(const char*, const void*, size_t) = dlsym(handle, "handle");
        if (!describe_fn || !handle_fn) {
            CcspTraceError(("Plugin %s missing contract functions\n", entry->d_name));
            dlclose(handle);
            continue;
        }

        plugin_descriptor_t *desc = describe_fn();
        registry_add(desc, handle_fn);   // registers against sysevent/Netlink names
    }
    closedir(d);
}

// On event: dispatch to every plugin registered for it, each under its own timeout
void dispatcher_dispatch_event(const char *name, const void *data, size_t len) {
    for_each_registered_plugin(name, plugin) {
        run_with_timeout(plugin->handle_fn, name, data, len, plugin->desc->timeout_ms);
        // a hung plugin times out and is logged -- never stalls the event loop
    }
}
