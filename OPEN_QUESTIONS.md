# RDK Dispatcher — Consolidated Open Questions

Every open item across this project's docs, gathered in one place so
nothing stays scattered. Grouped by status. When a change resolves an
item here, update its status rather than deleting the row — this file
is the project's running record of what's actually settled versus
still assumed.

## A. Resolved in draft, not yet archived

These have a concrete decision and an OpenSpec change behind them —
they still need `/opsx:apply` + `/opsx:archive` (or your explicit
sign-off) before being treated as settled.

| # | Question | Resolved by | Status |
|---|---|---|---|
| A1 | SAT token format and revocation mechanism (NFR-10) | `openspec/changes/define-sat-token-format/` — JWT, 5-min expiry + refresh, no revocation list | Drafted, unarchived |
| A2 | Does the RDK-B embedded build need to be out-of-process/sandboxed (FR-13/FR-14) to be a real implementation of this spec? | `openspec/changes/define-plane-vs-toolset-model/` — planes (config-apply/management/control/triage) are first-party, in-process, not FR-13/14-governed; toolsets are, regardless of which planes their internal logic touches | Drafted, unarchived |
| A3 | How does the four-plane model relate to the toolset model? | Same change as A2 — orthogonal, not siblings; a toolset may implement multiple planes internally | Drafted, unarchived |
| A4 | Should toolsets be MCP-discoverable/invocable, and does dynamic push require out-of-process execution? | `openspec/changes/define-toolset-as-mcp-tool-model/` — yes to both; persistent supervised process, not fork-per-call or `dlopen()` | Drafted, unarchived |
| A5 | Does rpcd-style low-friction toolset push bypass RDM Client's signature/manifest verification? | Same change as A4 — no, verification stays mandatory regardless of push convenience | Drafted, unarchived — this is my recommendation, not yet confirmed by you |
| A6 | Can a secure push happen synchronously over the same JSON-RPC/WRP channel instead of RDM Client's async download flow? | `openspec/changes/define-synchronous-toolset-push/` — yes, for plugin-scale artifacts specifically: `toolset.push` method, inline signature verification, RDM Client unchanged and still required for firmware/large-package installs | Drafted, unarchived |
| A7 | Is payload-level encryption required beyond transport TLS, and how do static (definition) vs. dynamic (command) WRP pushes route on the device? | `openspec/changes/require-payload-encryption-and-message-routing/` — yes, required, layered under TLS/WSS; message kind (definition/command) is decided post-decryption, before ACL, and is explicitly a different axis from `load_type` (compiled-in/dlopen). Supplied directly by you as a diagram; see `toolset_push_and_command_flow.svg` | Drafted, unarchived |

## B. Long-standing, still genuinely open

No proposed resolution yet.

| # | Question | First raised |
|---|---|---|
| B1 | RBUS and Thunder per-toolset translation logic — which TR-181 parameters or Thunder plugins each toolset actually maps to | `RDK_Dispatcher_Architecture_and_Requirements.md` §8 |
| B2 | Sandbox profile authoring workflow — who writes/reviews a toolset's seccomp/capability manifest, and how it changes post-install | `RDK_Dispatcher_Architecture_and_Requirements.md` §8 |
| B3 | Footprint budget for Dispatch Core + Plugin Manager + N sandboxed toolset processes, sized against real target hardware | `RDK_Dispatcher_Architecture_and_Requirements.md` §8 |
| B4 | Independent security review of NFR-3 (least privilege) and NFR-4 (no confused deputy) — design-reviewed only so far, never independently verified | `RDK_Dispatcher_Architecture_and_Requirements.md` §8 |
| B5 | No ACL check exists anywhere in `reference-impl/` or `CLAUDE.md`'s described flow | `docs/19` §2.3 |
| B6 | No code sketch exists for Dispatch Core receiving a cloud JSON-RPC (or now MCP `tools/call`) request and resolving it via Plugin Manager — only the local sysevent/Netlink event path is sketched | `docs/19` §2.4 |
| B7 | Dispatch Core's claimed portability (NFR-1: identical across RDK-B/RDK-V) is in tension with how CCSP/sysevent/Netlink-coupled the actual reference-impl is; does the plane model even apply to RDK-V the same way? | `docs/19` §2.5, `USER_GUIDE.md` |
| B8 | WRP `source`/`dest` addressing used in Phase 1 (`mac:.../rdk-dispatcher/triage`, `dns:skillset-mapper.xmidt.example.com/svc`) is illustrative, not checked against real Parodus service-registration conventions | `docs/19` §3 |
| B9 | RDM Client's install/rollback unit is a whole toolset package (FR-11/FR-12) — how does this reconcile with a toolset containing per-plugin static/dynamic sub-parts, once Phase 1 is corrected to aggregate rather than treat triage as its own toolset? | `docs/19` §3 |
| B10 | `.claude/settings.json`'s hooks reference `dispatcher_handlers.c`/`dispatcher_triage.c`, which don't exist in this repo yet — housekeeping, confirms nothing is implemented, not a design gap | `docs/19` §3 |

## C. Newly surfaced this session — need your input specifically

These came directly out of the MCP/toolset-exposure discussion and
don't have a recommended default in the same way A1–A5 do; they're
real forks in the design, not just gaps to fill in.

| # | Question | Why it matters |
|---|---|---|
| C1 | Should config/state-reads be modeled as MCP **resources**, distinct from invocable MCP **tools**, or should everything — reads and actions alike — stay unified as tools? | Affects the shape of every plane's exposure, not just triage; resources vs. tools is a real MCP-native distinction this design hasn't picked a side on |
| C2 | Does `tools/call` become the **only** supported command shape going forward, or does a non-MCP JSON-RPC 2.0 path stay supported indefinitely for cloud/ops clients that aren't MCP-aware? | Determines whether FR-1's generic JSON-RPC framing and the new MCP surface are one path or two permanently coexisting ones |
| C3 | ~~Toolset push/discovery mechanism~~ — superseded: resolved as two paths (persistent process for RDM-installed toolsets, synchronous inline push for plugin-scale artifacts) per A6 | Moved to A6 |
| C6 | Exact size/artifact-type threshold separating `toolset.push` from RDM Client's path | `define-synchronous-toolset-push` deliberately left this a policy decision rather than inventing a number — needs real WRP/WebSocket message-size and target-hardware footprint data (ties to B3) |
| ~~C7~~ | ~~Does this project have a confidentiality requirement transport TLS doesn't cover, justifying payload encryption?~~ — resolved: yes | Moved to A7 |
| C4 | `notifications/tools/list_changed` and `capability-sync`'s existing device-identity-authenticated push both fire from the same trigger and read the same data through two separate mechanisms — is keeping them permanently separate the right call, or should one become the source feeding the other over time? | Flagged as deliberately unmerged in `define-toolset-as-mcp-tool-model`, but worth a real decision rather than defaulting to "keep both forever" |
| C5 | Is "each plugin's MCP tool identity" scoped per-toolset-method (`wifi.setChannel`) as currently designed, or should some plugins be exposed as coarser, single MCP tools with the method as a structured argument — closer to how some MCP servers avoid a huge flat tool list? | Affects how large and how navigable `tools/list` becomes as toolsets grow; not addressed yet |

## How to use this file

Resolve an item by drafting (or pointing to) an OpenSpec change the
same way A1–A5 were resolved, then move its row into section A with a
link. Add new rows to B or C as they surface — don't let them live
only inside a chat response again.
