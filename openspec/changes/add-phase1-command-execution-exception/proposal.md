# Proposal: Phase 1 Command Execution Exception — Real Commands and Toolset Push, In-Process

## Intent

Bring real command execution and `toolset.push`
(`define-synchronous-toolset-push`) into Phase 1, ahead of the rest of
Phase 2's out-of-process/sandboxing work — per direct instruction,
reversing the earlier consolidation that moved all out-of-process work
to Phase 2.

This is deliberately scoped as an **explicit, reviewed exception** to
`define-plane-vs-toolset-model`'s Decision B (every toolset
out-of-process and sandboxed, no first-party exemption) — not a quiet
reversion of it. That document's own escape-valve clause anticipated
exactly this situation: "If out-of-process overhead turns out to be
unacceptable for the fastest control-plane paths, that needs to
surface as an explicit, reviewed exception request against this
decision — not a quiet reversion back to in-process without saying
so." This change is that request, made explicit and tracked, per
direct confirmation on scope (2026-08-14): just command execution and
toolset push move up, not the rest of Phase 2; sandboxing is the
explicit exception (in-process for now, tracked); encryption stays
Phase 2 unchanged.

## Scope

In scope:
- Real command execution (not just read-only discovery) becomes part
  of Phase 1.
- `toolset.push` becomes part of Phase 1, unchanged in its
  verification requirements and RDM boundary (`OPEN_QUESTIONS.md` A6).
- The toolset(s) handling Phase 1 commands run in-process — not
  out-of-process, not sandboxed — as a scoped, temporary exception,
  not a general reopening of Decision B.
- Dispatch Core's single ACL checkpoint (FR-4) stays mandatory,
  unaffected by the in-process exception — deferring sandboxing is not
  deferring authorization.
- A Phase-1-specific rollback model for `toolset.push`, since the
  on-demand/spawn-based fallback (`define-on-demand-toolset-execution`)
  assumes out-of-process execution Phase 1 doesn't have yet.

Out of scope (stays Phase 2, unchanged by this decision):
- Out-of-process execution and sandboxing (FR-13/FR-14) for Phase 1's
  command-executing toolset(s) — explicitly deferred, tracked as a
  real gap, not silently dropped.
- Payload encryption (`OPEN_QUESTIONS.md` A7) — Phase 1 stays plain
  JSON-RPC with mandatory verification, confirmed unchanged
  2026-08-14.
- RBUS/Thunder mapping, sandbox profile authoring workflow, footprint
  budget sizing, RDK-V namespace mapping, and the concluding NFR-3/
  NFR-4 independent security review — all still Phase 2.
- The on-demand spawn mechanism confirmation (A8) — not relevant until
  out-of-process execution actually lands in Phase 2.

## Why this is an explicit exception, not a redesign

Everything else already decided about toolsets — plane is a
descriptive tag, the cloud invokes uniformly by toolset name plus
arguments, `toolset.push`'s verification and RDM boundary — stays
exactly as designed. The only thing this change relaxes is *where the
code runs* for Phase 1 specifically, and it does so by naming the
relaxation explicitly, recording why, and scheduling the hardening —
the discipline `define-plane-vs-toolset-model` asked for if this
situation arose, not a silent reversion.
