// Included by main.cpp: keep the CLI/store interface private to this executable.
namespace {
#ifndef PENMODS_UPDATE_TRIAL_SECONDS
#define PENMODS_UPDATE_TRIAL_SECONDS 20
#endif
#ifndef PENMODS_UPDATE_TIMEOUT_SECONDS
#define PENMODS_UPDATE_TIMEOUT_SECONDS 120
#endif
std::string processStart(pid_t pid) {
    std::ifstream file("/proc/" + std::to_string(pid) + "/stat");
    std::string   line;
    std::getline(file, line);
    auto end = line.rfind(')');
    if (end == std::string::npos) return {};
    std::istringstream fields(line.substr(end + 2));
    std::string        field;
    for (int i = 3; i <= 22; ++i)
        if (!(fields >> field)) return {};
    return field;
}
void killChild(pid_t child) {
    kill(-child, SIGTERM);
    kill(child, SIGTERM);
    auto deadline = Clock::now() + std::chrono::seconds(2);
    int  status;
    while (Clock::now() < deadline) {
        auto result = waitpid(child, &status, WNOHANG);
        if (result == child || (result < 0 && errno == ECHILD)) return;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    kill(-child, SIGKILL);
    kill(child, SIGKILL);
    while (waitpid(child, &status, 0) < 0 && errno == EINTR) {}
}
void rollback(Store& store, Json& state, const std::string& reason) {
    // Journal remains armed until pointer and aliases are durably restored.
    auto confirmed = state.at("confirmed").get<std::string>();
    store.active(confirmed);
    store.aliases(store.generation(confirmed).at("components"));
    fault("rollback-pointer");
    state["trial"]      = nullptr;
    state["pending"]    = nullptr;
    state["last_error"] = reason;
    store.save(state);
    fault("rollback-committed");
}
class ChildGuard {
public:
    pid_t pid = 0;
    ~ChildGuard() {
        if (pid > 0) killChild(pid);
    }
};
int supervise(Store& store, char** argv) {
    ChildGuard childGuard;
    Lock       supervisor(store.root / "supervisor.lock", true);
    for (int sig : {SIGTERM, SIGINT, SIGHUP, SIGQUIT}) signal(sig, signalHandler);
    Json        state;
    std::string generationId;
    std::string trialToken;
    bool        trial = false;
    {
        Lock lock(store.root / "store.lock");
        store.initialize();
        state = store.state();
        if (state.at("trial").is_object()) {
            // A previous helper died/rebooted after arming a trial. Never retry it.
            auto old   = state.at("trial");
            auto pid   = old.value("pid", 0);
            auto start = old.value("process_start", std::string());
            if (pid > 1 && !start.empty() && processStart(pid) == start) {
                kill(-pid, SIGKILL);
                kill(pid, SIGKILL);
            }
            rollback(store, state, "interrupted startup trial");
        }
        bool deferred = state.at("pending").is_string() && store.lowBattery();
        if (deferred) {
            state["last_error"] = "Update deferred: battery below 20% without external power";
            store.save(state);
        }
        if (state.at("pending").is_string() && !deferred) {
            generationId   = state.at("pending").get<std::string>();
            trialToken     = token();
            trial          = true;
            state["trial"] = Json{
                {"generation", generationId},
                {"token",      trialToken  },
                {"pid",        0           },
                {"attempted",  true        }
            };
            state["pending"] = nullptr;
            store.save(state);
            fault("trial-armed");
        } else generationId = state.at("confirmed").get<std::string>();
        try {
            store.validateGeneration(generationId);
        } catch (const std::exception& error) {
            if (trial) {
                rollback(store, state, std::string("candidate validation failed: ") + error.what());
                trial = false;
                trialToken.clear();
                generationId = state.at("confirmed");
            }
            try {
                store.validateGeneration(generationId);
            } catch (const std::exception&) {
                if (!state["previous"].is_string()) throw;
                generationId = state["previous"].get<std::string>();
                store.validateGeneration(generationId);
                state["confirmed"]  = generationId;
                state["previous"]   = nullptr;
                state["pending"]    = nullptr;
                state["trial"]      = nullptr;
                state["last_error"] = "confirmed generation damaged; recovered previous generation";
                store.save(state);
            }
        }
        store.active(generationId);
        store.aliases(store.generation(generationId).at("components"));
        store.collect();
    }
    for (;;) {
        auto parent = getpid();
        auto child  = fork();
        checked(child >= 0, "fork app");
        if (!child) {
            setpgid(0, 0);
            // Parent death cannot leave the failed candidate alive behind a restarted helper.
            prctl(PR_SET_PDEATHSIG, SIGKILL);
            if (getppid() != parent) _exit(125);
            for (int sig : {SIGTERM, SIGINT, SIGHUP, SIGQUIT}) signal(sig, SIG_DFL);
            setenv("PENMODS_UPDATE_ROOT", store.root.c_str(), 1);
            setenv("PENMODS_UPDATE_GENERATION", generationId.c_str(), 1);
            if (trial) setenv("PENMODS_UPDATE_TOKEN", trialToken.c_str(), 1);
            else unsetenv("PENMODS_UPDATE_TOKEN");
            execvp(argv[0], argv);
            perror("penmods-update exec app");
            _exit(127);
        }
        childGuard.pid = child;
        setpgid(child, child);
        if (trial) {
            Lock lock(store.root / "store.lock");
            state                           = store.state();
            state["trial"]["pid"]           = child;
            state["trial"]["process_start"] = processStart(child);
            store.save(state);
            fault("trial-child-recorded");
        }
        auto     launched = Clock::now(), first = Clock::time_point{}, progress = Clock::time_point{};
        uint64_t sequence = 0;
        unsigned advances = 0;
        bool     failed   = false;
        int      status   = 0;
        for (;;) {
            if (receivedSignal) {
                auto sig       = receivedSignal;
                receivedSignal = 0;
                kill(-child, sig);
                kill(child, sig);
                auto deadline = Clock::now() + std::chrono::seconds(3);
                bool reaped   = false;
                while (Clock::now() < deadline) {
                    auto result = waitpid(child, &status, WNOHANG);
                    if (result == child || (result < 0 && errno == ECHILD)) {
                        reaped = true;
                        break;
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(20));
                }
                if (!reaped) killChild(child);
                childGuard.pid = 0;
                if (trial) {
                    Lock lock(store.root / "store.lock");
                    state = store.state();
                    rollback(store, state, "startup trial interrupted by signal");
                }
                return 128 + sig;
            }
            auto exited = waitpid(child, &status, WNOHANG);
            if (exited == child) {
                failed = trial;
                break;
            }
            if (exited < 0 && errno != EINTR) fail("waitpid app failed");
            if (trial) {
                auto now = Clock::now();
                try {
                    auto health = readJson(store.root / "health" / (trialToken + ".json"), 16384);
                    auto seq    = health.at("sequence").get<uint64_t>();
                    if (health.at("generation") == generationId && health.at("token") == trialToken
                        && health.at("pid") == child && seq > sequence) {
                        if (first == Clock::time_point{} || now - progress > std::chrono::seconds(6)) {
                            first    = now;
                            advances = 0;
                        }
                        sequence = seq;
                        progress = now;
                        ++advances;
                    }
                } catch (const std::exception&) { /* incomplete, missing, or unrelated heartbeat is not evidence */
                }
                if (first != Clock::time_point{} && advances >= 3
                    && now - first >= std::chrono::seconds(PENMODS_UPDATE_TRIAL_SECONDS)
                    && now - progress <= std::chrono::seconds(6)) {
                    Lock lock(store.root / "store.lock");
                    state               = store.state();
                    state["previous"]   = state.at("confirmed");
                    state["confirmed"]  = generationId;
                    state["trial"]      = nullptr;
                    state["last_error"] = "";
                    store.save(state);
                    fault("trial-confirmed");
                    trial = false;
                    std::error_code ec;
                    fs::remove(store.root / "health" / (trialToken + ".json"), ec);
                } else if (now - launched >= std::chrono::seconds(PENMODS_UPDATE_TIMEOUT_SECONDS)) {
                    failed = true;
                    killChild(child);
                    break;
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        childGuard.pid = 0;
        if (!failed) return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
        {
            Lock lock(store.root / "store.lock");
            state = store.state();
            rollback(store, state, "startup trial exited or heartbeat timed out");
            generationId = state.at("confirmed").get<std::string>();
            trial        = false;
            trialToken.clear();
        }
        std::cerr << "penmods-update: trial failed; starting confirmed generation " << generationId << '\n';
        // Keep guardian's watched process alive through the rollback/relaunch.
    }
}
int emergencyRun(const fs::path& root, const fs::path& prefix, char** argv) {
    auto mod            = prefix.empty() ? fs::path("/userdata/PenMods") : prefix / "userdata/PenMods";
    auto mainBackup     = mod / "libPenMods.so.pre-update-system";
    auto resourceBackup = mod / "libPenModsResources.so.pre-update-system";
    auto validBackup    = [](const fs::path& file) {
        if (!fs::is_regular_file(fs::symlink_status(file)))
            fail("independent emergency backup missing: " + file.string());
        std::ifstream              input(file, std::ios::binary);
        std::vector<unsigned char> header(64);
        if (!input.read(reinterpret_cast<char*>(header.data()), header.size())) fail("emergency backup truncated");
        validateElf(header, true);
    };
    auto mainPath = mod / "libPenMods.so", resourcePath = mod / "libPenModsResources.so";
    auto mainSource = fs::exists(fs::symlink_status(mainBackup)) ? mainBackup : mainPath;
    validBackup(mainSource);
    std::optional<fs::path> resourceSource;
    if (fs::exists(fs::symlink_status(resourceBackup))) resourceSource = resourceBackup;
    else if (fs::is_regular_file(fs::symlink_status(resourcePath))) resourceSource = resourcePath;
    if (resourceSource) validBackup(*resourceSource);
    // All backup checks precede alias writes. An unmigrated regular legacy file
    // is usable; a current managed symlink is never evidence for recovery.
    if (mainSource != mainPath) linkAtomic(mainSource, mainPath);
    if (resourceSource) {
        if (*resourceSource != resourcePath) linkAtomic(*resourceSource, resourcePath);
    } else {
        auto            resources = mod / "libPenModsResources.so";
        std::error_code ec;
        if (fs::is_symlink(fs::symlink_status(resources, ec))
            && fs::read_symlink(resources).string().rfind((root / "active").string() + '/', 0) == 0) {
            checked(unlink(resources.c_str()) == 0, "remove unavailable managed resources");
            syncDirectory(mod);
        }
    }
    std::cerr << "penmods-update: using independent userdata backup; managed updates require repair\n";
    for (int sig : {SIGTERM, SIGINT, SIGHUP, SIGQUIT}) signal(sig, signalHandler);
    auto parent = getpid();
    auto child  = fork();
    checked(child >= 0, "emergency app fork");
    if (!child) {
        setpgid(0, 0);
        prctl(PR_SET_PDEATHSIG, SIGKILL);
        if (getppid() != parent) _exit(125);
        for (int sig : {SIGTERM, SIGINT, SIGHUP, SIGQUIT}) signal(sig, SIG_DFL);
        unsetenv("PENMODS_UPDATE_ROOT");
        unsetenv("PENMODS_UPDATE_GENERATION");
        unsetenv("PENMODS_UPDATE_TOKEN");
        execvp(argv[0], argv);
        perror("emergency app exec");
        _exit(127);
    }
    ChildGuard guard;
    guard.pid = child;
    setpgid(child, child);
    int status = 0;
    for (;;) {
        if (receivedSignal) {
            auto sig       = receivedSignal;
            receivedSignal = 0;
            kill(-child, sig);
            killChild(child);
            guard.pid = 0;
            return 128 + sig;
        }
        auto result = waitpid(child, &status, WNOHANG);
        if (result == child) {
            guard.pid = 0;
            return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
        }
        if (result < 0 && errno != EINTR) fail("emergency app wait failed");
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}
} // namespace
} // namespace penmods::update

int main(int argc, char** argv) {
    using namespace penmods::update;
    fs::path root        = "/userdisk/PenMods/update-system", prefix;
    char**   originalApp = nullptr;
    try {
        std::string coreVersion = PENMODS_VERSION;
        int         i           = 1;
        while (i < argc && std::string(argv[i]).rfind("--", 0) == 0) {
            std::string option = argv[i++];
            if (i >= argc) fail("option value missing");
            if (option == "--root") root = argv[i++];
            else if (option == "--prefix") prefix = fs::absolute(argv[i++]);
            else if (option == "--core-version") coreVersion = argv[i++];
            else fail("unknown option: " + option);
        }
        if (i >= argc)
            fail(
                "usage: penmods-update [--root DIR] [--prefix DIR] [--core-version X.Y.Z] status|init|stage DESCRIPTOR "
                "ZIP|cancel|run -- APP ARGS..."
            );
        std::string command = argv[i++];
        Store       store(root, prefix);
        store.coreVersion = coreVersion;
        version(coreVersion);
        if (command == "run" && i < argc && std::string(argv[i]) == "--") {
            if (++i >= argc) fail("app command missing");
            // Even probing the store can throw (I/O, permissions, symlink loops).
            // Arm emergency recovery before the first filesystem access.
            originalApp = argv + i;
            if (fs::exists(store.root / "disabled")) {
                execvp(argv[i], argv + i);
                perror("disabled launcher exec");
                return 127;
            }
            store.prepare();
            return supervise(store, argv + i);
        }
        if (command == "status" && !fs::exists(store.root / "state.json")) {
            std::cout << store.status().dump() << '\n';
            return 0;
        }
        store.prepare();
        if (command == "init" && i < argc && std::string(argv[i]) == "--reinstall-snapshot") {
            Lock noApp(store.root / "supervisor.lock", true);
            Lock lock(store.root / "store.lock");
            store.initialize(true, true);
            store.collect();
        } else if (command == "enable") {
            Lock noApp(store.root / "supervisor.lock", true);
            Lock lock(store.root / "store.lock");
            store.enable();
        } else {
            Lock lock(store.root / "store.lock");
            if (command == "init") {
                bool snapshotOnly = i < argc && std::string(argv[i]) == "--snapshot-only";
                if (snapshotOnly) store.initialize(true);
                else {
                    Lock noApp(store.root / "supervisor.lock", true);
                    store.initialize();
                    auto s = store.state();
                    store.validateGeneration(s.at("confirmed"));
                    store.active(s.at("confirmed"));
                    store.aliases(store.generation(s.at("confirmed")).at("components"));
                }
                store.collect();
            } else if (command == "stage") {
                if (argc - i != 2) fail("stage requires descriptor and archive");
                store.initialize(true);
                store.collect();
                try {
                    store.stage(argv[i], argv[i + 1]);
                } catch (...) {
                    try {
                        store.collect();
                    } catch (...) {}
                    throw;
                }
                store.collect();
            } else if (command == "cancel") {
                auto s = store.state();
                if (!s.at("trial").is_null()) fail("cannot cancel a running trial");
                s["pending"] = nullptr;
                store.save(s);
                store.collect();
            } else if (command != "status") fail("unknown command: " + command);
        }
        std::cout << store.status().dump() << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "penmods-update: " << error.what() << '\n';
        if (originalApp) {
            try {
                return emergencyRun(root, prefix, originalApp);
            } catch (const std::exception& recoveryError) {
                std::cerr << "penmods-update: emergency recovery unavailable: " << recoveryError.what() << '\n';
            }
        }
        return 1;
    }
}
