# RDK Dispatcher — OpenSpec scaffold

This directory demonstrates how to run the RDK Dispatcher's ongoing
development through [OpenSpec](https://github.com/Fission-AI/OpenSpec),
seeded from `../RDK_Dispatcher_Architecture_and_Requirements.md`.

## Layout

```
openspec/
├── specs/                        # source of truth — one domain per component
│   ├── dispatch-core/spec.md
│   ├── acl-policy-store/spec.md
│   ├── sandboxed-runtime/spec.md
│   ├── platform-adapters/spec.md
│   ├── capability-sync/spec.md
│   └── toolset-lifecycle/spec.md
└── changes/
    ├── define-sat-token-format/  # a worked example: one of our open questions,
    │                              # run as an actual OpenSpec change
    │   ├── proposal.md
    │   ├── design.md
    │   ├── tasks.md
    │   └── specs/dispatch-core/spec.md   (delta: MODIFIED)
    └── archive/                  # empty until this example change is archived
```

Each `specs/<domain>/spec.md` was seeded directly from the FR/NFR
tables and component specs in `RDK_Dispatcher_Architecture_and_Requirements.md`
— every FR/NFR became a `### Requirement:` with at least one
`#### Scenario:`. Component boxes with only one or two requirements
each were grouped into `platform-adapters` and `toolset-lifecycle`
rather than getting a one-requirement file of their own; split them
further once a domain grows enough to warrant it.

## Running this for real

1. `npx openspec init` (or the installed CLI) in the actual RDK
   Dispatcher repo, then copy `specs/` in as the initial source of
   truth — this one-time seeding is itself worth doing as a change
   (e.g. `add-initial-dispatcher-specs`, all-ADDED deltas) so it has
   a proposal and an audit trail like everything after it.
2. For every future modification — a new toolset category, a changed
   token format, a new adapter — run `/opsx:propose <name>`, work
   through proposal → delta specs → design → tasks, implement with
   `/opsx:apply`, check it with `/opsx:verify`, then `/opsx:archive`.
3. Use OpenSpec's "Full spec" mode (not the default Lite mode) for any
   change touching `acl-policy-store` or `sandboxed-runtime` —
   these map directly to NFR-3 (least privilege) and NFR-4 (no
   confused deputy), the two items this project's own open-questions
   section flagged as needing independent security review, not just a
   design pass.
4. RBUS-adapter work and Thunder-adapter work can run as two parallel
   changes without conflicting, since they touch different scenarios
   within `platform-adapters/spec.md` — mirrors the actual RDK-B/RDK-V
   profile split.
