# Phase 1 Test Plan

Companion to `docs/21_phase1_requirements_document.md` and
`docs/22_phase1_architecture_hld_lld.md`. Every test suite below
traces to a requirement (P1-1 through P1-14) or a named architectural
risk from those two documents — nothing here tests behavior Phase 1
doesn't actually claim to have.

## 1. Scope and test levels

| Level | What it covers | Who runs it |
|---|---|---|
| Unit | Individual functions in isolation (`registry_resolve()`, `acl_policy_store_query()`, JSON-RPC parsing, WRP envelope handling) | Developer, on every commit |
| Functional/Integration | End-to-end request shapes (A/B/C from `docs/22` §1.2) through the real component chain | CI, on every PR |
| Negative | Malformed input, denied auth, failed verification, boundary violations | CI, on every PR |
| Performance | Latency budgets for in-process calls | Nightly/scheduled |
| Stress | Concurrent load, sustained push/swap cycles, crash blast-radius | Pre-release gate |

**Explicitly out of scope for this Phase 1 test plan:** sandbox escape
testing, seccomp/namespace verification, payload decryption
correctness, RBUS/Thunder adapter identity-forwarding tests, and the
independent NFR-3/NFR-4 security review — all of these test something
Phase 1 doesn't implement yet (see `docs/21` §6). Testing them now
would produce false confidence about controls that don't exist.

## 2. Unit test plan

### 2.1 Coverage targets

| Component | Target line coverage | Target branch coverage | Rationale |
|---|---|---|---|
| `dispatcher_command_path.c` (resolution/ACL/dispatch flow) | 90% | 85% | Security-relevant path (P1-8); every branch (allow/deny, resolve hit/miss) must be exercised |
| `toolset_resolution.c` (two-tier lookup) | 90% | 90% | Both tiers, and the miss-both-tiers case, must each have a dedicated test |
| ACL Policy Store query logic | 95% | 90% | Deny-first and write-implies-read are exactly the kind of subtle-ordering logic that needs near-complete branch coverage (see the rpcd precedent in `docs/20` §2 for how easy this is to get subtly wrong) |
| JSON-RPC/WRP envelope parse | 85% | 80% | High-volume, externally-facing input; malformed-input branches matter as much as happy-path ones |
| `toolset.push` verification + rollback | 90% | 90% | Every rollback branch (health pass, health fail) must be directly tested, not just the happy path |
| Housekeeping (`dispatcher_handlers.c`, `dispatcher_triage.c`, once implemented per P1-13) | 80% | 75% | Lower bar — these are newly-implemented, lower-risk glue |

### 2.2 Representative unit test cases

| ID | Target | Case |
|---|---|---|
| TC-UT-001 | `registry_resolve()` | Tier 1 (manifest) hit returns correct locator, no Tier 2 call made |
| TC-UT-002 | `registry_resolve()` | Tier 1 miss, Tier 2 hit — returns correct locator, triggers `manifest_repair_async()` |
| TC-UT-003 | `registry_resolve()` | Both tiers miss — returns false, caller maps this to `-32601` |
| TC-UT-004 | `acl_policy_store_query()` | Explicit deny rule wins over a matching allow rule (deny-first) |
| TC-UT-005 | `acl_policy_store_query()` | Write grant with no explicit read grant still permits a read call (write-implies-read) |
| TC-UT-006 | `acl_policy_store_query()` | No matching group at all — denies by default, does not fail open |
| TC-UT-007 | JSON-RPC parser | Well-formed request with all required fields parses correctly |
| TC-UT-008 | JSON-RPC parser | Missing `jsonrpc` field is rejected before reaching ACL/resolution |
| TC-UT-009 | `toolset.push` verifier | Valid signature from a trusted signer accepted |
| TC-UT-010 | `toolset.push` verifier | Signature from an untrusted/unknown signer rejected |
| TC-UT-011 | `toolset.push` verifier | Artifact declared as a `dlopen()`-able binary is rejected regardless of size (P1-9, A6) |
| TC-UT-012 | Phase 1 rollback logic | New version fails health check — prior version's dispatch table entry is unchanged after the attempt |

## 3. Functional / integration test suite

Each case exercises the real chain: WRP envelope in → Parodus Agent →
Dispatch Core → (ACL, resolution) → toolset → response back out.

