# SimGrid Queueing Simulation (practica_simulacion_2025)

This project simulates a queueing system using SimGrid actors and a cluster platform. It models an M/M/c-style system (exponential arrivals and services, c servers) with configurable dispatching and queue scheduling policies, collects end-to-end statistics, and includes a batch runner to sweep parameters and save results.

## Platform overview

- Defined in `platform-cluster.xml` under one AS with full routing
- Three clusters (hostnames are generated from prefix+radical):
  - Clients: `c-0..c-1`, speed 4 Gf
  - Dispatchers: `d-0..d-4`, speed 2 Gf
  - Servers: `s-0..s-1000`, speed 1 Gf
- High-capacity, 0us-latency backbone links connect client→dispatcher and dispatcher→servers
- The simulation currently instantiates 100 servers: `s-0..s-99`

## Actors and flow

- client: generates tasks with exponential inter-arrival times (rate λ). Each task has exponential service requirement (rate μ=1). Sends tasks to dispatcher mailbox `d-0`.
- dispatcher: receives tasks and forwards them to server mailboxes `s-<i>` according to a dispatch policy.
- server: enqueues incoming tasks in a per-server queue, updates counters, and signals the worker.
- dispatcherServer: per-server worker that pops from the queue and executes CPU work with `sg_actor_execute(req->t_service)`.

A request’s measured system time equals completion time minus arrival time; execution time equals requested flops divided by server CPU speed (1 Gf), so an exponential draw with mean 1 second maps directly to ~1s execution time.

## Dispatching policies: design, code, and CLI

Dispatcher chooses the destination server for each incoming task by consulting the current per-server workload metric `Nsystem[i]` (number in system = waiting + running). This counter is updated atomically on each server when a request arrives/leaves, and read by the dispatcher without extra locking (eventual consistency is fine for policy decisions).

Where in the code
- Function: `dispatcher()` in `practica.c`
- Mechanism: a `switch (DISPATCH_POLICY)` selects the strategy and computes an index `s` in `[0..NUM_SERVERS-1]`.
- Workload metric: `Nsystem[i]`

Policies implemented
- random
  - Uniformly picks a server: `s = uniform_int(0, NUM_SERVERS-1)`; O(1)
  - Simple, no load awareness.
- rr (round-robin)
  - Cycles deterministically: `s = rr_next_server; rr_next_server = (rr_next_server+1)%NUM_SERVERS`; O(1)
  - Fair in turns, not load-aware.
- sqf (smallest-queue-first)
  - Scans `Nsystem[]` and picks the minimum; O(NUM_SERVERS)
  - Direct load-aware choice; slightly higher selection cost.
- two-random-choices (aka power-of-two choices)
  - Draw two distinct servers uniformly at random: `a`, `b`.
  - Pick the one with smaller `Nsystem` (ties broken by first); expected O(1).
  - Near-SQF performance with constant-time selection.
- two-rr-random-choices
  - One candidate from RR (`a`), one from random (`b != a`), choose the smaller `Nsystem`.
  - Combines fairness of RR with load awareness of a second probe.

CLI mapping
- `random` → random
- `rr` → round-robin
- `sqf` → smallest-queue-first
- `two-random-choices` (alias `two-random`)
- `two-rr-random-choices` (alias `two-rr-random`)

Concurrency notes
- `Nsystem[i]` increments when `server()` enqueues a request and decrements after `dispatcherServer()` finishes `sg_actor_execute()`; the dispatcher reads these counters asynchronously, so instantaneous readings can be slightly stale. This is acceptable and typical for load-balancing heuristics.

## Queue policies: design, code, and CLI

Each server maintains a local queue `client_requests[i]` (SimGrid `xbt_dynar_t`). Requests are pushed on arrival and the worker pops from the head (`xbt_dynar_shift`). Scheduling is non-preemptive: once a job starts executing, it runs to completion.

Where in the code
- Enqueue: `server()` pushes into `client_requests[my_s]` and updates counters; then, if a non-FCFS policy is selected, it re-sorts the vector.
- Dequeue/execute: `dispatcherServer()` waits on the condition variable, pops from the head, then calls `sg_actor_execute(req->t_service)`.
- Comparators:
  - `sort_function` (ascending by `t_service`) → SJF
  - `sort_function_desc` (descending by `t_service`) → LJF

