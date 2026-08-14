# rdk-dispatcher

A standalone dispatcher and plugin framework for RDK-B (Broadband) and
RDK-V (Video) devices — modeled on the good parts of OpenWrt's
`rpcd`/`ubus` design (a clean plugin/ACL/session model), implemented
from scratch with no `ubus`/`rpcd` code or wire protocol, and bridged
into RDK's real subsystems (RBUS, IARM/Thunder) through explicit
adapters rather than replacing them.

This folder is self-contained: everything needed to understand the
design and start development lives here.

## What's in this folder

```
rdk-dispatcher/
├── README.md                                        this file
├── RDK_Dispatcher_Architecture_and_Requirements.md   the spec: components,
│                                                      data flows, FR/NFR, decisions log
├── rdk_dispatcher_architecture.svg                   the architecture diagram
├── toolset_push_and_command_flow.svg                 payload encryption + message-kind
│                                                      routing (definition vs. command push)
├── OPEN_QUESTIONS.md                                 every open question, one place, grouped
│                                                      by status — check here before assuming
│                                                      something is settled
├── USER_GUIDE.md                                     how to use OpenSpec here + next steps
├── CLAUDE.md                                         hard rules Claude Code reads automatically
├── CLAUDE_CODE_WORKFLOW.md                           how to build this day-to-day in Claude Code
├── docs/                                             the design record (how each decision
│   │                                                  was reached, checked against precedent)
│   ├── 16_rpcd_on_rdk_case_study_video_broadband.md
│   ├── 17_dispatcher_plugin_architecture_cross_check_and_standalone_implications.md
│   ├── 18_rdk_dispatcher_standalone_architecture.md
│   └── 19_architecture_and_code_review_findings.md          gap analysis vs. code
├── openspec/                                         spec-driven development workspace
│   ├── README.md
│   ├── specs/          source of truth, one domain per component
│   └── changes/        proposed modifications:
│       ├── define-sat-token-format/                  worked example (token format)
│       ├── add-triage-skillset-mapping-phase1/        Phase 1: WRP-framed triage.capabilities
│       │                                               (blocked — see OPEN_QUESTIONS.md A2-A5)
│       ├── define-plane-vs-toolset-model/             resolves plane-vs-toolset taxonomy +
│       │                                               RDK-B process-model divergence (docs/19)
│       ├── define-toolset-as-mcp-tool-model/           toolsets as MCP tools/list·tools/call;
│       │                                               out-of-process push/discovery model
│       ├── define-synchronous-toolset-push/            toolset.push: synchronous, signed,
│       │                                               plugin-scale push over the same JSON-RPC/WRP
│       │                                               channel — RDM Client stays for firmware-class
│       └── require-payload-encryption-and-message-routing/  payload encryption + definition/
│                                                       command routing (toolset_push_and_command_flow.svg)
├── .claude/                                          Claude Code scaffold: commands, hooks,
│   │                                                  sub-agents, skill (see CLAUDE_CODE_WORKFLOW.md)
│   ├── commands/explain-handler.md
│   ├── settings.json
│   ├── agents/{hal-agent.md, triage-agent.md}
│   └── skills/
│       ├── ccsp-component-conventions/SKILL.md         CCSP handler code
│       └── dispatcher-protocol-conventions/SKILL.md    Dispatch Core's request
│                                                        path: encryption, message
│                                                        routing, MCP, toolset.push
└── reference-impl/                                   illustrative plugin-loader sketch (not
                                                        reviewed production code) — plugin_contract.h,
                                                        dispatcher_core.c, triage_capabilities.c,
                                                        plugins/{triage_wifi.c, triage_core_static.c}
```

## Reading order, if you're new to this project

1. `RDK_Dispatcher_Architecture_and_Requirements.md` — start here. It's
   the standalone spec; you don't need to read the `docs/` files to
   understand what's being built.
2. `rdk_dispatcher_architecture.svg` — the visual layout.
3. `docs/16` → `17` → `18`, in order, only if you want the reasoning
   behind each design decision (why `rpcd` can't be ported as-is, how
   the dispatch model was cross-checked against `ubus`/RBUS/Thunder,
   and how the standalone architecture — including sandboxing and the
   ACL Policy Store — was arrived at).
4. `docs/19` — a review pass checking the specs and diagram above
   against the actual `reference-impl/` code and `CLAUDE.md`; read this
   before trusting the RDK-B build-out as a faithful implementation of
   the main spec, since it isn't yet one in every respect.
5. `USER_GUIDE.md` — how to actually work on this project day to day
   using OpenSpec, and what to do next.
6. `CLAUDE_CODE_WORKFLOW.md` — if you're building the RDK-B side in
   embedded C, this is the concrete Claude Code workflow (custom
   commands, hooks, sub-agents, the plugin-loader sketch) for it.

## Status

Design complete at the specification level. Not yet implemented, and
not yet reviewed by a security team — see
`RDK_Dispatcher_Architecture_and_Requirements.md` §8 for the open
questions blocking a production build.
