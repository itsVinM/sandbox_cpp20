#include <devops/sandbox.hpp>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <format>

#ifdef __linux__
#include <sched.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <seccomp.h>
#include <linux/seccomp.h>
#endif

namespace devops {

namespace {
constexpr size_t kStackSize = 8 * 1024;
}

Sandbox::Sandbox(SandboxConfig config) : config_(std::move(config)) {}

Sandbox::~Sandbox() { cleanup(); }

void Sandbox::cleanup() {
#ifdef __linux__
    if (cgroup_path_.size() > 1) {
        rmdir(("/sys/fs/cgroup" + cgroup_path_).c_str());
    }
    for (int& fd : pipe_fd_) {
        if (fd >= 0) close(fd);
        fd = -1;
    }
#endif
}

#ifdef __linux__

// Data passed to the child; only pointers, no copies.
struct ChildArgs {
    const SandboxConfig* config;
    const std::string* command;
    const std::string* cgroup_path;
    int pipe_fd;
};

static int write_file(const std::string& path, const char* fmt, auto... args) {
    FILE* f = fopen(path.c_str(), "w");
    if (!f) return -1;
    int n = fprintf(f, fmt, args...);
    fclose(f);
    return n;
}

// Allow single rule, skipping syscalls that don't exist on the target arch
// (e.g. arch_prctl is x86_64-only, faccessat2 is newer kernels), so the same
// filter compiles and runs everywhere.
static void allow_syscall(scmp_filter_ctx ctx, const char* name) {
    int sys = seccomp_syscall_resolve_name(name);
    if (sys != __NR_SCMP_ERROR) {
        seccomp_rule_add(ctx, SCMP_ACT_ALLOW, (int)sys, 0);
    }
}

// Syscalls needed by the child (glibc startup + /bin/sh) before it execs a
// command. The child runs without network; socket syscalls stay blocked.
static void install_allowlist(scmp_filter_ctx ctx) {
    for (const char* name : {
            "read", "write", "close", "openat", "lseek", "poll", "ppoll",
            "select", "pselect6", "pipe", "pipe2",
            "mmap", "munmap", "mprotect", "mremap", "brk",
            "execve", "exit", "exit_group",
            "getpid", "getppid", "getuid", "geteuid", "getgid", "getegid",
            "getrandom", "futex", "rseq", "set_robust_list", "set_tid_address",
            "fstat", "newfstatat", "statfs", "fstatfs",
            "faccessat", "faccessat2", "readlinkat", "getcwd", "chdir", "uname",
            "getdents64", "ioctl", "arch_prctl", "prctl",
            "rt_sigaction", "rt_sigprocmask", "sigaltstack",
            "clock_gettime", "nanosleep",
            "clone", "wait4", "waitid", "fcntl", "dup2", "dup3",
            "prlimit64", "getrlimit", "setrlimit", "getrusage"}) {
        allow_syscall(ctx, name);
    }
}

// Child entry point, runs in a fresh PID namespace on its own stack.
static int child_entry(void* arg) {
    auto* args = static_cast<ChildArgs*>(arg);
    const SandboxConfig& cfg = *args->config;

    std::string cg_base = "/sys/fs/cgroup" + *args->cgroup_path;
    mkdir(cg_base.c_str(), 0755);

    write_file(cg_base + "/memory.max", "%llu", (unsigned long long)cfg.memory_limit_bytes);
    write_file(cg_base + "/cpu.max", "%u %u", cfg.cpu_quota_us, cfg.cpu_period_us);
    write_file(cg_base + "/pids.max", "%u", cfg.pids_limit);
    write_file(cg_base + "/cgroup.procs", "%d", getpid());

    if (cfg.enable_seccomp) {
        scmp_filter_ctx ctx = seccomp_init(SCMP_ACT_KILL);
        if (ctx) {
            install_allowlist(ctx);
            seccomp_load(ctx);
            seccomp_release(ctx);
        }
    }

    dup2(args->pipe_fd, STDOUT_FILENO);
    dup2(args->pipe_fd, STDERR_FILENO);
    close(args->pipe_fd);

    execl("/bin/sh", "sh", "-c", args->command->c_str(), nullptr);
    _exit(127);
}

ExecResult Sandbox::run(const std::string& command, const std::vector<std::string>&,
                        std::function<void(const std::string&)> output_fn) {
    ExecResult result{};

    cgroup_path_ = "/devops-" + config_.id;

    if (pipe(pipe_fd_) != 0) {
        result.exit_code = -1;
        result.err = "pipe() failed";
        return result;
    }

    ChildArgs child_args{&config_, &command, &cgroup_path_, pipe_fd_[1]};

    unsigned long flags = CLONE_NEWPID | SIGCHLD;
    if (!config_.enable_network) flags |= CLONE_NEWNET;

    auto* stack = static_cast<char*>(malloc(kStackSize));
    if (!stack) {
        result.exit_code = -1;
        result.err = "failed to allocate child stack";
        cleanup();
        return result;
    }

    auto start = std::chrono::steady_clock::now();
    child_pid_ = clone(child_entry, stack + kStackSize, flags, &child_args);

    if (child_pid_ < 0) {
        result.exit_code = -1;
        result.err = std::format("clone() failed: {}", strerror(errno));
        free(stack);
        cleanup();
        return result;
    }

    close(pipe_fd_[1]);
    pipe_fd_[1] = -1;

    char buf[4096];
    ssize_t n;
    while ((n = read(pipe_fd_[0], buf, sizeof(buf) - 1)) > 0) {
        buf[n] = '\0';
        std::string chunk(buf, n);
        if (output_fn) output_fn(chunk);
        result.out += chunk;
    }

    int status = 0;
    waitpid(child_pid_, &status, 0);

    auto end = std::chrono::steady_clock::now();
    result.duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();

    // The child has exited; its stack is no longer in use.
    free(stack);

    if (WIFEXITED(status)) {
        result.exit_code = WEXITSTATUS(status);
    } else if (WIFSIGNALED(status)) {
        result.exit_code = 128 + WTERMSIG(status);
    } else {
        result.exit_code = -1;
    }

    cleanup();
    return result;
}

#else

// Non-Linux stub
ExecResult Sandbox::run(const std::string&, const std::vector<std::string>&,
                        std::function<void(const std::string&)> output_fn) {
    ExecResult result{};
    result.exit_code = -1;
    result.err = "sandbox only supported on Linux (use Docker or VM for macOS)";
    return result;
}

#endif

} // namespace devops