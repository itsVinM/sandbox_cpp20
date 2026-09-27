#include <devops/sandbox.hpp>
#include <format>

// Minimal translation unit: verifies the header + stub sandbox.cpp compile
// (and link) for non-Linux targets. Runtime sandboxing is Linux-only on purpose.
int main() {
    devops::Sandbox box(devops::SandboxConfig{});
    auto r = box.run("printf hi", {});
    if (r.exit_code != -1) return 1;
    if (r.err.find("sandbox only supported on Linux") == std::string::npos) return 2;
    return 0;
}