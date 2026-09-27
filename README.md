# devops-sandbox

A C++20 secure job-execution daemon. It pops untrusted shell commands from a
Redis-backed queue, runs each one isolated in a Linux sandbox (PID/NET
namespaces, cgroup v2 resource limits, a BPF seccomp allowlist), and reports
exit code, duration, and streamed output back.

The sandbox host is Linux-only by design; on other platforms the API is a stub
that returns an explicit "not supported" error.

## How it isolates

Each command runs through `Sandbox::run(cmd, ...)`, which:

- **PID namespace** (`CLONE_NEWPID`) — the command sees only itself and its
  children.
- **Network namespace** (`CLONE_NEWNET`) unless `enable_network` is set — no
  sockets, no network.
- **cgroup v2** — created per job under `/sys/fs/cgroup/devops-<id>`:
  - `memory.max` (default 256 MB)
  - `cpu.max` (`cpu_quota_us` / `cpu_period_us`, default 100000/100000 = 1 core)
  - `pids.max` (default 64)
- **seccomp BPF allowlist** — default-action KILL, with an allowlist covering
  glibc startup + `/bin/sh`. The allowlist is architecture-tolerant: syscalls
  that don't exist on the target arch are skipped. Missing-but-needed syscalls
  kill the job with SIGSYS (see Known limitations).
- **stdout/stderr capture** — the child's stdout/stderr are dup2'd onto a pipe
  read back by the daemon; output is streamed via a callback and buffered in
  `ExecResult::out`.

The command is executed as `/bin/sh -c "<command>"`.

## Architecture

```
job producer        sandbox daemon                          per-job sandbox
(external)
     │  ──┐
     │    ▼ RedisClient      job queue
     ▼    ├──────────────►  poll job_next ──► claim ──► Sandbox::run(cmd)
          │
          ├──► PID namespace   (CLONE_NEWPID)
          ├──► NET namespace   (CLONE_NEWNET unless enabled)
          ├──► cgroup v2       memory.max / cpu.max / pids.max
          ├──► seccomp BPF     default KILL + allowlist
          └──► stdout/stderr   ──pipe──► streamed via callback

          ◄── job_log   (line)
          ◄── job_result (exit_code, duration_ms)
```

Job flow, as implemented in `devops/main.cpp`:

1. register as a sandbox worker (`sandbox register <id> <type> <addr>`)
2. poll for work (`job next`)
3. claim the job (`sandbox claim <id> <job id>`)
4. run it in a new sandbox, streaming output (`job log <id> <line>`)
5. report the result (`job result <id> <exit_code> <duration_ms>`)
6. release the sandbox (`sandbox release <id>`)

### Components

| Component            | Role                                                            |
| -------------------- | --------------------------------------------------------------- |
| `Sandbox`            | clone/cgroup/seccomp execution engine (`sandbox.cpp`)            |
| `RedisClient`        | Minimal RESP client + app-level command helpers                 |
| `MetricsServer`      | Prometheus text format on `/metrics`                            |
| `HealthMonitor`      | Periodic checks (redis, sandbox, resources)                     |
| `LogAggregator`      | Structured log sink                                              |
| `ProcessSupervisor`  | Spawn/restart managed processes, emits STARTED/CRASHED… events   |
| `ResourceManager`    | Per-sandbox usage tracking, over-limit alerts                   |

## Requirements

- Linux with **cgroup v2** mounted at `/sys/fs/cgroup`
- `CAP_SYS_ADMIN` (for `CLONE_NEWPID`/`CLONE_NEWNET`) and write access to the
  cgroup tree — run as root or with `docker run --privileged`
- libseccomp, CMake ≥ 3.20, a C++20 compiler
- macOS/Windows: the API compiles but `run()` returns an error stub

No Redis client library is needed: `RedisClient` speaks RESP over a plain
socket, which is why `libseccomp` is the only link-time dependency beyond libc,
libstdc++ and pthreads.

## Build & run

With Docker, nothing but Docker is required on the host:

```sh
docker build -t devops-sandbox .        # configures, builds and runs ctest
docker run --rm devops-sandbox --help
```

