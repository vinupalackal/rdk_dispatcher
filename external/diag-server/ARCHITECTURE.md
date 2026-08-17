# Diag-Server Architecture Diagram

This architecture is derived from the runtime behavior implemented in the service code.

## 1. Component Architecture

```mermaid
flowchart TB
    subgraph Cloud[Cloud Side]
        OPS[OPS Gateway]
    end

    subgraph Device[Device Side]
        PAR[Parodus\nTCP 127.0.0.1:6666]

        subgraph DS[Diag-Server Process]
            MAIN[Main Thread\nLifecycle + Receive Loop]
            REG[Registration Builder\nWRP msg_type=9]
            WRPDEC[WRP Decoder\nOuter msgpack map]
            PAYDEC[Payload Decoder\nInner map: tool, command]
            CAT[Catalog Manager\nLoad + Lookup JSON]
            SAFETY[Safety Gate\nBlocked first-token list]
            EXEC[Command Executor\npopen + stdout capture]
            RESP[Response Builder\nInner payload + outer WRP]
            KA[Keepalive Handler\nmsg_type=10]
            WORK[Detached Worker Threads\nOne per request]
        end

        CATALOG[(catalog.json)]
    end

    OPS -->|WRP via Parodus| PAR
    PAR -->|REQ msg_type=3\nPULL 127.0.0.1:6669| MAIN
    MAIN --> WRPDEC
    WRPDEC --> PAYDEC
    PAYDEC --> CAT
    CATALOG --> CAT
    CAT --> SAFETY
    SAFETY --> WORK
    WORK --> EXEC
    EXEC --> RESP
    RESP -->|Response msg_type=3\nPUSH 127.0.0.1:6666| PAR
    PAR --> OPS

    MAIN -->|Startup registration| REG
    REG -->|msg_type=9| PAR

    MAIN -->|Incoming keepalive msg_type=10| KA
    KA -->|Ack msg_type=10| PAR
```

## 2. Request Processing Workflow

```mermaid
sequenceDiagram
    participant O as OPS Gateway
    participant P as Parodus
    participant M as Diag-Server Main Thread
    participant W as Worker Thread
    participant C as Catalog
    participant E as Command Executor

    O->>P: WRP Request (msg_type=3)
    P->>M: Forward request to PULL socket
    M->>M: Decode outer WRP map
    M->>M: Decode payload (tool, optional command)
    M->>W: Dispatch request (detached thread)

    W->>C: Lookup tool in catalog
    alt tool exists
        W->>W: Resolve command override or catalog default
        W->>W: Blocked-token safety check
        alt command allowed
            W->>E: Execute command
            E-->>W: stdout + exit code
        else command blocked
            W->>W: Build blocked error result
        end
    else tool missing
        W->>W: Build tool-not-found result
    end

    W->>W: Build inner payload (tool, exit_code, stdout)
    W->>W: Build outer WRP response (msg_type=3)
    W->>P: Send response over PUSH socket
    P->>O: Return response
```

## 3. Runtime Boundaries and Responsibilities

1. Main Thread
- Owns startup, socket setup, registration, receive loop, and keepalive acknowledgments.
- Stays responsive by delegating request execution to detached workers.

2. Worker Thread
- Owns per-request decoding context, catalog validation, command safety validation, execution, and response send.

3. Catalog
- Defines allowed tool names and default commands.
- Acts as first allowlist boundary for remote requests.

4. Safety Gate
- Denies execution when the first command token is in blocked set.

5. Executor
- Captures command stdout with a fixed upper bound.
- Returns process exit code for response payload.

## 4. Key Interfaces

- Inbound from Parodus: WRP msgpack over nanomsg PULL, bound at 127.0.0.1:6669.
- Outbound to Parodus: WRP msgpack over nanomsg PUSH, connected to 127.0.0.1:6666.
- Catalog file: JSON from default /etc/diag-server/catalog.json or startup override argument.

## 5. Notes from Current Code Baseline

- Catalog timeout field exists in JSON but is not enforced by current execution path.
- Command execution currently uses shell-based popen behavior.
- Request handlers run concurrently in detached pthreads.

## 6. Interface Sub-Component Workflows

### 6.1 Transport Interface Workflow (Socket Lifecycle)

```mermaid
flowchart TD
    S0[Process Start] --> S1[Create PULL socket]
    S1 --> S2[Set RCVTIMEO + RCVBUF]
    S2 --> S3[Bind PULL to 127.0.0.1:6669]
    S3 --> S4[Create PUSH socket]
    S4 --> S5[Set SNDTIMEO]
    S5 --> S6{Connect PUSH to 127.0.0.1:6666 successful?}
    S6 -- No --> S7[Log error and backoff retry]
    S7 --> S6
    S6 -- Yes --> S8[Send registration message]
    S8 --> S9[Enter receive loop]
    S9 --> S10{Signal received?}
    S10 -- No --> S9
    S10 -- Yes --> S11[Close PUSH + PULL sockets]
    S11 --> S12[Shutdown complete]
```

Primary code path mapping:
- main: socket create/bind/connect/retry/loop/shutdown.

### 6.2 Registration Interface Workflow (WRP Type 9)

```mermaid
sequenceDiagram
    participant M as Main Thread
    participant RB as Registration Builder
    participant NN as nanomsg PUSH
    participant P as Parodus

    M->>RB: build_registration()
    RB->>RB: Pack msg_type=9
    RB->>RB: Pack service_name and url
    RB-->>M: Serialized msgpack bytes
    M->>NN: nn_send(registration)
    NN->>P: WRP registration frame
    alt send failed
        M->>M: Log registration send failure
    else send success
        M->>M: Log registered service
    end
```