| ID | Traces to | Case | Expected result |
|---|---|---|---|
| TC-FN-001 | P1-1 | Send a well-formed WRP-wrapped JSON-RPC 2.0 request | Response is a well-formed WRP-wrapped JSON-RPC 2.0 response with matching `transaction_uuid` |
| TC-FN-002 | P1-3 | `triage.capabilities` from an identity with read access | Response lists every loaded triage plugin, tagged `load_type` correctly |
| TC-FN-003 | P1-3 | `triage.capabilities` reflects a plugin added since last query | Newly-loaded plugin appears without a device restart |
| TC-FN-004 | P1-4 | Real command (e.g. `wifi.setChannel`) from an authorized identity | Command executes, result reflects the actual applied state, not just an echo of the request |
| TC-FN-005 | P1-4, P1-8 | Same command from an identity with read-only (not write) access | Denied with `-32000`, command does not execute |
| TC-FN-006 | P1-5 | `toolset.push` with valid signature and a non-binary artifact | Synchronous `{"status": "loaded", ...}` response; subsequent calls route to the new version |
| TC-FN-007 | P1-5, P1-10 | `toolset.push` where the new version fails its health check | Synchronous rejection response; subsequent calls still route to the prior version, unaffected |
| TC-FN-008 | P1-9 | `toolset.push` attempting to carry a `dlopen()`-able binary | Rejected outright, regardless of declared size |
| TC-FN-009 | P1-11 | Toolset load/unload/reload event | Device-identity-authenticated capability push fires to the cloud automatically, no polling needed |
| TC-FN-010 | P1-12 | Same command (TC-FN-004) issued over the local UDS interface instead of cloud/WRP | Identical ACL outcome and identical result shape — no local-socket privilege difference |
| TC-FN-011 | P1-2 | A `dynamic`-typed message with a tampered signature | Rejected before the command portion is acted on |
| TC-FN-012 | P1-13 | Repo housekeeping check | `dispatcher_handlers.c`/`dispatcher_triage.c` exist and the configured `cppcheck`/schema-validator hooks run against them without a "file not found" failure |

## 4. Negative test suite

Deliberately malformed, hostile, or boundary-violating input — this
suite exists because Phase 1's in-process exception (P1-6) means
fewer containment layers catch a mistake before it reaches real code,
making input validation more load-bearing here than it would be once
Phase 2's sandboxing lands.

