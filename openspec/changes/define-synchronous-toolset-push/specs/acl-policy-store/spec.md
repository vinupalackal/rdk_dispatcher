# Delta for acl-policy-store

## ADDED Requirements

### Requirement: Dedicated toolset-publish permission class
The system SHALL support a permission class scoped specifically to
`toolset.push` authorization, structurally separate from the
`read`/`write` scopes granted over a toolset's own methods. This
permission SHALL be assignable only to identities intended to publish
toolset code (e.g., a build/release pipeline identity), not to
ordinary operational or AI-agent-facing identities.

#### Scenario: Toolset-publish scope is independently grantable and revocable
- GIVEN an identity holding full read/write access to every toolset's
  methods via `tools/call`
- WHEN that identity's permissions are reviewed
- THEN it does not hold the toolset-publish scope unless explicitly
  and separately granted, and revoking its toolset method access does
  not implicitly revoke or grant toolset-publish
