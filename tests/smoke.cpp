#include <devops/sandbox.hpp>
#include <iostream>
#include <string>

#include <chrono>

namespace {

int check(const std::string& name, devops::SandboxConfig cfg, const std::string& cmd,
          int want_exit, const std::string& expect_in_stdout) {
    devops::Sandbox box(cfg);
    auto r = box.run(cmd, {});
    bool ok = r.exit_code == want_exit && r.out.find(expect_in_stdout) != std::string::npos;
    std::cout << (ok ? "  ok   " : "  FAIL ")
              << name << " [exit=" << r.exit_code << " want=" << want_exit
              << ", " << r.duration_ms << "ms, stdout=\"" << r.out << "\"]\n";
    if (!ok) std::cout << "        stderr: " << r.err << "\n";
    return ok ? 0 : 1;
}

} // namespace

int main() {
    int fails = 0;

    devops::SandboxConfig base;
    base.id = "smoke";

    fails += check("seccomp-on  printf", base, "printf 'hello-from-sandbox\\n'", 0, "hello-from-sandbox");
    fails += check("seccomp-on  builtin  1+2", base, "echo $((1 + 2))", 0, "3");

    devops::SandboxConfig no_sec = base;
    no_sec.enable_seccomp = false;
    fails += check("seccomp-off printf", no_sec, "printf 'hello-from-sandbox\\n'", 0, "hello-from-sandbox");

    fails += check("exit code    42", base, "exit 42", 42, "");
    fails += check("nonzero      false", base, "false", 1, "");

    std::cout << ((fails == 0) ? "PASS" : "FAIL") << " sandbox_smoke (" << fails << " failures)\n";
    return fails == 0 ? 0 : 1;
}