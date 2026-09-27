#!/usr/bin/env bash
# Functional test for the sandbox on Linux. Runs inside the privileged
# `test` service: `docker compose run --rm test`.
# 1. Builds the full project (unit tests + sandbox executable) with CMake.
# 2. Runs ctest.
# 3. Compiles & runs a smoke test that exercises the real Sandbox API
#    (CLONE_NEWPID, CLONE_NEWNET, cgroups v2, seccomp, pipes).
set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"

echo "== building project (cmake/ninja) =="
cmake -S "$root" -B "$root/build" -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build "$root/build" --parallel

echo "== ctest =="
ctest --test-dir "$root/build" --output-on-failure

echo "== sandbox_smoke (real namespaces/cgroup/seccomp) =="
g++ -std=c++20 -I "$root/include" "$root/tests/smoke.cpp" \
    "$root/src/sandbox.cpp" -o /tmp/sandbox_smoke -lseccomp -pthread
/tmp/sandbox_smoke

echo "== built sandbox executable =="
"$root/build/sandbox" --help 2>&1 | head -5 || true

echo "ALL LINUX TESTS DONE"