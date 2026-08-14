# Proposal: Unified Toolset Architecture Across All Planes

**Revised 2026-08-13, per explicit direction.** The original version
of this change split planes (first-party, in-process, exempt from
FR-13/FR-14) from toolsets (distributable, out-of-process, sandboxed).
That split is withdrawn here. The corrected, simpler answer: **keep
the toolset architecture for all four planes.** Plane is a descriptive
tag on a toolset, not a separate execution-trust tier. There is no
first-party, in-process exemption for any plane's logic.

## Intent

`docs/19_architecture_and_code_review_findings.md` raised two
questions that this change now answers together, with one answer
instead of two:

1. How does `CLAUDE.md`'s four-plane model (config-apply, management,
   control, triage) relate to the main spec's toolset model (common,
   network, wifi, DOCSIS, vendor)?
2. Is the RDK-B reference-impl's in-process, `dlopen()` +
   `run_with_timeout()` plugin model an acceptable divergence from
   FR-13/FR-14, or does it need to become out-of-process?

Answer: plane and toolset are not two competing models — every plane
is implemented *as* one or more toolsets, uniformly out-of-process and
sandboxed like any other toolset, and the cloud manages all of them
identically by naming a toolset and passing arguments. Question 2's
answer follows directly: yes, it needs to become out-of-process — there
was never a valid exemption for it to rely on.

## Scope

In scope:
- Establishing plane as a purely descriptive tag on toolset logic —
  what functional category it belongs to, nothing about trust,
  distribution, or execution model.
- Establishing that every toolset, regardless of which plane(s) it
  implements, is governed uniformly by FR-13/FR-14 (out-of-process,
  sandboxed) and the toolset install/lifecycle requirements
  (Toolset Store/RDM Client, `toolset.push`, Plugin Manager
  supervision).
- Establishing that the cloud's invocation model is uniform across
  planes: name a toolset, pass arguments, via `tools/call` — no
  plane-specific invocation path.
- Reopening (not silently dropping) the isolation-tradeoff question
  in `CLAUDE_CODE_WORKFLOW.md` as real, unresolved follow-up work.

Out of scope:
- Actually rewriting `reference-impl/` to move plane logic
  out-of-process — tracked as a task, not done in this change.
- Sizing the real IPC/latency cost of out-of-process plane operations
  on the embedded event loop — this is now explicitly tied to the
  still-open footprint budget question (`OPEN_QUESTIONS.md` B3), not
  resolved here.
- RDK-V applicability of the plane model — separate open question,
  unchanged by this revision.
