# devops-sandbox

A C++20 job-execution daemon. It pulls untrusted shell commands off a
Redis-backed queue, runs each one isolated in a Linux sandbox, and reports back
exit code, duration and streamed output.

Linux-only host. On macOS and Windows the `Sandbox` API compiles but `run()`
returns an explicit "not supported" error.

## How it isolates

Every command goes through `Sandbox::run(cmd, ...)`, executed as
`/bin/sh -c "<command>"`:

- **PID namespace** (`CLONE_NEWPID`) — the command sees only itself and its children.
- **Network namespace** (`CLONE_NEWNET`) unless `enable_network` is set.
- **cgroup v2** — one group per job at `/sys/fs/cgroup/devops-<id>`, capped on
  memory, CPU and process count (see [Configuration](#configuration)).
- **seccomp BPF allowlist** — default-action `KILL`, allowing only glibc startup
  and `/bin/sh`. Syscalls that don't exist on the target arch are skipped, so one
  allowlist works everywhere; a needed syscall that is missing kills the job with
  `SIGSYS`.
- **stdout/stderr** — `dup2`'d onto a pipe, streamed back through a callback and
  buffered in `ExecResult::out`.

## Job flow

As implemented in `src/main.cpp`:

1. register as a worker — `sandbox register <id> <type> <addr>`
2. poll for work — `job next`
3. claim the job — `sandbox claim <id> <job id>`
4. run it in a new sandbox, streaming output — `job log <id> <line>`
5. report the result — `job result <id> <exit_code> <duration_ms>`
6. release the sandbox — `sandbox release <id>`

## Components

| Component           | Role                                                          |
| ------------------- | ------------------------------------------------------------- |
| `Sandbox`           | clone/cgroup/seccomp execution engine                         |
| `RedisClient`       | minimal RESP client + command helpers                         |
| `MetricsServer`     | Prometheus text on `/metrics`                                 |
| `HealthMonitor`     | periodic checks (redis, sandbox, resources)                   |
| `LogAggregator`     | structured log sink                                            |
| `ProcessSupervisor` | spawns/restarts processes, emits STARTED/CRASHED…             |
| `ResourceManager`   | per-sandbox usage tracking, over-limit alerts                 |

## Requirements

- Linux with **cgroup v2** mounted at `/sys/fs/cgroup`
- `CAP_SYS_ADMIN` (for the namespaces) and write access to the cgroup tree — run
  as root or with `--privileged`
- libseccomp, CMake ≥ 3.20, a C++20 compiler

No Redis library needed: `RedisClient` speaks RESP over a plain socket, so
libseccomp is the only link-time dependency beyond libc, libstdc++ and pthreads.

## Build & run

```sh
docker build -t devops-sandbox .      # shippable image; ctest runs in the native stage it depends on
docker run --rm devops-sandbox --help
```

The last stage is `runtime`, so that is what a bare `docker build` tags. It
copies the binary out of `native`, so ctest has already run by then and a failing
test fails the build.

Bare metal:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
./build/sandbox --host 127.0.0.1 --port 1234 --id sandbox-1
```

| Flag                  | Description                                    | Default         |
| --------------------- | ---------------------------------------------- | --------------- |
| `-h, --host`          | Redis host                                     | `127.0.0.1`     |
| `-p, --port`          | Redis port                                     | `1234`          |
| `-i, --id`            | Sandbox worker ID                              | `cpp-sandbox-1` |
| `-t, --type`          | Target type; `network` sets `enable_network`   | `local`         |
| `-m, --metrics-port`  | Prometheus port                                | `9090`          |
| `-n, --no-seccomp`    | Disable the seccomp filter                    | off             |
| `-r, --max-restarts`  | Accepted; restarts come from `ProcessConfig`  | `3`             |
| `--help`              | Show usage                                     |                 |

## Configuration

`SandboxConfig`, in `include/sandbox.hpp`:

| Field                | Default | Meaning                                            |
| -------------------- | ------- | -------------------------------------------------- |
| `id`                 | —       | cgroup path suffix / worker identity               |
| `memory_limit_bytes` | 256 MB  | cgroup v2 `memory.max`                             |
| `cpu_quota_us`       | 100000  | cgroup v2 `cpu.max` quota                          |
| `cpu_period_us`      | 100000  | cgroup v2 `cpu.max` period (1 core)                |
| `pids_limit`         | 64      | cgroup v2 `pids.max`                               |
| `enable_network`     | `false` | skip `CLONE_NEWNET`, i.e. join the host network    |
| `enable_seccomp`     | `true`  | install the BPF allowlist                          |

## Client protocol

The daemon talks over its Redis connection; a Redis-side shim is expected to
implement these:

| Command                              | Meaning                                                          |
| ------------------------------------ | ---------------------------------------------------------------- |
| `job next`                           | Pop next job: `[job_id, job_name, target, command, …]` (≥5 fields) or nil |
| `job status <id>`                    | Query job state                                                  |
| `job result <id> <exit> <ms>`        | Record completion                                                |
| `job log <id> <line>`                | Append a log line (truncated to 1000 chars)                      |
| `sandbox register <id> <type> <addr>`| Register as a worker                                             |
| `sandbox claim <id> <job id>`        | Claim a job                                                      |
| `sandbox release <id>`               | Free the worker                                                  |

`RedisClient` also exposes the core RESP helpers directly: `SET`, `GET`, `DEL`,
`LPUSH`, `RPOP`, `LRANGE`, `LLEN`.

## Tests

Compile-time checks are Docker build stages, so a green build is a green test:

```sh
docker compose build                    # native build + ctest
docker compose run --rm test            # functional: real namespaces/cgroup/seccomp
docker compose --profile matrix build   # both cross-compile checks
```

`test` is the only privileged service — the namespaces and cgroup writes can't
be asserted at build time, so that one check has to run. The script is baked
into the image rather than bind-mounted, so pass `--build` after editing it:
`docker compose run --rm --build test`.

## Layout

```
CMakeLists.txt   include/ is the include dir, so headers include as <x.hpp>
Dockerfile       stages: native, cross-win, cross-mac, runtime
compose.yaml     the runtime, the functional test, the matrix
include/         the nine public headers
src/             the daemon, the sandbox engine, the sidecars
tests/           unit tests, cross-compile fixture, functional test
```

## Known limitations

- **No filesystem isolation** — the biggest gap. The command runs in the
  daemon's mount namespace and `SandboxConfig::rootfs` is unused, so a job can
  read anything the daemon can read.
- **Static seccomp allowlist** — anything outside it dies with `SIGSYS`.
- **No wall-clock timeout** — a hung command blocks the poll loop.
- **cgroup cleanup** is best-effort on `cleanup()`.
