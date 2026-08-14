# Tasks

## 1. Token issuance
- [ ] 1.1 Generate and securely persist Dispatch Core's EdDSA signing key on first boot
- [ ] 1.2 Implement JWT issuance on successful login, embedding `sub`/`groups`/`iat`/`exp`/`jti`
- [ ] 1.3 Implement a refresh endpoint for pre-expiry renewal

## 2. Token validation
- [ ] 2.1 Implement signature + expiry validation with no ACL Policy Store round-trip
- [ ] 2.2 Reject tokens with invalid signature or expired `exp`
- [ ] 2.3 Log `jti` on every validated request for audit correlation

## 3. Verification against spec
- [ ] 3.1 Confirm restart-without-session-loss scenario (dispatch-core/spec.md)
- [ ] 3.2 Confirm expired-token rejection scenario
- [ ] 3.3 Load-test claim-based validation to confirm it removes the store round-trip from the hot path
