// SPDX-License-Identifier: GPL-3.0-only
// Stable, Qt-free recovery launcher. Never load or link a candidate library here.
#include "Archive.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <iostream>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <sys/file.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

#ifndef PENMODS_VERSION
#define PENMODS_VERSION "0.0.0"
#endif
#ifndef PENMODS_CHANNEL
#define PENMODS_CHANNEL "dev"
#endif

namespace penmods::update {
using Json = nlohmann::json;
namespace {
using Clock                          = std::chrono::steady_clock;
volatile sig_atomic_t receivedSignal = 0;
void                  signalHandler(int signal) { receivedSignal = signal; }
[[noreturn]] void     fail(const std::string& what) { throw std::runtime_error(what); }
void                  checked(bool ok, const std::string& what) {
    if (!ok) fail(what + ": " + std::strerror(errno));
}
class Fd {
public:
    int value;
    explicit Fd(int fd) : value(fd) {}
    ~Fd() {
        if (value >= 0) close(value);
    }
    Fd(const Fd&)            = delete;
    Fd& operator=(const Fd&) = delete;
};
std::string token() {
    std::array<unsigned char, 16> bytes;
    Fd                            random(open("/dev/urandom", O_RDONLY | O_CLOEXEC));
    checked(random.value >= 0, "open random source");
    size_t done = 0;
    while (done < bytes.size()) {
        auto n = read(random.value, bytes.data() + done, bytes.size() - done);
        if (n < 0 && errno == EINTR) continue;
        checked(n > 0, "read random source");
        done += n;
    }
    constexpr char digits[] = "0123456789abcdef";
    std::string    out;
    for (auto byte : bytes) {
        out += digits[byte >> 4];
        out += digits[byte & 15];
    }
    return out;
}
void syncDirectory(const fs::path& path) {
    Fd fd(open(path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
    checked(fd.value >= 0, "open directory " + path.string());
    checked(fsync(fd.value) == 0, "fsync directory " + path.string());
}
void directories(const fs::path& path) {
    if (path.empty()) return;
    if (fs::exists(path)) {
        if (!fs::is_directory(path)) fail("not a directory: " + path.string());
        return;
    }
    directories(path.parent_path());
    checked(mkdir(path.c_str(), 0755) == 0 || errno == EEXIST, "mkdir " + path.string());
    syncDirectory(path.parent_path());
}
void writeBytes(const fs::path& path, const unsigned char* bytes, size_t size, mode_t mode = 0644) {
    directories(path.parent_path());
    Fd fd(open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, mode));
    checked(fd.value >= 0, "create " + path.string());
    size_t done = 0;
    while (done < size) {
        auto n = write(fd.value, bytes + done, size - done);
        if (n < 0 && errno == EINTR) continue;
        checked(n > 0, "write " + path.string());
        done += n;
    }
    checked(fsync(fd.value) == 0, "fsync " + path.string());
}
void atomicJson(const fs::path& path, const Json& data) {
    auto     text      = data.dump(2) + '\n';
    fs::path temporary = path.string() + ".tmp-" + token();
    writeBytes(temporary, reinterpret_cast<const unsigned char*>(text.data()), text.size());
    checked(rename(temporary.c_str(), path.c_str()) == 0, "publish " + path.string());
    syncDirectory(path.parent_path());
}
Json readJson(const fs::path& path, size_t limit = 1024 * 1024) {
    if (!fs::is_regular_file(path) || fs::file_size(path) > limit) fail("missing or oversized JSON: " + path.string());
    std::ifstream in(path);
    Json          result;
    in >> result;
    if (!result.is_object()) fail("JSON object required: " + path.string());
    return result;
}
void copyFile(const fs::path& source, const fs::path& target, mode_t mode = 0644) {
    Fd input(open(source.c_str(), O_RDONLY | O_CLOEXEC));
    checked(input.value >= 0, "open " + source.string());
    struct stat st{};
    checked(fstat(input.value, &st) == 0 && S_ISREG(st.st_mode), "regular input required");
    directories(target.parent_path());
    Fd output(open(target.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, mode));
    checked(output.value >= 0, "create " + target.string());
    std::array<unsigned char, 65536> bytes;
    for (;;) {
        auto n = read(input.value, bytes.data(), bytes.size());
        if (n < 0 && errno == EINTR) continue;
        checked(n >= 0, "copy read");
        if (!n) break;
        ssize_t done = 0;
        while (done < n) {
            auto count = write(output.value, bytes.data() + done, n - done);
            if (count < 0 && errno == EINTR) continue;
            checked(count > 0, "copy write");
            done += count;
        }
    }
    checked(fsync(output.value) == 0, "fsync copied file");
}
void syncTree(const fs::path& root) {
    std::vector<fs::path> dirs{root};
    for (const auto& entry : fs::recursive_directory_iterator(root)) {
        auto st = entry.symlink_status();
        if (fs::is_directory(st)) dirs.push_back(entry.path());
        else if (!fs::is_regular_file(st) && !fs::is_symlink(st)) fail("special file in store");
    }
    std::sort(dirs.begin(), dirs.end(), [](const auto& a, const auto& b) {
        return a.native().size() > b.native().size();
    });
    for (const auto& dir : dirs) syncDirectory(dir);
}
void linkAtomic(const fs::path& target, const fs::path& alias) {
    directories(alias.parent_path());
    auto temporary = alias.string() + ".tmp-" + token();
    checked(symlink(target.c_str(), temporary.c_str()) == 0, "create alias " + alias.string());
    checked(rename(temporary.c_str(), alias.c_str()) == 0, "replace alias " + alias.string());
    syncDirectory(alias.parent_path());
}
void fault(const char* point) {
#ifdef PENMODS_UPDATE_TESTING
    const char* requested = getenv("PENMODS_UPDATE_FAILPOINT");
    if (requested && std::string(requested) == point) _exit(86);
#else
    (void)point;
#endif
}
class Lock {
    Fd fd;

public:
    explicit Lock(const fs::path& path, bool nonblock = false)
    : fd(open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0644)) {
        checked(fd.value >= 0, "open lock");
        checked(flock(fd.value, LOCK_EX | (nonblock ? LOCK_NB : 0)) == 0, "lock " + path.string());
    }
};
std::array<unsigned long, 3> version(const std::string& text) {
    std::array<unsigned long, 3> result{};
    std::istringstream           in(text);
    char                         dot;
    for (size_t i = 0; i < result.size(); ++i) {
        if (!(in >> result[i]) || result[i] > 1000000) fail("invalid version: " + text);
        if (i != 2 && (!(in >> dot) || dot != '.')) fail("invalid version: " + text);
    }
    if (in.peek() != std::char_traits<char>::eof()) fail("invalid version: " + text);
    return result;
}
bool digest(const std::string& value, size_t length) {
    return value.size() == length && value.find_first_not_of("0123456789abcdefABCDEF") == std::string::npos;
}
std::string lowercase(std::string value) {
    for (char& c : value)
        if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
    return value;
}
std::string legacyMd5(const fs::path& path) {
    int pipes[2];
    checked(pipe2(pipes, O_CLOEXEC) == 0, "MD5 pipe");
    auto pid = fork();
    checked(pid >= 0, "MD5 fork");
    if (!pid) {
        dup2(pipes[1], STDOUT_FILENO);
        close(pipes[0]);
        close(pipes[1]);
        execl("/usr/bin/md5sum", "md5sum", path.c_str(), static_cast<char*>(nullptr));
        execl("/bin/md5sum", "md5sum", path.c_str(), static_cast<char*>(nullptr));
        _exit(127);
    }
    close(pipes[1]);
    std::array<char, 4096> data{};
    ssize_t                n;
    do {
        n = read(pipes[0], data.data(), data.size() - 1);
    } while (n < 0 && errno == EINTR);
    close(pipes[0]);
    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    if (n < 32 || !WIFEXITED(status) || WEXITSTATUS(status)) fail("md5sum failed");
    return lowercase(std::string(data.data(), 32));
}
void validateElf(const std::vector<unsigned char>& bytes, bool library) {
    if (bytes.size() < 64 || std::memcmp(bytes.data(), "\177ELF", 4) || bytes[4] != 2 || bytes[5] != 1
        || bytes[18] != 183 || bytes[19] != 0 || (library ? bytes[16] != 3 : (bytes[16] != 2 && bytes[16] != 3))
        || bytes[17])
        fail("payload must be a little-endian aarch64 ELF " + std::string(library ? "shared library" : "executable"));
}
const std::set<std::string> componentIds{"core", "player", "mathjax", "ppocr"};
bool                        payloadPath(const std::string& id, const std::string& name) {
    if (id == "core") return name == "libPenMods.so" || name == "libPenModsResources.so";
    if (id == "player") return name == "FFmpegPlayer/libffmpegplayerplugin.so" || name == "FFmpegPlayer/qmldir";
    if (id == "mathjax") return name == "mathjax-server";
    if (id == "ppocr")
        return name == "libyocr.so"
            || (name.rfind("models/", 0) == 0
                && name.substr(7).find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_.-")
                       == std::string::npos
                && name.find('/', 7) == std::string::npos
                && (name.size() > 11
                    && (name.compare(name.size() - 11, 11, ".ncnn.param") == 0
                        || name.compare(name.size() - 9, 9, ".ncnn.bin") == 0)));
    return false;
}
void validateFiles(const std::string& id, const fs::path& path) {
    std::set<std::string> names;
    for (const auto& entry : fs::recursive_directory_iterator(path)) {
        if (entry.is_directory()) continue;
        if (!fs::is_regular_file(entry.symlink_status())) fail("payload link or special file");
        auto name = entry.path().lexically_relative(path).generic_string();
        if (name == "object.json") continue;
        if (!payloadPath(id, name) || !fs::file_size(entry.path())) fail("unexpected or empty payload: " + name);
        names.insert(name);
    }
    auto require = [&](const char* name) {
        if (!names.count(name)) fail("incomplete " + id + " package: " + name);
    };
    if (id == "core") {
        require("libPenMods.so");
        require("libPenModsResources.so");
    }
    if (id == "player") {
        require("FFmpegPlayer/libffmpegplayerplugin.so");
        require("FFmpegPlayer/qmldir");
    }
    if (id == "mathjax") require("mathjax-server");
    if (id == "ppocr") {
        require("libyocr.so");
        require("models/PP_OCRv5_mobile_rec.ncnn.param");
        require("models/PP_OCRv5_mobile_rec.ncnn.bin");
        bool pair = false;
        for (const auto& name : names)
            if (name.size() > 11 && name.compare(name.size() - 11, 11, ".ncnn.param") == 0) {
                auto bin = name.substr(0, name.size() - 11) + ".ncnn.bin";
                if (!names.count(bin)) fail("OCR model parameter/bin pair missing");
                pair = true;
            }
        for (const auto& name : names)
            if (name.size() > 9 && name.compare(name.size() - 9, 9, ".ncnn.bin") == 0
                && !names.count(name.substr(0, name.size() - 9) + ".ncnn.param"))
                fail("OCR model bin/parameter pair missing");
        if (!pair) fail("OCR package has no model pair");
    }
}
} // namespace

class Store {
public:
    fs::path    root;
    fs::path    prefix;
    std::string coreVersion = PENMODS_VERSION;
    Store(fs::path path, fs::path devicePrefix)
    : root(fs::absolute(path).lexically_normal()),
      prefix(std::move(devicePrefix)) {}
    fs::path device(const fs::path& path) const { return prefix.empty() ? path : prefix / path.relative_path(); }
    void     prepare() {
        if (prefix.empty() && root == fs::path("/userdisk/PenMods/update-system")) {
            struct stat disk{}, system{};
            checked(
                stat("/userdisk", &disk) == 0 && stat("/", &system) == 0 && disk.st_dev != system.st_dev,
                "userdisk volume unavailable"
            );
        }
        directories(root);
        for (const auto* dir : {"objects", "generations", "health", "staging"}) directories(root / dir);
    }
    Json state() const {
        try {
            return readJson(root / "state.json");
        } catch (const std::exception&) {
            auto previous = readJson(root / "state.backup.json");
            std::cerr << "penmods-update: using durable state backup after corrupt state\n";
            return previous;
        }
    }
    void save(const Json& value) {
        if (fs::exists(root / "state.json")) {
            try {
                atomicJson(root / "state.backup.json", readJson(root / "state.json"));
            } catch (const std::exception&) { /* retain the previous valid backup */
            }
        } else atomicJson(root / "state.backup.json", value);
        atomicJson(root / "state.json", value);
    }
    Json generation(const std::string& id) const {
        if (id.empty() || id.find_first_not_of("0123456789abcdef") != std::string::npos)
            fail("invalid generation identifier");
        return readJson(root / "generations" / id / "manifest.json");
    }
    std::string publishGeneration(const Json& components) {
        auto id  = token();
        auto tmp = root / "generations" / (".tmp-" + id);
        directories(tmp);
        for (auto it = components.begin(); it != components.end(); ++it) {
            if (!componentIds.count(it.key())) fail("unknown stored component");
            auto object = it.value().at("object").get<std::string>();
            if (object.empty() || object.find_first_not_of("0123456789abcdef") != std::string::npos)
                fail("invalid object identifier");
            checked(
                symlink((fs::path("../../objects") / object).c_str(), (tmp / it.key()).c_str()) == 0,
                "generation member"
            );
        }
        atomicJson(
            tmp / "manifest.json",
            Json{
                {"schema",     1         },
                {"components", components}
        }
        );
        syncTree(tmp);
        checked(rename(tmp.c_str(), (root / "generations" / id).c_str()) == 0, "publish generation");
        syncDirectory(root / "generations");
        fault("generation-published");
        return id;
    }
    void active(const std::string& id) {
        generation(id);
        linkAtomic(fs::path("generations") / id, root / "active");
        fault("active-published");
    }
    bool matchesConfirmedSnapshot(const fs::path& aliasPath, const fs::path& path) const {
        std::string component, file;
        if (aliasPath == "/userdata/PenMods/libPenMods.so") {
            component = "core";
            file      = "libPenMods.so";
        } else if (aliasPath == "/userdata/PenMods/libPenModsResources.so") {
            component = "core";
            file      = "libPenModsResources.so";
        } else if (aliasPath == "/userdisk/ppocr_backend/libyocr.so" || aliasPath == "/userdisk/Qtlib/libyocr.so") {
            component = "ppocr";
            file      = "libyocr.so";
        } else return false;
        try {
            auto current  = state();
            auto id       = current.at("confirmed").get<std::string>();
            auto spec     = generation(id).at("components").at(component);
            auto expected = spec.at("files").at(file).get<std::string>();
            return fs::is_regular_file(fs::symlink_status(path)) && sha256File(path) == expected;
        } catch (const std::exception&) {
            return false;
        }
    }
    void alias(const fs::path& aliasPath, const fs::path& target) {
        auto            path = device(aliasPath);
        std::error_code ec;
        auto            st = fs::symlink_status(path, ec);
        if (fs::is_symlink(st) && fs::read_symlink(path) == target) return;
        if (fs::exists(st)) {
            fs::path backup = path.string() + ".pre-update-system";
            if (!fs::exists(fs::symlink_status(backup))) {
                checked(rename(path.c_str(), backup.c_str()) == 0, "preserve legacy " + path.string());
                syncDirectory(path.parent_path());
            } else if (!fs::is_symlink(st) && !matchesConfirmedSnapshot(aliasPath, path)) {
                fail("refusing to overwrite changed legacy path with existing backup: " + path.string());
            }
        }
        linkAtomic(target, path);
    }
    void removeManagedAlias(const fs::path& aliasPath) {
        auto            path = device(aliasPath);
        std::error_code ec;
        if (!fs::is_symlink(fs::symlink_status(path, ec))) return;
        auto target       = fs::read_symlink(path);
        auto activePrefix = (root / "active").string() + '/';
        if (target.string().rfind(activePrefix, 0) != 0) return;
        checked(unlink(path.c_str()) == 0, "remove inactive alias");
        auto backup = fs::path(path.string() + ".pre-update-system");
        if (fs::exists(fs::symlink_status(backup)))
            checked(rename(backup.c_str(), path.c_str()) == 0, "restore inactive alias");
        syncDirectory(path.parent_path());
    }
    void aliases(const Json& components) {
        if (components.contains("core")) {
            alias("/userdata/PenMods/libPenMods.so", root / "active/core/libPenMods.so");
            if (components.at("core").at("files").contains("libPenModsResources.so"))
                alias("/userdata/PenMods/libPenModsResources.so", root / "active/core/libPenModsResources.so");
            else removeManagedAlias("/userdata/PenMods/libPenModsResources.so");
        }
        if (components.contains("player"))
            alias("/userdata/PenMods/qml/FFmpegPlayer", root / "active/player/FFmpegPlayer");
        else removeManagedAlias("/userdata/PenMods/qml/FFmpegPlayer");
        if (components.contains("ppocr")) {
            alias("/userdisk/ppocr_backend/libyocr.so", root / "active/ppocr/libyocr.so");
            alias("/userdisk/ppocr_models", root / "active/ppocr/models");
            auto shadow = device("/userdisk/Qtlib/libyocr.so");
            if (fs::exists(fs::symlink_status(shadow)))
                alias("/userdisk/Qtlib/libyocr.so", root / "active/ppocr/libyocr.so");
        } else {
            removeManagedAlias("/userdisk/ppocr_backend/libyocr.so");
            removeManagedAlias("/userdisk/ppocr_models");
            removeManagedAlias("/userdisk/Qtlib/libyocr.so");
        }
    }
    std::optional<Json> import(const std::string& id, const std::vector<std::pair<fs::path, std::string>>& files) {
        if (files.empty()) return std::nullopt;
        for (const auto& file : files)
            if (!fs::is_regular_file(device(file.first))) return std::nullopt;
        auto object = token();
        auto tmp    = root / "staging" / object;
        directories(tmp);
        for (const auto& file : files) {
            auto destination = tmp / file.second;
            directories(destination.parent_path());
            // Same-filesystem legacy files are renamed to preserved backups during
            // alias migration; no writer is ever allowed to overwrite those inodes.
            if (link(fs::canonical(device(file.first)).c_str(), destination.c_str()) != 0)
                copyFile(device(file.first), destination, id == "mathjax" ? 0755 : 0644);
            Fd fd(open(destination.c_str(), O_RDONLY | O_CLOEXEC));
            checked(fd.value >= 0 && fsync(fd.value) == 0, "sync imported file");
        }
        if (id != "core") validateFiles(id, tmp);
        Json value{
            {"version",  id == "core" ? coreVersion : "0.0.0"},
            {"object",   object                              },
            {"requires", Json::object()                      },
            {"imported", true                                },
            {"files",    fileHashes(tmp)                     }
        };
        atomicJson(tmp / "object.json", value);
        syncTree(tmp);
        checked(rename(tmp.c_str(), (root / "objects" / object).c_str()) == 0, "publish baseline object");
        syncDirectory(root / "objects");
        return value;
    }
    void initialize(bool snapshotOnly = false, bool reinstall = false) {
        if (reinstall && !fs::exists(root / "disabled")) fail("reinstall snapshot requires disabled marker");
        if (fs::exists(root / "state.json") && !reinstall) return;
        Json                                          components = Json::object();
        std::vector<std::pair<fs::path, std::string>> coreFiles{
            {"/userdata/PenMods/libPenMods.so", "libPenMods.so"}
        };
        if (fs::is_regular_file(device("/userdata/PenMods/libPenModsResources.so")))
            coreFiles.emplace_back("/userdata/PenMods/libPenModsResources.so", "libPenModsResources.so");
        auto core = import("core", coreFiles);
        if (!core) fail("baseline core main library missing; install a known-good library before hooking launcher");
        components["core"] = *core;
        auto player        = import(
            "player",
            {
                {"/userdata/PenMods/qml/FFmpegPlayer/libffmpegplayerplugin.so",
                 "FFmpegPlayer/libffmpegplayerplugin.so"                                             },
                {"/userdata/PenMods/qml/FFmpegPlayer/qmldir",                   "FFmpegPlayer/qmldir"}
        }
        );
        if (player) components["player"] = *player;
        std::vector<std::pair<fs::path, std::string>> ocr{
            {"/userdisk/ppocr_backend/libyocr.so", "libyocr.so"}
        };
        auto modelPath = device("/userdisk/ppocr_models");
        if (fs::is_directory(modelPath))
            for (const auto& entry : fs::directory_iterator(modelPath)) {
                auto relative = "models/" + entry.path().filename().string();
                if (payloadPath("ppocr", relative) && entry.is_regular_file())
                    ocr.emplace_back(fs::path("/userdisk/ppocr_models") / entry.path().filename(), relative);
            }
        if (ocr.size() > 1 && fs::is_regular_file(device(ocr.front().first))) {
            auto ppocr = import("ppocr", ocr);
            if (ppocr) components["ppocr"] = *ppocr;
        }
        auto id = publishGeneration(components);
        save(
            Json{
                {"schema",     1      },
                {"confirmed",  id     },
                {"previous",   nullptr},
                {"pending",    nullptr},
                {"trial",      nullptr},
                {"last_error", ""     }
        }
        );
        fault(reinstall ? "reinstall-snapshot-committed" : "baseline-committed");
        if (!snapshotOnly) {
            active(id);
            aliases(components);
        }
    }
    Json exposed(const std::string& id) const {
        Json result     = Json::object();
        auto components = generation(id).at("components");
        for (auto it = components.begin(); it != components.end(); ++it) {
            auto path = root / "generations" / id / it.key();
            if (it.key() == "core") path /= "libPenMods.so";
            if (it.key() == "player") path /= "FFmpegPlayer";
            if (it.key() == "mathjax") path /= "mathjax-server";
            if (it.key() == "ppocr") path /= "libyocr.so";
            result[it.key()] = Json{
                {"version", it.value().at("version")},
                {"path",    path.string()           }
            };
        }
        return result;
    }
    std::pair<bool, std::string> launcherReadiness() const {
        if (fs::exists(root / "disabled")) return {false, "launcher disabled; enable after bootstrap completes"};
        auto launcher = device("/usr/bin/runDictPen");
        if (!fs::is_regular_file(fs::symlink_status(launcher)))
            return {false, "/usr/bin/runDictPen is missing or not a regular file"};
        auto helper = device("/userdata/PenMods/penmods-update");
        if (!fs::is_regular_file(fs::symlink_status(helper)) || access(helper.c_str(), X_OK) != 0)
            return {false, "/userdata/PenMods/penmods-update is missing or not executable"};
        const std::string hooked =
            "stdbuf -o0 /userdata/PenMods/penmods-update run -- ./YoudaoDictPen -platform wayland > $LOG_FILE 2>&1";
        const std::string stock = "stdbuf -o0 ./YoudaoDictPen -platform wayland > $LOG_FILE 2>&1";
        std::ifstream     input(launcher);
        std::string       line;
        unsigned          matches  = 0;
        bool              hasStock = false;
        while (std::getline(input, line)) {
            if (line == hooked) ++matches;
            if (line == stock) hasStock = true;
        }
        if (!input.eof()) return {false, "cannot read /usr/bin/runDictPen"};
        if (matches == 1 && !hasStock) return {true, ""};
        if (matches == 0 && hasStock)
            return {false, "launcher hook missing; reinstall the launch hook using tools/update/install-launcher.sh from the PenMods bootstrap package"};
        return {false, "/usr/bin/runDictPen has an unrecognized or ambiguous launcher hook"};
    }
    Json status() const {
        auto readiness = launcherReadiness();
        if (!fs::exists(root / "state.json"))
            return Json{
                {"schema",             1               },
                {"installed",          false           },
                {"active",             nullptr         },
                {"pending",            nullptr         },
                {"trial",              nullptr         },
                {"components",         Json::object()  },
                {"pending_components", Json::object()  },
                {"last_error",         ""              },
                {"launcher_ready",     readiness.first },
                {"launcher_error",     readiness.second}
            };
        auto value = state();
        auto id    = value["trial"].is_object() ? value["trial"].at("generation").get<std::string>()
                                                : value.at("confirmed").get<std::string>();
        return Json{
            {"schema", 1},
            {"installed", true},
            {"active", id},
            {"pending", value.at("pending")},
            {"trial", value.at("trial")},
            {"components", exposed(id)},
            {"pending_components",
             value["pending"].is_string() ? exposed(value["pending"].get<std::string>()) : Json::object()},
            {"last_error", value.value("last_error", std::string())},
            {"disabled", fs::exists(root / "disabled")},
            {"launcher_ready", readiness.first},
            {"launcher_error", readiness.second}
        };
    }
    void enable() {
        if (!fs::exists(root / "disabled")) fail("updates are not disabled");
        if (!fs::exists(root / "state.json")) fail("no update state to enable");
        auto current = state();
        if (!current.at("trial").is_null()) fail("cannot enable during a trial");
        if (!current.at("pending").is_string()) fail("enable requires a pending generation");
        auto baseline = current.at("confirmed").get<std::string>();
        auto pending  = current.at("pending").get<std::string>();
        validateGeneration(baseline);
        validateGeneration(pending);
        auto components = generation(pending).at("components");
        if (!components.contains("core")) fail("pending generation has no core");
        auto descriptor = components.at("core").at("descriptor");
        if (descriptor.value("health_protocol", 0) != 1) fail("pending core requires health_protocol 1");
        validateCombination(components);
        auto disabled = root / "disabled";
        Fd   marker(open(disabled.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW));
        checked(marker.value >= 0, "open disabled marker");
        checked(unlink(disabled.c_str()) == 0, "enable update launcher");
        syncDirectory(root);
        fault("enabled");
    }
    void validateCombination(const Json& components) {
        for (auto it = components.begin(); it != components.end(); ++it) {
            auto dependencies = it.value().value("requires", Json::object());
            if (!dependencies.is_object()) fail("requires must be an object");
            for (auto dep = dependencies.begin(); dep != dependencies.end(); ++dep) {
                if (!componentIds.count(dep.key()) || !components.contains(dep.key()) || !dep.value().is_object())
                    fail(it.key() + " requires missing or invalid dependency: " + dep.key());
                auto actualText = components.at(dep.key()).at("version").get<std::string>();
                auto actual     = version(actualText);
                for (auto bound = dep.value().begin(); bound != dep.value().end(); ++bound) {
                    auto required = version(bound.value().get<std::string>());
                    if (bound.key() == "min") {
                        if (actual < required)
                            fail(
                                it.key() + " requires " + dep.key() + " >= " + bound.value().get<std::string>()
                                + "; selected " + actualText
                            );
                    } else if (bound.key() == "max_exclusive") {
                        if (actual >= required)
                            fail(
                                it.key() + " requires " + dep.key() + " < " + bound.value().get<std::string>()
                                + "; selected " + actualText
                            );
                    } else fail("unknown dependency bound: " + bound.key());
                }
            }
        }
    }
    Json fileHashes(const fs::path& directory) const {
        Json hashes = Json::object();
        for (const auto& file : fs::recursive_directory_iterator(directory)) {
            if (fs::is_directory(file.symlink_status())) continue;
            if (!fs::is_regular_file(file.symlink_status())) fail("object contains link or special file");
            auto name = file.path().lexically_relative(directory).generic_string();
            if (name != "object.json") hashes[name] = sha256File(file.path());
        }
        return hashes;
    }
    void validateGeneration(const std::string& id) const {
        auto generationPath = root / "generations" / id;
        if (fs::is_symlink(fs::symlink_status(generationPath))) fail("generation directory is a link");
        auto components = generation(id).at("components");
        if (!components.contains("core")) fail("generation has no core");
        for (auto it = components.begin(); it != components.end(); ++it) {
            const auto& component = it.value();
            auto        object    = component.at("object").get<std::string>();
            if (object.empty() || object.find_first_not_of("0123456789abcdef") != std::string::npos)
                fail("invalid object path");
            if (!componentIds.count(it.key()) || !fs::is_symlink(fs::symlink_status(generationPath / it.key()))
                || fs::read_symlink(generationPath / it.key()) != fs::path("../../objects") / object)
                fail("generation member link mismatch");
            auto directory = root / "objects" / object;
            if (fs::is_symlink(fs::symlink_status(directory)) || !fs::is_directory(directory))
                fail("missing object directory");
            if (fileHashes(directory) != component.at("files"))
                fail("object missing or checksum mismatch: " + it.key());
        }
    }
    bool lowBattery() const {
        int           capacity = -1;
        std::ifstream sensor(device("/sys/class/power_supply/battery/capacity"));
        if (!(sensor >> capacity) || capacity < 0 || capacity > 100 || capacity >= 20) return false;
        auto            supplies = device("/sys/class/power_supply");
        std::error_code error;
        for (const auto& supply : fs::directory_iterator(supplies, error)) {
            auto        name = lowercase(supply.path().filename().string());
            std::string type;
            std::ifstream(supply.path() / "type") >> type;
            type = lowercase(type);
            if (type == "battery" || name == "battery") continue;
            if (type != "mains" && type.rfind("usb", 0) != 0 && name.find("usb") == std::string::npos && name != "ac")
                continue;
            int online = 0;
            std::ifstream(supply.path() / "online") >> online;
            if (online == 1) return false;
        }
        return true;
    }
    void collect() {
        auto                  value = state();
        std::set<std::string> keepGenerations, keepObjects;
        for (const char* key : {"confirmed", "previous", "pending"})
            if (value.contains(key) && value[key].is_string()) keepGenerations.insert(value[key].get<std::string>());
        if (value["trial"].is_object()) keepGenerations.insert(value["trial"].at("generation").get<std::string>());
        bool            keepAllGenerations = false;
        auto            activePath         = root / "active";
        std::error_code activeError;
        auto            activeStatus = fs::symlink_status(activePath, activeError);
        if (!activeError && fs::is_symlink(activeStatus)) {
            auto target = fs::read_symlink(activePath);
            auto id     = target.filename().string();
            if (target.parent_path() == "generations" && id.size() == 32
                && id.find_first_not_of("0123456789abcdef") == std::string::npos)
                keepGenerations.insert(id);
            else keepAllGenerations = true;
        } else if (!activeError && fs::exists(activeStatus)) keepAllGenerations = true;
        // A damaged active pointer means no generation is safe to collect.
        // Backups are recovery state and must retain their generations as well.
        try {
            auto backup = readJson(root / "state.backup.json");
            for (const char* key : {"confirmed", "previous", "pending"})
                if (backup.contains(key) && backup[key].is_string())
                    keepGenerations.insert(backup[key].get<std::string>());
            if (backup["trial"].is_object())
                keepGenerations.insert(backup["trial"].at("generation").get<std::string>());
        } catch (const std::exception&) {}
        for (const auto& id : keepGenerations) {
            try {
                auto manifest = generation(id);
                for (const auto& c : manifest.at("components").items())
                    keepObjects.insert(c.value().at("object").get<std::string>());
            } catch (const std::exception&) {
                // A damaged retained manifest cannot justify deleting any object.
                for (const auto& object : fs::directory_iterator(root / "objects"))
                    keepObjects.insert(object.path().filename().string());
            }
        }
        auto clean = [&](const char* subdir, const std::set<std::string>& keep) {
            auto directory = root / subdir;
            for (const auto& entry : fs::directory_iterator(directory))
                if (!keep.count(entry.path().filename().string())) {
                    // Targets are literal store children discovered here, never payload paths.
                    fs::remove_all(entry.path());
                }
            syncDirectory(directory);
        };
        clean("staging", {});
        if (keepAllGenerations)
            for (const auto& entry : fs::directory_iterator(root / "generations"))
                keepGenerations.insert(entry.path().filename().string());
        clean("generations", keepGenerations);
        if (keepAllGenerations)
            for (const auto& object : fs::directory_iterator(root / "objects"))
                keepObjects.insert(object.path().filename().string());
        clean("objects", keepObjects);
        std::set<std::string> health;
        if (value["trial"].is_object()) health.insert(value["trial"].at("token").get<std::string>() + ".json");
        clean("health", health);
    }
    void stage(const fs::path& descriptorPath, const fs::path& archivePath) {
        auto value = state();
        if (!value.at("trial").is_null()) fail("cannot stage during a startup trial");
        auto descriptor = readJson(descriptorPath, 65536);
        auto id         = descriptor.at("id").get<std::string>();
        if (!componentIds.count(id)) fail("unknown component identifier");
        if (id == "core" && descriptor.value("health_protocol", 0) != 1)
            fail("core package requires health_protocol 1");
        version(descriptor.at("version").get<std::string>());
        if (descriptor.value("model", std::string()) != "YDP02X"
            || descriptor.value("arch", std::string()) != "aarch64")
            fail("package model or architecture mismatch");
        if (descriptor.value("channel", std::string()) != lowercase(PENMODS_CHANNEL)) fail("package channel mismatch");
        auto size = descriptor.at("size").get<uint64_t>();
        if (!size || size > 256ULL * 1024 * 1024 || fs::file_size(archivePath) != size) fail("archive size mismatch");
        auto expandedClaim = descriptor.at("unpacked_size").get<uint64_t>();
        if (expandedClaim > 512ULL * 1024 * 1024) fail("expanded package exceeds limit");
        auto required = size * 2 + expandedClaim + 8ULL * 1024 * 1024;
        if (fs::space(root).available < required) fail("insufficient free space for package transaction");
        auto transaction = token();
        auto work        = root / "staging" / transaction;
        directories(work);
        // The validated archive is a private copy; subsequent unzip reads cannot race a downloader replacing its input.
        auto archiveCopy = work / "package.zip";
        copyFile(archivePath, archiveCopy);
        if (fs::file_size(archiveCopy) != size) fail("archive changed while copying");
        if (descriptor.contains("sha256")) {
            auto expected = descriptor["sha256"].get<std::string>();
            if (!digest(expected, 64) || sha256File(archiveCopy) != lowercase(expected))
                fail("archive SHA256 mismatch");
        } else {
            auto expected = descriptor.value("md5", std::string());
            if (id != "core" || !digest(expected, 32) || legacyMd5(archiveCopy) != lowercase(expected))
                fail("archive digest missing or MD5 mismatch");
        }
        Archive  archive(archiveCopy);
        uint64_t expanded = 0;
        for (const auto& entry : archive.entries()) {
            expanded += entry.size;
            if (entry.directory) continue;
            if (!(id == "core" && entry.name == "_do_update.sh") && !payloadPath(id, entry.name))
                fail("unexpected package member: " + entry.name);
        }
        if (!descriptor.contains("unpacked_size") || descriptor["unpacked_size"].get<uint64_t>() != expanded)
            fail("unpacked size mismatch");
        auto object  = token();
        auto payload = work / "payload";
        directories(payload);
        for (const auto& entry : archive.entries()) {
            if (entry.directory || (id == "core" && entry.name == "_do_update.sh")) continue;
            auto bytes = archive.read(entry);
            if (entry.name.find(".so") != std::string::npos || id == "mathjax") validateElf(bytes, id != "mathjax");
            if (fs::space(root).available < bytes.size() + 1024ULL * 1024)
                fail("insufficient free space while extracting");
            writeBytes(payload / entry.name, bytes.data(), bytes.size(), id == "mathjax" ? 0755 : 0644);
        }
        validateFiles(id, payload);
        Json component{
            {"version", descriptor.at("version")},
            {"object", object},
            {"requires", descriptor.value("requires", Json::object())},
            {"descriptor", descriptor},
            {"files", fileHashes(payload)}
        };
        auto base       = value["pending"].is_string() ? value["pending"].get<std::string>()
                                                       : value.at("confirmed").get<std::string>();
        auto components = generation(base).at("components");
        components[id]  = component;
        validateCombination(components);
        atomicJson(payload / "object.json", component);
        syncTree(payload);
        checked(rename(payload.c_str(), (root / "objects" / object).c_str()) == 0, "publish object");
        syncDirectory(root / "objects");
        fault("object-published");
        auto generationId   = publishGeneration(components);
        value["pending"]    = generationId;
        value["last_error"] = "";
        save(value);
        fault("pending-committed");
        // This subtree is private staging data, never an active generation or device path.
        fs::remove_all(work);
        syncDirectory(root / "staging");
    }
};

#include "Supervisor.inc"
