# Tasks

## 1. Spec updates
- [ ] 1.1 `dispatch-core/spec.md`: add payload-level encryption requirement (encrypt before WRP envelope/TLS transport; decrypt on receipt, before ACL evaluation)
- [ ] 1.2 `dispatch-core/spec.md`: add message-kind routing requirement (definition → Plugin Manager load path; command → local JSON-RPC IPC → Execution Framework)
- [ ] 1.3 `toolset-lifecycle/spec.md`: cross-reference that a `definition`-kind message is how `toolset.push` (and RDM Client-fed installs) reach Plugin Manager
- [ ] 1.4 Amend `define-synchronous-toolset-push/design.md`'s "payload encryption not assumed necessary" note with a pointer to this change's superseding decision

## 2. Open items not decided here
- [ ] 2.1 Choose the payload encryption scheme (symmetric with a device-provisioned key, asymmetric envelope encryption, etc.) and key management/rotation approach
- [ ] 2.2 Confirm whether `definition`-kind messages delivered via RDM Client's own (non-WRP-command) path need the same payload-encryption treatment, or whether RDM's existing download-integrity mechanism already covers it

## 3. Verification against spec
- [ ] 3.1 Confirm scenario: a command-kind message with a valid decrypted payload is still denied by ACL if the caller lacks permission — decryption success does not imply authorization
- [ ] 3.2 Confirm scenario: a definition-kind message routes to Plugin Manager's load path and never reaches Execution Framework
- [ ] 3.3 Confirm scenario: a `load_type: static` (compiled-in) plugin can be the target of a `command`-kind message — message kind and load_type vary independently
