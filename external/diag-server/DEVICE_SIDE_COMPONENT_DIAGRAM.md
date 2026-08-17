# Device Side Communication - Component Diagram

## Component Architecture (Device Side)

```mermaid
graph TB
    subgraph ExtSys["External"]
        OPS["Cloud OPS Gateway"]
    end

    subgraph Device["Device Side"]
        subgraph Network["Network Layer"]
            Parodus["Parodus<br/>(TCP 127.0.0.1:6666)"]
        end

        subgraph DiagServer["Diag-Server Process"]
            subgraph MainThread["Main Thread"]
                Lifecycle["Lifecycle Manager<br/>(Startup & Registration)"]
                RecvLoop["Receive Loop<br/>(PULL Socket Listener)"]
                KAHandler["Keepalive Handler<br/>(msg_type=10)"]
            end

            subgraph RequestPath["Request Processing Path"]
                WRPDecoder["WRP Decoder<br/>(Outer msgpack map)"]
                PayloadDecoder["Payload Decoder<br/>(tool, command)"]
                CatalogMgr["Catalog Manager<br/>(Lookup allowed tools)"]
                SafetyGate["Safety Gate<br/>(Blocked-token list)"]
                ThreadDispatch["Thread Dispatcher<br/>(Detach worker per request)"]
            end

            subgraph WorkerThread["Worker Thread Pool"]
                CmdExecutor["Command Executor<br/>(popen + capture stdout)"]
                ResponseBuilder["Response Builder<br/>(Build inner payload)"]
                WRPBuilder["WRP Response Builder<br/>(Build outer WRP)"]
            end

            Data["📄 Catalog JSON<br/>(tools & defaults)"]
        end
    end

    %% External flow
    OPS -->|"WRP Request<br/>msg_type=3"| Parodus

    %% Main thread registration
    Lifecycle -->|"Registration<br/>msg_type=9"| Parodus

    %% Request receive path
    Parodus -->|"PULL socket"| RecvLoop
    RecvLoop -->|"Route by type"| KAHandler
    RecvLoop -->|"Route by type"| WRPDecoder

    %% Decode and validate
    WRPDecoder --> PayloadDecoder
    PayloadDecoder --> CatalogMgr
    CatalogMgr --> Data
    CatalogMgr --> SafetyGate

    %% Worker dispatch
    SafetyGate --> ThreadDispatch
    ThreadDispatch --> CmdExecutor

    %% Command execution and response
    CmdExecutor --> ResponseBuilder
    ResponseBuilder --> WRPBuilder

    %% Response back to Parodus
    WRPBuilder -->|"PUSH socket"| Parodus
    Parodus -->|"WRP Response<br/>msg_type=3"| OPS

    %% Keepalive ack
    KAHandler -->|"Ack msg_type=10"| Parodus

    style MainThread fill:#e1f5ff
    style RequestPath fill:#fff3e0
    style WorkerThread fill:#f3e5f5
    style Network fill:#e8f5e9
    style Data fill:#fce4ec
```

## Data Flow Sequence (Device Side)

```mermaid
sequenceDiagram
    participant OPS as OPS<br/>Gateway
    participant Paro as Parodus
    participant Main as Main<br/>Thread
    participant Worker as Worker<br/>Thread
    participant Catalog as Catalog
    participant Exec as Executor

    rect rgb(200, 220, 255)
        note over OPS,Exec: Initialization Phase
        Main->>Paro: Register (msg_type=9)
        Paro->>OPS: Ack registration
    end

    rect rgb(255, 240, 200)
        note over OPS,Exec: Request Processing Phase
        OPS->>Paro: WRP Request (msg_type=3)
        Paro->>Main: PULL socket delivery
        Main->>Main: Decode WRP outer map
        Main->>Main: Decode payload (tool, command)
        Main->>Worker: Dispatch detached thread
    end

    rect rgb(243, 229, 245)
        note over Worker,Catalog: Validation & Execution Phase
        Worker->>Catalog: Lookup tool
        alt Tool exists
            Worker->>Worker: Apply command override if provided
            Worker->>Worker: Safety check (blocked-token list)
            alt Command allowed
                Worker->>Exec: Execute command
                Exec-->>Worker: stdout + exit_code
            else Command blocked
                Worker->>Worker: Build blocked error result
            end
        else Tool not found
            Worker->>Worker: Build tool-not-found result
        end
    end

    rect rgb(232, 245, 233)
        note over Worker,Paro: Response Building & Send Phase
        Worker->>Worker: Build inner payload<br/>(tool, exit_code, stdout)
        Worker->>Worker: Build outer WRP<br/>(msg_type=3, sender, recipient)
        Worker->>Paro: PUSH socket send
        Paro->>OPS: Forward response
    end

    rect rgb(224, 242, 254)
        note over Main,Paro: Keepalive Monitoring Phase
        Paro->>Main: Keepalive ping (msg_type=10)
        Main->>Main: Handle keepalive
        Main->>Paro: Ack keepalive (msg_type=10)
    end
```

## Component Responsibilities

| Component | Responsibility | Technology |
|-----------|-----------------|------------|
| **Parodus** | Messaging gateway between device & cloud | nanomsg TCP |
| **Main Thread** | Initialization, socket management, receive loop coordination | POSIX threads |
| **WRP Decoder** | Parse outer msgpack WRP message envelope | msgpack-c |
| **Payload Decoder** | Extract tool & command from request payload | msgpack-c, cJSON |
| **Catalog Manager** | Load & lookup allowed tools from JSON | cJSON |
| **Safety Gate** | Validate commands against blocked-token list | String matching |
| **Thread Dispatcher** | Create detached worker thread per request | pthread_create |
| **Command Executor** | Run shell command and capture output | popen/pclose |
| **Response Builder** | Serialize result into msgpack payload | msgpack-c |
| **Keepalive Handler** | Monitor & respond to keepalive pings | nanomsg |

## Key Design Patterns

1. **Asynchronous Request Handling**: Main thread stays responsive by delegating execution to detached worker threads
2. **Layered Validation**: Request validation happens in stages (catalog → safety gate) before execution
3. **Message Envelope Pattern**: WRP outer envelope + msgpack inner payload separation
4. **Resource Safety**: Command output capped at 64KB, command execution timeout at 10s
5. **Graceful Degradation**: Missing tools/blocked commands return error results instead of crashes

## Message Types Used

- **msg_type=3**: Request/Response (bidirectional)
- **msg_type=9**: Service Registration (device → cloud)
- **msg_type=10**: Keepalive ping/ack (bidirectional)

## Socket Configuration

| Socket | Type | URL | Purpose |
|--------|------|-----|---------|
| PUSH | nanomsg | 127.0.0.1:6666 | Send responses & register to Parodus |
| PULL | nanomsg | 127.0.0.1:6669 | Receive requests from Parodus |
