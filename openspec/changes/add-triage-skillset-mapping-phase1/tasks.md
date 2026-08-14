# Tasks

## 1. Triage Toolset process scaffold
- [ ] 1.1 Stand up the Triage Toolset as its own process, registered with Plugin Manager (coarse entry only, per `toolset-lifecycle/spec.md`)
- [ ] 1.2 Implement the static plugin registration table (compiled-in `describe()` calls, no `dlopen()`)
- [ ] 1.3 Port `dispatcher_load_plugins()`'s dynamic `.so` loader into the Triage Toolset process, scanning `/usr/libexec/dispatcher/triage/`
- [ ] 1.4 Extend `plugin_descriptor_t` with `load_type` and `version`; update `reference-impl/plugins/triage_wifi.c` to set them

## 2. `triage.capabilities` method
- [ ] 2.1 Implement `capabilities()` on the Triage Toolset merging static + dynamic plugin descriptors into the response shape in `design.md`
- [ ] 2.2 Wire `triage.capabilities` as a JSON-RPC 2.0 method resolvable by Dispatch Core → Plugin Manager → Triage Toolset

## 3. WRP transport
- [ ] 3.1 Confirm Parodus Agent delivers the `dest: mac:.../rdk-dispatcher/triage` request to Dispatch Core unmodified
- [ ] 3.2 Confirm the JSON-RPC response is wrapped back into a `msg_type: 3` WRP response with matching `transaction_uuid` and `status: 200`
- [ ] 3.3 Confirm a malformed/unauthorized request yields a WRP response with a non-200 `status` and a JSON-RPC `error` object, not a dropped message

## 4. ACL
- [ ] 4.1 Confirm `triage.capabilities` is reachable under the existing write-implies-read default for any identity with triage write access
- [ ] 4.2 Add a read-only "discovery" ACL group scoped to `triage.capabilities` only, per `triage/spec.md`'s ACL scenario

## 5. Verification against spec
- [ ] 5.1 Confirm the static+dynamic merge scenario (`triage/spec.md`)
- [ ] 5.2 Confirm the WRP request/response shape scenario (`triage/spec.md`)
- [ ] 5.3 Confirm this phase does not alter `capability-sync/spec.md`'s existing push behavior (regression check)