| ID | Case | Expected result |
|---|---|---|
| TC-NEG-001 | Malformed JSON payload inside a well-formed WRP envelope | JSON-RPC parse error returned; no partial processing |
| TC-NEG-002 | JSON-RPC request missing the `type` field on a command-carrying message | Rejected — `type` is mandatory per P1-2, not inferred |
| TC-NEG-003 | Request naming a nonexistent toolset | `-32601`, not a crash or a hang |
| TC-NEG-004 | Request naming a real toolset but a nonexistent method on it | `-32601`, distinguishable in logs from a nonexistent-toolset case |
| TC-NEG-005 | Expired or malformed SAT token | Rejected before the ACL check even runs *(test blocked pending A1's confirmation — see §7)* |
| TC-NEG-006 | Valid token, but caller has no ACL grant for the target method at all (not even a deny — simply absent) | Denied — absence of a grant is not fail-open |
| TC-NEG-007 | `toolset.push` from an identity with `tools/call`-style access to the toolset but not the dedicated `toolset-publish` scope | Rejected — the two scopes are independently gated, one never satisfies the other |
| TC-NEG-008 | `toolset.push` with a replayed (previously-used) nonce | Rejected, even with an otherwise-valid signature |
| TC-NEG-009 | `toolset.push` artifact field containing unexpected/unrecognized content | Rejected safely, not passed through to the loader unchecked *(exact expected shape blocked pending C8)* |
| TC-NEG-010 | Oversized request payload | Rejected at the transport/parse boundary, not allowed to reach resolution or ACL logic |
| TC-NEG-011 | Two `toolset.push` requests for the same toolset arriving concurrently | Second request is serialized behind the first, not processed as an interleaved partial update |
| TC-NEG-012 | A command request during an in-flight `toolset.push` health check for the same toolset | Routed to whichever version is currently marked "serving" — never to a half-loaded new version |

## 5. Performance test plan

Phase 1's in-process model should be *fast* — that's part of its
rationale (no IPC/process-spawn overhead to pay yet, per `docs/22`
§2.2 step 5's no-op). This suite exists to confirm that expectation
rather than assume it, and to give Phase 2's out-of-process hardening
a real "before" number to compare against later.

| ID | Metric | Method | Target (initial, to be revised with real hardware data) |
|---|---|---|---|
| TC-PERF-001 | End-to-end latency, `triage.capabilities` | p50/p95/p99 over 1,000 sequential calls | p95 < 50ms on reference hardware |
| TC-PERF-002 | End-to-end latency, real command execution | p50/p95/p99 over 1,000 sequential calls | p95 < 75ms (includes ACL check + resolution, absent in read-only case) |
| TC-PERF-003 | ACL checkpoint overhead in isolation | Time `acl_policy_store_query()` alone, excluding transport/parse | < 2ms p95 |
| TC-PERF-004 | Two-tier resolution overhead, Tier 1 hit | Time `registry_resolve()` alone | < 1ms p95 |
| TC-PERF-005 | Two-tier resolution overhead, Tier 2 fallback | Time `registry_resolve()` when Tier 1 misses | < 5ms p95 — Tier 2's live self-registration query is expected to be slower; this confirms it doesn't dominate |
| TC-PERF-006 | `toolset.push` synchronous accept/reject latency | Time from request receipt to synchronous response | < 200ms p95 for a small (non-binary) artifact |

## 6. Stress test plan

| ID | Case | What it's actually testing |
|---|---|---|
| TC-STR-001 | Sustained concurrent real-command traffic (e.g. 50 concurrent callers, 10 minutes) | Dispatch Core and the in-process command-execution toolset(s) remain responsive and correct under load — no request corruption, no ACL check skipped under contention |
| TC-STR-002 | Repeated `toolset.push` cycles (push → health-check pass → push again) in rapid succession | The in-process swap mechanism (P1-10) doesn't leak the discarded prior version's resources, and doesn't leave a window where neither version is fully "serving" |
| TC-STR-003 | Repeated `toolset.push` cycles where every health check fails | Confirms the prior version keeps serving indefinitely under repeated failed updates — no forced cutover, no degraded state accumulating across attempts |
| TC-STR-004 | **Deliberate crash inside the in-process command-execution toolset** (e.g. a command handler that null-derefs or throws unhandled) | **Documents the known, accepted risk of P1-6's exception (NFR-2 weakened):** confirm whether this brings down Dispatch Core itself, and if so, confirm the failure is clean (process exits, supervisor restarts it, no corrupted ACL/session state survives) rather than silently returning a wrong result. This test's job is to make the blast radius *known and measured*, not to make it small — Phase 1 does not claim NFR-2 isolation, and this suite proves that claim is accurate rather than assumed |
| TC-STR-005 | High-frequency `triage.capabilities` polling from multiple callers simultaneously with concurrent toolset load/unload events | Confirms discovery responses stay internally consistent (no torn reads of the plugin registry) under concurrent mutation |

## 7. Blocked test cases — explicit list

These cannot be executed until the noted open item resolves. Listed
here rather than silently omitted, so test coverage gaps are visible,
not accidental.

| Blocked case(s) | Blocked on | What unblocks it |
|---|---|---|
| TC-NEG-005, any SAT-token-specific unit tests | A1 (SAT token format) | A1's confirmation and implementation |
| TC-NEG-009 | C8 (`toolset.push` artifact field contents) | C8's resolution |
| Full negative coverage of `docs/20`'s ACL design | B5/B6 real implementation (currently design + sketch only) | Implementation landing, not just design |

## 8. Entry and exit criteria

**Entry criteria for this test plan to begin execution:** A1 and C8
resolved; `docs/20`'s ACL design implemented (not just designed);
P1-13's housekeeping files exist.

**Exit criteria for Phase 1 to be considered test-complete:** all
unit coverage targets in §2.1 met; every functional case in §3
passing; every negative case in §4 passing (§7's blocked cases
included, once unblocked); TC-PERF-001 through 006 meeting their
targets or having revised, justified targets recorded; TC-STR-001
through 005 executed and their results — including TC-STR-004's
blast-radius finding — recorded in this document's revision history,
not just left as a pass/fail without the actual measured behavior
written down.

## 9. Traceability matrix

| Requirement (docs/21) | Unit | Functional | Negative | Performance | Stress |
|---|---|---|---|---|---|
| P1-1 | TC-UT-007, 008 | TC-FN-001 | TC-NEG-001 | — | — |
| P1-2 | — | — | TC-NEG-002, 011 | — | — |
| P1-3 | — | TC-FN-002, 003 | — | TC-PERF-001 | TC-STR-005 |
| P1-4 | — | TC-FN-004 | TC-NEG-003, 004 | TC-PERF-002 | TC-STR-001 |
| P1-5 | TC-UT-009–012 | TC-FN-006, 007 | TC-NEG-007–009, 011, 012 | TC-PERF-006 | TC-STR-002, 003 |
| P1-6 | — | — | — | — | TC-STR-004 |
| P1-7 | — | — | — | — | — |
| P1-8 | TC-UT-004–006 | TC-FN-005 | TC-NEG-006 | TC-PERF-003 | TC-STR-001 |
| P1-9 | TC-UT-011 | TC-FN-008 | — | — | — |
| P1-10 | TC-UT-012 | TC-FN-007 | TC-NEG-012 | — | TC-STR-002, 003 |
| P1-11 | — | TC-FN-009 | — | — | — |
| P1-12 | — | TC-FN-010 | — | — | — |
| P1-13 | — | TC-FN-012 | — | — | — |
| P1-14 | — (blocked) | — (blocked) | TC-NEG-005 (blocked) | — | — |