Primary code path mapping:
- build_registration: constructs the type-9 map.
- main: sends registration bytes over PUSH.

### 6.3 Request Decode and Dispatch Workflow

```mermaid
flowchart TD
    R0[Receive bytes via nn_recv] --> R1[Allocate wrp_req_t]
    R1 --> R2[decode_wrp outer map]
    R2 --> R3{msg_type}
    R3 -- 3 (REQ) --> R4[Create detached worker thread]
    R4 --> R5{pthread_create success?}
    R5 -- Yes --> R6[Worker owns request]
    R5 -- No --> R7[Fallback process inline]
    R3 -- 10 (ALIVE) --> R8[Build keepalive ack map]
    R8 --> R9[nn_send ack]
    R3 -- Other --> R10[Ignore and continue loop]
```

Primary code path mapping:
- decode_wrp: parses msg_type, source, dest, transaction_uuid, payload.
- main loop: selects REQ thread path or ALIVE ack path.

### 6.4 Tool Resolution and Safety Workflow

```mermaid
flowchart TD
    T0[Worker starts handle_request] --> T1[decode_request_payload]
    T1 --> T2{tool present?}
    T2 -- No --> T3[Log missing tool and return]
    T2 -- Yes --> T4[catalog_lookup(tool)]
    T4 --> T5{catalog entry found?}
    T5 -- No --> T6[Set output: tool not in catalog]
    T5 -- Yes --> T7{command provided in request?}
    T7 -- No --> T8[Use catalog command]
    T7 -- Yes --> T9[Use request command]
    T8 --> T10[is_blocked(first token)]
    T9 --> T10
    T10 --> T11{blocked?}
    T11 -- Yes --> T12[Set output: command blocked or missing]
    T11 -- No --> T13[run_command]
```

Primary code path mapping:
- decode_request_payload: extracts tool and optional command.
- catalog_lookup: validates tool is declared.
- is_blocked: validates first token against blocked list.

### 6.5 Command Execution and Response Build Workflow

```mermaid
flowchart TD
    E0[run_command] --> E1[popen(cmd)]
    E1 --> E2[Read stdout chunks]
    E2 --> E3{MAX_OUTPUT_BYTES reached?}
    E3 -- Yes --> E4[Truncate at max bytes]
    E3 -- No --> E5[Continue until EOF]
    E4 --> E6[pclose and derive exit code]
    E5 --> E6
    E6 --> E7[build_response_payload]
    E7 --> E8[build_wrp_response]
    E8 --> E9[nn_send response]
    E9 --> E10[Free request and buffers]
```

Primary code path mapping:
- run_command: executes shell command and captures bounded stdout.
- build_response_payload: packs tool, exit_code, stdout.
- build_wrp_response: wraps inner payload into outer type-3 response.

### 6.6 Keepalive Interface Workflow (WRP Type 10)

```mermaid
sequenceDiagram
    participant P as Parodus
    participant M as Main Thread
    participant K as Keepalive Sub-Component

    P->>M: WRP msg_type=10
    M->>K: Enter keepalive branch
    K->>K: Pack map {msg_type:10}
    K->>M: Ack bytes
    M->>P: nn_send ack
    M->>M: Log keepalive ack sent
```

Primary code path mapping:
- main loop branch for WRP_MSG_TYPE_ALIVE.

### 6.7 Error Handling Sub-Workflow

```mermaid
flowchart TD
    X0[Interface operation] --> X1{Operation success?}
    X1 -- Yes --> X2[Continue normal flow]
    X1 -- No --> X3{Category}
    X3 -- Socket create/bind/connect/send --> X4[Log error, retry or exit]
    X3 -- Decode failures --> X5[Drop message and continue loop]
    X3 -- Missing tool/blocked command --> X6[Build non-success output response]
    X3 -- Allocation failure --> X7[Best-effort cleanup and return]
```

Primary code path mapping:
- main: retries connection and logs I/O errors.
- handle_request: emits controlled outputs for missing/blocked command conditions.
- decode helpers: tolerate malformed maps by not progressing to execution.

## 7. Device-Side Communication Workflow (Presentation View)

```mermaid
sequenceDiagram
    autonumber
    participant PAR as Parodus
    participant MAIN as Diag-Server Main Thread
    participant W as Worker Thread
    participant CAT as Catalog Manager
    participant SAFE as Safety Gate
    participant EXEC as Command Executor

    Note over MAIN,PAR: Startup: service registration
    MAIN->>PAR: WRP type=9 registration (service_name, url)

    loop Runtime message handling
        PAR->>MAIN: WRP frame on PULL socket
        alt msg_type=10 keepalive
            MAIN->>PAR: WRP type=10 keepalive ack
        else msg_type=3 request
            MAIN->>MAIN: Decode outer WRP (source, dest, uuid, payload)
            MAIN->>W: Dispatch request context
            W->>W: Decode inner payload (tool, optional command)
            W->>CAT: Lookup tool in catalog
            alt Tool found
                W->>SAFE: Validate command (blocked-token check)
                alt Command allowed
                    W->>EXEC: popen + capture stdout + exit code
                    EXEC-->>W: Command result
                else Command blocked
                    W->>W: Build blocked/missing output
                end
            else Tool missing
                W->>W: Build tool-not-in-catalog output
            end
            W->>W: Build response payload + outer WRP response
            W->>PAR: WRP type=3 response on PUSH socket
        else unsupported msg_type
            MAIN->>MAIN: Ignore and continue
        end
    end
```

Key messaging paths on the device:
- Inbound request path: Parodus -> Diag-Server PULL (127.0.0.1:6669).
- Outbound response path: Diag-Server PUSH -> Parodus (127.0.0.1:6666).
- Control path: keepalive ping/ack (WRP type 10) handled inline by main thread.
