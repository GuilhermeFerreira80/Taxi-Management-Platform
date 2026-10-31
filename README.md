# Autonomous Taxi Fleet Management Platform

Systems programming project developed for the **Operating Systems** course in the Informatics Engineering BSc curriculum at **ISEC (Instituto Superior de Engenharia de Coimbra)**.

### Overview
A multi-process, multithreaded distributed system implemented in C for UNIX/Linux environments to manage and orchestrate an autonomous taxi service. The solution emphasizes core Operating Systems concepts, including Inter-Process Communication (IPC), asynchronous event handling, and low-level resource synchronization.

### System Architecture
The platform is composed of three interconnected modules:
- **Controller (`controlador.c`):** Central coordinator running a multithreaded core that manages dispatching, enforces fleet thresholds via `NVEICULOS`, maintains service states, and hosts an admin console.
- **Client (`cliente.c`):** CLI client providing commands to register, schedule rides, inspect operational states, and cancel reservations.
- **Vehicle (`veiculo.c`):** Ephemeral worker process spawned dynamically per ride to simulate route progress and stream telemetry data.

### IPC & Concurrency Highlights
- **Named Pipes (FIFOs):**
  - Central non-blocking inbound channel (`fifo_controlador`) using `O_NONBLOCK` for dispatch registration and request queuing.
  - Dedicated per-client reply pipes (`/tmp/fifo_client_<PID>`) ensuring private delivery of server notifications and driver arrival alerts.
- **Anonymous Pipes & I/O Redirection:** Workers are created using `fork()` and `execl()`, redirecting their standard output (`stdout`) to an anonymous pipe via `dup2()`, ingested upstream by dedicated telemetry threads.
- **I/O Multiplexing:** Client CLI processes leverage `select()` to synchronously monitor both user keystrokes (`stdin`) and inbound messages on the local FIFO without blocking the main event loop.
- **POSIX Signal Control:**
  - `SIGINT`: Intercepted for clean, graceful system teardown and unlinking of filesystem primitives.
  - `SIGTERM`: Dispatched by the controller to terminate active client instances upon platform shutdown.
  - `SIGUSR1`: Asynchronous cancellation interrupt routed directly to moving vehicles when a customer or admin cancels an in-flight booking.
- **Synchronization & Thread Safety:** Coordinated through POSIX threads (`pthread`) alongside dedicated mutexes (`pthread_mutex_t`) protecting shared memory tables, connected user state, and aggregate route metrics.
- **Non-blocking Reaper:** Prevention of zombie processes through periodic non-blocking status harvesting using `waitpid(-1, &status, WNOHANG)`.
