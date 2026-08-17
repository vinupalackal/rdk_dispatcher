# Delta for toolset-lifecycle

## ADDED Requirements

### Requirement: Descriptive per-method metadata is a sibling field, not embedded in a `tools/list` entry's `inputSchema`
A toolset's `tools/list` entry MAY include an optional `methods` array
— one item per method, keyed by method name — carrying descriptive
metadata about that method or the plugin implementing it (for example
`load_type`, `version`, `timeout_ms`). This field SHALL be a sibling
of `inputSchema`, not embedded within any of `inputSchema`'s `oneOf`
branches. `inputSchema`'s `oneOf` branches SHALL contain only the
argument shape for invoking each method (`method` and `params`), never
descriptive metadata.

#### Scenario: A toolset's methods metadata is queryable without polluting its argument schema
- GIVEN a toolset with a `methods` array in its `tools/list` entry
- WHEN a caller inspects that entry
- THEN `inputSchema`'s `oneOf` branches contain only `method`/`params`
  keys, and per-method descriptive detail is read from the separate
  `methods` array, correlated by method name

#### Scenario: A toolset with nothing descriptive to add omits `methods` entirely
- GIVEN a toolset whose methods need no metadata beyond their argument
  shapes
- WHEN its `tools/list` entry is built
- THEN the `methods` field is simply absent — it is optional, not a
  field every toolset must populate

#### Scenario: A generic MCP client is unaffected by the presence of `methods`
- GIVEN an MCP client that only reads the standard `name`/
  `description`/`inputSchema` fields
- WHEN it receives a `tools/list` entry that includes a `methods`
  sibling field
- THEN it parses the entry correctly, ignoring the unrecognized
  additional field — `methods` is additive and never required for
  correct argument-schema interpretation
