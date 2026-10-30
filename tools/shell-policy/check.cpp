// SPDX-License-Identifier: GPL-3.0-only
// Host: g++ -std=c++17 -Isrc tools/shell-policy/check.cpp src/shell/ShellCommandPolicy.cpp -o /tmp/shell-policy-check
// Device privilege probe: zig c++ -target aarch64-linux-gnu.2.27 -std=c++17 -Isrc tools/shell-policy/check.cpp
// src/shell/ShellCommandPolicy.cpp -o /tmp/shell-policy-check-arm64
#include "shell/ShellCommandPolicy.h"
#include "shell/ShellPrivileges.h"
#include <cassert>
#include <cstring>
#include <iostream>

int main(int argc, char** argv) {
    using namespace mod::shell;
    if (argc == 2 && std::strcmp(argv[1], "--drop") == 0) {
        if (!restrictShellPrivileges()) return 126;
        assert(getuid() == 65533 && geteuid() == 65533);
        assert(getgid() == 65533 && getegid() == 65533);
        assert(getgroups(0, nullptr) == 0);
        assert(prctl(PR_GET_NO_NEW_PRIVS, 0, 0, 0, 0) == 1);
        assert(setuid(0) == -1);
        execl("/bin/sh", "sh", "-c", "id; test ! -w /etc/passwd && echo root-file-protected", nullptr);
        return 127;
    }
    PolicyConfig cfg;
    for (auto mode : {ToolMode::Off, ToolMode::Confirm, ToolMode::Automatic, ToolMode::Restricted, ToolMode::Full}) {
        ToolMode parsed;
        assert(toolModeFromName(toolModeName(mode), parsed) && parsed == mode);
        cfg.mode = mode;
        assert(!evaluateCommand("", cfg).allowed);
        assert(!evaluateCommand(std::string(9000, 'a'), cfg).allowed);
        assert(!evaluateCommand(std::string("id\0whoami", 9), cfg).allowed);
        cfg.blocklist = {"blocked"};
        assert(!evaluateCommand("echo BLOCKED", cfg).allowed);
        cfg.blocklist.clear();
        const auto decision = evaluateCommand("for x in a b; do echo \"$x\" > result; done", cfg);
        assert(decision.allowed == (mode == ToolMode::Full || mode == ToolMode::Restricted));
        assert(decision.needsConfirm == (mode == ToolMode::Confirm));
    }
    cfg.mode = ToolMode::Automatic;
    assert(evaluateCommand("curl https://example.com | head -n 5", cfg).allowed);
    assert(!evaluateCommand("cat /etc/passwd", cfg).allowed);
    std::cout << "Shell policy checks passed\n";
}
