# Capability Sync Specification

## Purpose

Reports the device's currently installed toolset capabilities to the
cloud automatically, over the same transport used for commands, so the
Cloud Tool & Skill Platform's device model mapping and tool catalog
stay current without polling.

## Requirements

### Requirement: Event-triggered reporting
The system SHALL report updated toolset capabilities to the cloud when
Plugin Manager loads, unloads, or reloads a toolset. The system SHALL
NOT rely on the cloud polling the device for this information.

#### Scenario: New toolset triggers a capability push
- GIVEN a device with no pending capability updates
- WHEN Plugin Manager successfully loads a new toolset plugin
- THEN Schema & Discovery's capabilities for that toolset are sent to
  the cloud without any cloud-initiated request

### Requirement: Shared transport, separate authentication
The system SHALL send capability-sync traffic over the same Transport
Adapter and XMiDT connection as command traffic, but authenticated by
device identity rather than a per-session command token.

#### Scenario: Capability sync succeeds without an active user session
- GIVEN no cloud/ops client is currently authenticated to the device
- WHEN a toolset reload triggers a capability sync
- THEN the sync succeeds using device-identity authentication, not
  requiring any per-session SAT token
