# Platform Abstraction & Execution Adapters Specification

## Purpose

Bridges decoded, already-ACL-checked toolset plugin calls into RDK's
real subsystems (RBUS on broadband, IARM/Thunder on video, dmcli/CLI
as fallback) without becoming a confused-deputy path around each
subsystem's own native access control.

## Requirements

### Requirement: Structural separation per subsystem
The system SHALL implement RBUS, IARM/Thunder, and dmcli/CLI as
independent adapter components, each owning its own subsystem-specific
translation logic. No generic adapter SHALL merge these.

#### Scenario: RBUS and Thunder adapters evolve independently
- GIVEN a change to the RBUS adapter's TR-181 parameter handling
- WHEN that change is implemented
- THEN the IARM/Thunder adapter's COM-RPC/JSON-RPC handling is
  unaffected and requires no corresponding change

### Requirement: Caller identity propagation
Every adapter SHALL forward or re-derive the original caller's
identity into its underlying subsystem's own native ACL check. No
adapter SHALL execute a call under a single, fixed, elevated identity
that bypasses that subsystem's own access control.

#### Scenario: Adapter cannot bypass RBUS's own ACL
- GIVEN a request that passed Dispatch Core's ACL check
- WHEN the RBUS Adapter forwards the resulting RBUS call
- THEN RBUS's own ACL evaluates the call against the original
  caller's identity, not a blanket service-account identity the
  adapter runs under

#### Scenario: A Dispatcher-level bug does not grant RBUS-level access
- GIVEN a hypothetical bug in Dispatch Core's ACL evaluation that
  incorrectly allows a request
- WHEN the RBUS Adapter still forwards the original caller's identity
- THEN RBUS's own independent ACL check still has the opportunity to
  deny the call, providing defense in depth

### Requirement: Profile-specific adapter set
Only the adapter layer SHALL differ between RDK-B and RDK-V profiles;
Dispatch Core, Plugin Manager, ACL Policy Store, and Execution
Framework SHALL be identical across both.

#### Scenario: Same Dispatcher binary runs on both profiles
- GIVEN a Dispatch Core build with no profile-specific code
- WHEN deployed to an RDK-B gateway and an RDK-V set-top box
- THEN only the set of active adapters (RBUS vs. IARM/Thunder) differs
  between the two deployments
