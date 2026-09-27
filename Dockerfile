# Linux Process Sandbox — build, test and run image.
#
#   docker build -t devops-sandbox .              # native build + ctest
#   docker build --target cross-win -t x .        # mingw-w64 cross-compile check
#   docker build --target cross-mac -t x .        # Zig/macOS cross-compile check
#   docker compose run --rm test                  # functional test (needs --privileged)
#   docker run --rm devops-sandbox --help
#
# Each check that can be expressed as a compile is a build stage, so a green
# `docker build --target <stage>` *is* the test result. Only the checks that need
# namespaces, cgroups and seccomp at runtime live in compose, under `test`.

# -----------------------------------------------------------------------------
FROM ubuntu:24.04 AS base
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
        ca-certificates curl file xz-utils \
    && rm -rf /var/lib/apt/lists/*

# -----------------------------------------------------------------------------
# Native toolchain: builds the daemon and its unit tests.
FROM base AS native
RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential cmake ninja-build libseccomp-dev \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY CMakeLists.txt ./
COPY devops/ ./devops/
COPY tests/ ./tests/
# Tests run as part of the build: a stage that does not compile, or whose ctest
# fails, never produces an image.
RUN cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
    && cmake --build build --parallel \
    && ctest --test-dir build --output-on-failure

# -----------------------------------------------------------------------------
# Windows cross-compile check. sandbox.cpp has a non-Linux stub path; this proves
# it still compiles for a foreign target. mingw-w64's posix threading model is
# required for std::thread/std::format to behave.
FROM base AS cross-win
RUN apt-get update && apt-get install -y --no-install-recommends \
        g++-mingw-w64-x86-64 \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY devops/sandbox.hpp devops/result.hpp ./devops/
COPY devops/sandbox.cpp ./devops/sandbox.cpp
COPY tests/cross_stub_main.cpp ./cross_stub_main.cpp
RUN x86_64-w64-mingw32-g++-posix -std=c++20 -O2 -I . \
        cross_stub_main.cpp devops/sandbox.cpp -static -o sandbox.exe \
    && file sandbox.exe

# -----------------------------------------------------------------------------
# macOS cross-compile check, with no Apple hardware and no Xcode licence: Zig
# ships the macOS SDK and a C++20 libc++. This is what keeps the __APPLE__
# branches of sandbox.cpp honest.
FROM base AS cross-mac
ARG ZIG_VERSION=0.14.1
RUN set -eux; \
    case "$(dpkg --print-architecture)" in \
        amd64) ZIG_TARBALL=zig-x86_64-linux-${ZIG_VERSION}.tar.xz ;; \
        arm64) ZIG_TARBALL=zig-aarch64-linux-${ZIG_VERSION}.tar.xz ;; \
        *) echo "unsupported arch: $(dpkg --print-architecture)"; exit 1 ;; \
    esac; \
    curl -fsSL "https://ziglang.org/download/${ZIG_VERSION}/${ZIG_TARBALL}" -o /tmp/zig.tar.xz; \
    mkdir -p /opt/zig; \
    tar -xJf /tmp/zig.tar.xz -C /opt/zig --strip-components=1; \
    rm /tmp/zig.tar.xz; \
    ln -s /opt/zig/zig /usr/local/bin/zig
WORKDIR /src
COPY devops/sandbox.hpp devops/result.hpp ./devops/
COPY devops/sandbox.cpp ./devops/sandbox.cpp
COPY tests/cross_stub_main.cpp ./cross_stub_main.cpp
RUN zig c++ -target x86_64-macos -O2 -std=c++20 -I . -c cross_stub_main.cpp \
    && zig c++ -target x86_64-macos -O2 -std=c++20 -I . -c devops/sandbox.cpp \
    && zig c++ -target x86_64-macos -O2 -std=c++20 cross_stub_main.o sandbox.o -o sandbox-mac \
    && file sandbox-mac

# -----------------------------------------------------------------------------
# The daemon. Runtime dependencies only: libseccomp2 plus the C++ runtime that
# ubuntu:24.04 already provides. No compiler, no CMake, no sources.
FROM ubuntu:24.04 AS runtime
LABEL org.opencontainers.image.title="devops-sandbox" \
      org.opencontainers.image.description="Linux process sandbox: namespaces, cgroups v2, seccomp-BPF" \
      org.opencontainers.image.source="https://github.com/itsVinM/redisops"

RUN apt-get update && apt-get install -y --no-install-recommends libseccomp2 \
    && rm -rf /var/lib/apt/lists/* \
    && useradd --create-home --shell /usr/sbin/nologin sandbox
COPY --from=native /src/build/sandbox /usr/local/bin/sandbox
USER sandbox
# The daemon polls Redis for jobs, so it needs a reachable endpoint; the default
# is the compose service name. Runs unprivileged by default — the privileges in
# question belong to the *child* processes it isolates, which is what
# `compose.test` exercises.
ENTRYPOINT ["/usr/local/bin/sandbox"]
CMD ["--help"]
