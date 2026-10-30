// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <grp.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <unistd.h>

namespace mod::shell {

// Called only in the forked child. Fail closed before exec; never fall back to root.
// No account/passwd mutation is needed. This UID is distinct from stock services.
inline bool restrictShellPrivileges() {
    constexpr uid_t uid = 65533;
    constexpr gid_t gid = 65533;
    const rlimit    core{0, 0};
    const rlimit    files{64, 64};
    const rlimit    processes{32, 32};
    const rlimit    fileSize{16 * 1024 * 1024, 16 * 1024 * 1024};
    umask(0077);
    return prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) == 0 && setrlimit(RLIMIT_CORE, &core) == 0
        && setrlimit(RLIMIT_NOFILE, &files) == 0 && setrlimit(RLIMIT_NPROC, &processes) == 0
        && setrlimit(RLIMIT_FSIZE, &fileSize) == 0 && setgroups(0, nullptr) == 0 && setresgid(gid, gid, gid) == 0
        && setresuid(uid, uid, uid) == 0;
}

} // namespace mod::shell
