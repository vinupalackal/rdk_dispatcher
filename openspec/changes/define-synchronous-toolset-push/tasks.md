# Tasks

## 1. Spec updates
- [ ] 1.1 `dispatch-core/spec.md`: add `toolset.push` JSON-RPC method requirement (synchronous accept/reject, inline signature verification)
- [ ] 1.2 `dispatch-core/spec.md`: add requirement that `toolset.push` is authorized via its own dedicated ACL scope, never satisfied by a `tools/call`-granting scope
- [ ] 1.3 `toolset-lifecycle/spec.md`: add requirement for the health-check-gated rollback behavior (prior version stays live until new version passes health check)
- [ ] 1.4 `toolset-lifecycle/spec.md`: add requirement stating `toolset.push` and RDM Client are both valid install paths, scoped by artifact class, with the same verification policy applied to both
- [ ] 1.5 `acl-policy-store/spec.md`: add requirement for a dedicated `toolset-publish`-class permission, structurally separate from toolset read/write scopes

## 2. Boundary and policy decisions
- [x] 2.1 ~~Define the size/artifact-type threshold separating `toolset.push` from RDM Client's path~~ — resolved 2026-08-13: type-based, not size-based. RDM Client handles any `dlopen()`-able binary regardless of size; `toolset.push` never carries one
- [x] 2.2 ~~Decide whether payload-level encryption beyond transport TLS is required~~ — resolved by `require-payload-encryption-and-message-routing`: yes in Phase 2, phased — Phase 1 is plain JSON-RPC with mandatory verification (see that change's phasing decision)
- [ ] 2.3 Define the nonce/replay-window policy for `toolset.push` requests
- [ ] 2.4 **New:** now that `toolset.push` cannot carry a `dlopen()`-able binary, define what its `params.artifact` field actually carries, if anything — candidates: toolset definition/manifest metadata referencing an RDM-delivered binary, a script-based (non-`dlopen()`) toolset implementation, or no artifact at all for pure command/registration messages. Not assumed — needs explicit confirmation before implementation
- [ ] 2.5 **New, 2026-08-13:** define the actual numeric WRP payload
      size ceiling for `toolset.push` itself — the type-based boundary
      (task 2.1) settles *what kind* of artifact can never go through
      `toolset.push`, but not the largest size a permitted (non-
      `dlopen()`-able) artifact may be. Confirmed as **Phase 2** work,
      not decided here — see `OPEN_QUESTIONS.md` B12. Distinct from,
      but related to, the ~300KB total footprint budget (B3): B3 bounds
      resident process memory, this bounds a single push message's
      payload size

## 3. Verification against spec
- [ ] 3.1 Confirm scenario: a `toolset.push` with an invalid signature is rejected synchronously, with no partial load
- [ ] 3.2 Confirm scenario: an identity with `tools/call` access to a toolset, but not the `toolset-publish` scope, is denied on `toolset.push`
- [ ] 3.3 Confirm scenario: a pushed toolset that fails its health check is automatically reverted, and the prior version continues serving `tools/call` requests uninterrupted throughout
- [ ] 3.4 Confirm scenario: a replayed (previously-used) nonce is rejected even with a otherwise-valid signature
