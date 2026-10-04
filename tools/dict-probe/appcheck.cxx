// SPDX-License-Identifier: GPL-3.0-only
/*
 * appcheck -- validate a .dat container through the *same* code path the pen
 * uses: YQueryDictManager (libDictManager.so), which inserts "V2" before the
 * file extension, prefers "<name>V2.dat" over "<name>.dat", and drives
 * CYDOfflineDictParser for the former and QYdDictManager for the latter.
 *
 * Usage:
 *   ./appcheck /userdisk/dictprobe/localdict/tiny.dat apple zebra 中文词
 *   (argv[1] is the path the engine builds itself: $APP_ROOT_PATH/localdict/<name>.dat)
 *
 * Build (needs the pen's Qt 5.15.2, like PenMods itself):
 *   QT=$HOME/PenMods/aarch64-linux-qt-5.15.2
 *   aarch64-linux-gnu-g++ -O2 -std=c++11 -fPIC -I$QT/include -I$QT/include/QtCore \
 *     -o appcheck appcheck.cxx -ldl -L$QT/lib -lQt5Core -Wl,--allow-shlib-undefined
 *
 * Run (device):
 *   LD_LIBRARY_PATH=/oem/YoudaoDictPen/output/libs:/userdisk/Qtlib \
 *     ./appcheck /userdisk/dictprobe/localdict/tiny.dat apple
 *
 * Notes:
 *  - The pen keeps the object inside a std::_Sp_counted_ptr_inplace<YQueryDictManager>
 *    control block: {vptr; use_count; weak_count; storage}, so the class itself
 *    starts at +16 with 16 bytes of state (see YDictQueryEnginePrivate::loadDict).
 *  - libDictManager leaves TranslatorParas/Init/updatePatch/Destroy undefined and
 *    resolves them from the NMT library, so preload it here as well.
 */
#include <dlfcn.h>
#include <unistd.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include <QtCore/QCoreApplication>
#include <QtCore/QFile>
#include <QtCore/QString>

namespace {

constexpr size_t kQueryDictManagerOffset = 0x10;  // counted-ptr control block header

const char* kSymCtor = "_ZN17YQueryDictManagerC1Ev";
const char* kSymOpen = "_ZN17YQueryDictManager4openERK7QString";
const char* kSymLookUp = "_ZN17YQueryDictManager6lookUpERK7QString";
const char* kSymHasWord = "_ZN17YQueryDictManager7hasWordERK7QString";

std::string utf8(const QString& s) {
    const QByteArray b = s.toUtf8();
    return std::string(b.constData(), (size_t)b.size());
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: %s <path.dat> <word> [word...]\n", argv[0]);
        return 2;
    }

    QCoreApplication qt(argc, argv);

    for (const char* dep : {"libmini-nmt.so", "libynmt.so", "libdmgr.so.1", "libcomm.so.1"})
        dlopen(dep, RTLD_LAZY | RTLD_GLOBAL);

    void* lib = dlopen("libDictManager.so.1", RTLD_NOW | RTLD_GLOBAL);
    if (!lib) {
        fprintf(stderr, "dlopen(libDictManager.so.1) failed: %s\n", dlerror());
        return 3;
    }

    typedef void* (*CtorFn)(void*);
    typedef bool (*OpenFn)(void*, const QString&);
    typedef QString (*LookUpFn)(void*, const QString&);
    typedef bool (*HasWordFn)(void*, const QString&);

    CtorFn ctor = (CtorFn)dlsym(lib, kSymCtor);
    OpenFn open = (OpenFn)dlsym(lib, kSymOpen);
    LookUpFn lookUp = (LookUpFn)dlsym(lib, kSymLookUp);
    HasWordFn hasWord = (HasWordFn)dlsym(lib, kSymHasWord);
    if (!ctor || !open || !lookUp) {
        fprintf(stderr, "dlsym failed: %s\n", dlerror());
        return 4;
    }

    static unsigned char block[0x20];
    memset(block, 0, sizeof(block));
    *(int*)(block + 8) = 1;   // use_count
    *(int*)(block + 12) = 1;  // weak_count
    void* manager = block + kQueryDictManagerOffset;
    ctor(manager);

    const QString path = QString::fromUtf8(argv[1]);
    QString v2path = path;
    v2path.insert(v2path.size() - 4, QStringLiteral("V2"));
    fprintf(stderr, "looking for %s (exists=%d), fallback exists=%d\n", v2path.toUtf8().constData(),
            (int)QFile::exists(v2path), (int)QFile::exists(path));

    const bool opened = open(manager, path);
    printf("YQueryDictManager::open(%s) -> %d\n", argv[1], (int)opened);

    int failures = 0;
    for (int i = 2; i < argc; ++i) {
        const QString word = QString::fromUtf8(argv[i]);
        const bool found = hasWord ? hasWord(manager, word) : false;
        const std::string json = utf8(lookUp(manager, word));
        bool plausible = !json.empty();
        if (plausible) {
            const char c = json[json.find_first_not_of('\t')];
            plausible = (c == '{' || c == '[');
        }
        printf("  hasWord=%-3s lookUp=%6zu chars %s : %.200s%s\n", found ? "yes" : "no", json.size(),
               json.empty() ? "" : (plausible ? "" : "(NOT JSON!)"), json.c_str(),
               json.size() > 200 ? " ..." : "");
        if (!json.empty() && !plausible) failures++;
    }
    fflush(stdout);
    // The vendor library is not meant to be dlclose()d/unwound from a foreign
    // process; skip its static destructors to keep the output clean.
    _exit(failures ? 6 : 0);
}
