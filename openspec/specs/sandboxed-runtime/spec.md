# Sandboxed Toolset Plugin Runtime Specification

## Purpose

Executes every toolset plugin as an isolated, least-privilege
out-of-process unit, so a crashing or compromised plugin cannot affect
Dispatch Core, other plugins, or reach beyond its declared needs.

## Requirements

### Requirement: Out-of-process execution
The system SHALL execute every toolset plugin in its own OS process.
No toolset plugin SHALL be loaded into Dispatch Core's own address
space.

#### Scenario: Plugin crash does not affect the Dispatcher
- GIVEN a running toolset plugin process
- WHEN that process crashes due to a bug in its own code
- THEN Dispatch Core, Plugin Manager, and all other toolset plugin
  processes continue operating unaffected

### Requirement: Namespace isolation
The system SHALL launch each toolset plugin process inside its own
mount and PID namespace.

#### Scenario: Plugin cannot see other plugins' processes
- GIVEN two toolset plugin processes running concurrently
- WHEN one process lists visible processes
- THEN it observes only its own process tree, not the other plugin's
  or the Dispatcher's

### Requirement: Per-plugin syscall allowlist
The system SHALL enforce a `seccomp-bpf` syscall allowlist defined per
plugin, not globally.

#### Scenario: DOCSIS toolset needs different syscalls than a stateless toolset
- GIVEN a DOCSIS toolset plugin manifest declaring hardware-access
  syscalls
- AND a common toolset plugin manifest declaring only basic syscalls
- WHEN each plugin is launched
- THEN each receives a distinct seccomp profile matching its own
  manifest, not a shared global list

### Requirement: Resource limits via cgroup
The system SHALL enforce Execution Framework's declared timeouts and
resource limits through a `cgroup` applied to each plugin process, not
as an unenforced convention.

#### Scenario: Plugin exceeding memory limit is contained
- GIVEN a plugin process with a cgroup memory limit set from its
  declared resource limits
- WHEN the process attempts to exceed that limit
- THEN the kernel enforces the limit (e.g., OOM-kills the process)
  rather than the limit being merely advisory

### Requirement: Least-privilege capabilities
The system SHALL run each toolset plugin as a non-root UID with Linux
capabilities dropped to the minimum set declared in its manifest, and
read-only filesystem access except an explicitly granted scratch path.

#### Scenario: Plugin requests only what it declares
- GIVEN a toolset plugin manifest declaring no elevated capabilities
- WHEN the plugin is launched
- THEN it runs without `CAP_NET_ADMIN`, `CAP_SYS_ADMIN`, or any
  capability not explicitly declared, and cannot write outside its
  granted scratch path