Policies implemented
- fcfs (First-Come First-Served)
  - Push at tail, pop from head; no sorting; FIFO semantics; O(1) enqueue/dequeue.
- sjf (Shortest Job First)
  - After push, sort ascending by `t_service` using `sort_function`; favors short jobs; O(n log n) per enqueue.
- ljf (Longest Job First)
  - After push, sort descending by `t_service` using `sort_function_desc`; favors long jobs; O(n log n) per enqueue.

Notes
- Non-preemptive SJF/LJF: the policy affects only waiting order. Running jobs are not interrupted.
- `t_service` is in FLOPs; with server speed 1 Gf, simulated runtime (seconds) equals the sampled exponential time.
- For large queues, a binary heap or indexed priority queue would reduce reorder cost; the current vector+sort is simple and sufficient for this project.

## Key parameters

- `NUM_SERVERS = 100` (fixed)
- `NUM_CLIENTS = 1`, `NUM_DISPATCHERS = 1`, `NUM_TASKS = 100000`
- `SERVICE_RATE μ = 1.0`
- `ARRIVAL_RATE λ_total = lambda_arg * NUM_SERVERS` (the CLI lambda is per-server rate)

Keep λ per server below μ for stability (ρ < 1). With λ=0.7 and μ=1.0, ρ=0.7.

## Build and run

Prerequisites: SimGrid installed at `$(HOME)/master/distributed_parallel_systems/simgrid/install_dir` (as set in the Makefile).

Build:

```bash
make -j
```

Run:

```bash
./practica platform-cluster.xml <lambda> [dispatcher_policy] [queue_policy]
```

- Dispatcher policies:
  - `random`: choose a random server
  - `sqf`: choose the server with smallest Nsystem (queue + running)
  - `rr`: round-robin
  - `two-random-choices` (alias `two-random`): pick two random servers, send to the less loaded
  - `two-rr-random-choices` (alias `two-rr-random`): pick one by RR and one random, send to the less loaded
- Queue policies:
  - `fcfs`: First-Come First-Served (FIFO)
  - `sjf`: Shortest Job First (ascending service time)
  - `ljf`: Longest Job First (descending service time)

Examples:

```bash
# Random dispatch, FCFS queue, per-server lambda=0.7
./practica platform-cluster.xml 0.7 random fcfs

# Two random choices dispatch, SJF queue
./practica platform-cluster.xml 0.7 two-random-choices sjf

# Two RR+random choices dispatch, LJF queue
./practica platform-cluster.xml 0.7 two-rr-random-choices ljf
```

Program output (final line):

```
tiempoMedioServicio  TamañoMediocola  TareasMediasEnElSistema  tareas
<mean_system_time>   <mean_queue>     <mean_in_system>         <num_tasks>
```

## Batch experiments

Use the provided script to sweep multiple parameter combinations and store results:

```bash
./run_experiments.sh
```

- Generates: `results/results.csv`
- Columns: `lambda,dispatch,queue,mean_system_time,mean_queue_size,mean_in_system,tasks`
- Sweeps λ in `{0.2, 0.5, 0.7, 0.9}`, all dispatchers `{random, sqf, rr, two-random-choices, two-rr-random-choices}` and queues `{fcfs, sjf, ljf}`.

## File map

- `practica.c`: all actor logic, CLI parsing, statistics, policies
- `rand.c` / `rand.h`: RNG helpers (`exponential()`, `uniform_int()`, `seed()`)
- `platform-cluster.xml`: simulated platform (hosts, links, routes)
- `Makefile`: build configuration (honors `INSTALL_PATH`)
- `run_experiments.sh`: batch runner, writes `results/results.csv`

## Notes and tips

- Stability: choose λ per server < μ to avoid divergence.
- Performance: 100,000 tasks can be slow; if needed, we can add a CLI flag to set NUM_TASKS for faster iterations.
- Reproducibility: the RNG is seeded with current time; adding a fixed-seed CLI is straightforward if desired.
- Debugging: both `server` and `dispatcherServer` actors run on each `s-<i>` host; logging can be added if needed via SimGrid’s facilities.
