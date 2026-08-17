# Tasks

**Revised 2026-08-13** — replaces the original task list, which
assumed the now-withdrawn plane exemption.

## 1. Spec updates
- [ ] 1.1 `sandboxed-runtime/spec.md`: remove the "toolsets only, not
      dispatcher-core planes" exemption; add a requirement stating
      FR-13/FR-14 applies uniformly regardless of plane
- [ ] 1.2 `toolset-lifecycle/spec.md`: retain "a toolset may implement
      multiple planes internally"; remove any wording implying plane
      logic can live outside the toolset architecture

## 2. Documentation resolution
- [x] 2.1 Reopen `CLAUDE_CODE_WORKFLOW.md`'s "Isolation trade-off"
      open thread — done, see the file directly
- [ ] 2.2 Update `docs/19`'s §4 disposition note to reflect the
      revised (not withdrawn) resolution

## 3. Required follow-up: rework, not just spec text
- [ ] 3.1 Redesign `reference-impl/dispatcher_core.c`'s plugin loader
      to run plane logic out-of-process, supervised by Plugin Manager,
      instead of `dlopen()`-ing into the same process
- [ ] 3.2 Size the IPC/latency overhead this introduces against
      `CLAUDE.md`'s real-time constraints (sysevent thread must never
      block) — feeds `OPEN_QUESTIONS.md` B3, footprint budget
- [ ] 3.3 If sizing in 3.2 finds an unacceptable cost for specific
      fast-path control operations, bring that back as an explicit,
      reviewed exception request against Decision B — do not quietly
      revert to in-process without recording why

## 4. Verification against spec
- [ ] 4.1 Confirm scenario: a vendor-supplied DOCSIS toolset requires
      out-of-process execution and full sandboxing, regardless of
      which planes its internal logic touches (unchanged from before)
- [ ] 4.2 Confirm scenario: a first-party triage-plane toolset shipped
      by the dispatcher's own team is *also* out-of-process and
      sandboxed — no exemption for being first-party
- [x] 4.3 ~~Confirm NFR-1 (portability) is unaffected in principle by
      this revision — still tracked separately for RDK-V applicability~~
      — resolved 2026-08-13, Decision D: NFR-1 stays a hard
      requirement; the plane model applies identically to RDK-V,
      isolated behind a per-platform event namespace (see
      `OPEN_QUESTIONS.md` A9)

## 5. New, from Decision D (RDK-V portability)
- [ ] 5.1 Document the namespace-relative event-name convention in
      `plugin_contract.h` — `events` entries resolve against a
      platform-supplied namespace table, not raw sysevent/Netlink names
- [ ] 5.2 Define where a platform's namespace table itself lives and
      how it's authored (config file shipped per-platform build? a
      Platform Adapter responsibility?) — not decided by Decision D,
      only that the indirection exists
- [ ] 5.3 Decide, per real event, whether RDK-B and RDK-V share one
      namespace entry or diverge — left to platform integrators per
      Decision D, not fixed here; needs a first real worked example
      before treating this as settled in practice

## 6. New, from Decision F (application as a toolset domain)
- [x] 6.1 ~~Document "application" in the extensible toolset-domain
      list~~ — done, `RDK_Dispatcher_Architecture_and_Requirements.md`
      §4.3
- [ ] 6.2 Once real application toolsets are being designed, confirm
      by worked example that config-apply/management/control/triage
      logic for an app genuinely fits the existing four planes with no
      awkward edge case — Decision F is reasoned, not yet tested
      against a real application toolset's actual method list
