#pragma once

namespace devops {

class Seccomp {
public:
    static bool install_default_filter();
};

} // namespace devops