Bare metal:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
./build/sandbox --host 127.0.0.1 --port 1234 --id sandbox-1
```

| Flag            | Description                                  | Default        |
| --------------- | -------------------------------------------- | -------------- |
| `-h, --host`    | Redis host                                   | `127.0.0.1`    |
| `-p, --port`    | Redis port                                   | `1234`         |
| `-i, --id`      | Sandbox worker ID                            | `cpp-sandbox-1` |
| `-t, --type`    | Target type (feeds job routing / network)    | `local`        |
| `-m, --metrics-port` | Prometheus port                          | `9090`         |
| `-n, --no-seccomp`   | Disable the seccomp filter               | seccomp on     |
| `-r, --max-restarts` | Accepted; restarts are configured per process via `ProcessConfig` | `3` |
| `--help`        | Show usage                                   |                |

`--type network` sets `enable_network = true` for a job; any other type runs
with networking disabled.

## Client protocol

The daemon speaks app-level commands over its Redis connection. A Redis-side
shim (module/gateway) is expected to implement them:

| Command                            | Meaning                              |
| ---------------------------------- | ------------------------------------ |
| `job next`                         | Pop next job; reply is an array `[job_id, job_name, target, command, …]` (≥5 fields) or nil |
| `job status <id>`                  | Query job state                      |
| `job result <id> <exit> <ms>`      | Record completion                    |
| `job log <id> <line>`              | Append a log line (truncated to 1000 chars by the daemon) |
| `sandbox register <id> <type> <addr>` | Register as a worker             |
| `sandbox claim <id> <job id>`      | Claim a job                          |
| `sandbox release <id>`             | Free the worker                      |

Core RESP helpers (`SET`, `GET`, `DEL`, `LPUSH`, `RPOP`, `LRANGE`, `LLEN`) are
available on `RedisClient` directly.

## Configuration

`SandboxConfig` (see `devops/sandbox.hpp`):

| Field               | Default        | Meaning                                  |
| ------------------- | -------------- | ---------------------------------------- |
| `id`                | —              | cgroup path suffix / worker identity     |
| `memory_limit_bytes`| 256 MB         | cgroup v2 `memory.max`                   |
| `cpu_quota_us`      | 100000         | cgroup v2 `cpu.max` quota                |
| `cpu_period_us`     | 100000         | cgroup v2 `cpu.max` period (1 core)      |
| `pids_limit`        | 64             | cgroup v2 `pids.max`                     |
| `enable_network`    | `false`        | join the network namespace (`CLONE_NEWNET` skipped) |
| `enable_seccomp`    | `true`         | install the BPF allowlist                |

## Tests

Everything is driven from `compose.yaml` at the repo root. Compile-time checks
are Docker build stages, so a green build *is* a green test — there is no
separate test command to forget to run:

```sh
docker compose build                          # native build + ctest
docker build --target cross-win -t x .        # mingw-w64: PE stub path compiles
docker build --target cross-mac -t x .        # Zig + macOS SDK: Mach-O stub path compiles
docker compose run --rm test                  # functional: real namespaces/cgroups/seccomp
docker compose --profile matrix build         # both cross-compile checks
```

`test` is the only service that needs privilege: `clone(CLONE_NEWPID)`,
`clone(CLONE_NEWNET)` and cgroup v2 writes cannot be asserted at build time, so
that one check has to run. It builds the `native` stage, so it re-runs `ctest`
before the functional smoke test.

Because the test script is baked into the image rather than bind-mounted, pass
`--build` after editing it: `docker compose run --rm --build test`. A plain
`docker compose run` reuses whatever image is already tagged, which will happily
run a stale script.

Deliberately **not** done: the daemon image does not bind-mount the source tree.
A build from copied sources is a real result; a bind mount over a stale host
`build/` directory is not. Mount the tree only when you want an edit-build loop.

## Repository layout

Two source folders, both at the root:

```
CMakeLists.txt          the project root is the include dir, so <devops/x.hpp> resolves
Dockerfile              build stages: native, cross-win, cross-mac, runtime
compose.yaml            the runtime, the functional test, the matrix
devops/                 the daemon and the sandbox engine; each component's .hpp
                        sits next to its .cpp
tests/                  unit tests, the cross-compile fixture, the functional test
```

`include/` and `src/` are gone: at this size, separating them only meant
`include/devops/` and `src/` for files that always travel in pairs, and the
`devops/` name is what the include path already keys off.

## Known limitations

- **Static seccomp allowlist** — commands that syscall things outside the
  allowlist are killed (SIGSYS). Next step: fuzz the allowlist to expand it
  safely for real workloads.
- **No wall-clock timeout** — a hung sandboxed command blocks the poll loop.
- **cgroup cleanup** — per-job cgroups are removed best-effort on `cleanup()`.
- **No filesystem isolation** — the command runs in the daemon's rootfs/native
  mount namespace; `SandboxConfig::rootfs` is currently unused. This is the
  largest remaining gap: a job can read anything the daemon can read.