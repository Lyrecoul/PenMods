// SPDX-License-Identifier: GPL-3.0-only
/*
 * datcheck -- drive the *vendor's own* dictionary reader (libDictManager.so)
 * against an arbitrary .dat file, so a generated container can be validated
 * with the code the pen actually runs instead of our reimplementation.
 *
 * Usage:  datcheck <file.dat> <word> [word...]
 *         DAT_OLD=1 datcheck ...        # QYdDictManager(flag=true), i.e. the
 *                                       # old (0x1044 / 点读包) layout
 *         DAT_NOZERO=1 datcheck ...     # do not pre-zero the object memory
 *
 * Build (host):  aarch64-linux-gnu-g++ -O2 -std=c++11 -o datcheck datcheck.cxx -ldl
 * Run (device):  LD_LIBRARY_PATH=/oem/YoudaoDictPen/output/libs:/userdisk/Qtlib \
 *                  /tmp/datcheck /userdisk/tiny.dat apple
 *
 * Notes:
 *  - QYdDictManager is a 0x2f8-byte object (see YQueryDictManager::open in
 *    YoudaoDictPen); ctor/open/lookUp/hasWord are exported by libDictManager.so
 *    with the libstdc++ (__cxx11) string ABI, hence the mangled lookups.
 *  - The app opens "<name>V2.dat" with CYDOfflineDictParser (not exported) and
 *    "<name>.dat" with QYdDictManager(flag=true).  Both parse the same header;
 *    QYdDictManager is used here because its ctor is actually exported.
 *  - lookUp returns a QByteArray, whose only member is an 8-byte data pointer,
 *    so the result is decoded straight out of QArrayData instead of linking Qt.
 *  - libDictManager leaves TranslatorParas/Init/updatePatch/Destroy undefined;
 *    the pen resolves those from the NMT library, so preload it here too.
 */
#include <dlfcn.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstddef>
#include <new>
#include <string>

namespace {

constexpr size_t kManagerSize = 0x2f8;  // sizeof(QYdDictManager)

const char* kSymCtor = "_ZN14QYdDictManagerC1Eb";
const char* kSymDtor = "_ZN14QYdDictManagerD1Ev";
const char* kSymOpen = "_ZN14QYdDictManager4openERKNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEE";
const char* kSymLookUp = "_ZN14QYdDictManager6lookUpERKNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEE";
const char* kSymHasWord = "_ZN14QYdDictManager7hasWordERKNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEE";

// A QByteArray is exactly one pointer; the user-declared destructor below makes
// the compiler treat it as a non-trivial return type, i.e. pass the result
// pointer in x8 (Itanium sret) exactly like the real class does.
struct RawQByteArray {
    void* d;
    ~RawQByteArray() {}
};

// Qt5 QArrayData: { int ref; int size; uint alloc:31; uint reserved:1; qptrdiff offset; }
size_t qbaSize(const RawQByteArray& b) {
    int size = 0;
    if (b.d) memcpy(&size, (const char*)b.d + 4, 4);
    return size > 0 ? (size_t)size : 0;
}

const char* qbaData(const RawQByteArray& b) {
    if (!b.d) return NULL;
    ptrdiff_t offset = 0;
    memcpy(&offset, (const char*)b.d + 16, sizeof(offset));
    return (const char*)b.d + offset;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: %s <file.dat> <word> [word...]\n", argv[0]);
        return 2;
    }

    for (const char* dep : {"libmini-nmt.so", "libynmt.so", "libdmgr.so.1", "libcomm.so.1"}) {
        if (dlopen(dep, RTLD_LAZY | RTLD_GLOBAL))
            fprintf(stderr, "preloaded %s\n", dep);
        else
            fprintf(stderr, "preload %s: %s\n", dep, dlerror());
    }

    void* lib = dlopen("libDictManager.so.1", RTLD_NOW | RTLD_GLOBAL);
    if (!lib) {
        fprintf(stderr, "dlopen(libDictManager.so.1) failed: %s\n", dlerror());
        return 3;
    }

    typedef void* (*CtorFn)(void*, bool);
    typedef bool (*OpenFn)(void*, const std::string&);
    typedef RawQByteArray (*LookUpFn)(void*, const std::string&);
    typedef bool (*HasWordFn)(void*, const std::string&);
    typedef void (*DtorFn)(void*);

    CtorFn ctor = (CtorFn)dlsym(lib, kSymCtor);
    OpenFn open = (OpenFn)dlsym(lib, kSymOpen);
    LookUpFn lookUp = (LookUpFn)dlsym(lib, kSymLookUp);
    HasWordFn hasWord = (HasWordFn)dlsym(lib, kSymHasWord);
    DtorFn dtor = (DtorFn)dlsym(lib, kSymDtor);
    if (!ctor || !open || !lookUp) {
        fprintf(stderr, "dlsym failed: %s\n", dlerror());
        return 4;
    }

    const bool oldFormat = getenv("DAT_OLD") != NULL;
    void* manager = operator new(kManagerSize);
    if (!getenv("DAT_NOZERO")) memset(manager, 0, kManagerSize);
    ctor(manager, oldFormat);

    const std::string path(argv[1]);
    if (!open(manager, path)) {
        printf("open(%s, old_format=%d) FAILED\n", path.c_str(), (int)oldFormat);
        return 5;
    }
    printf("open(%s, old_format=%d) ok\n", path.c_str(), (int)oldFormat);

    int failures = 0;
    for (int i = 2; i < argc; ++i) {
        const std::string word(argv[i]);
        const bool found = hasWord ? hasWord(manager, word) : false;
        RawQByteArray raw;
        memset(&raw, 0, sizeof(raw));
        raw = lookUp(manager, word);
        const size_t n = qbaSize(raw);
        const char* data = qbaData(raw);
        bool plausible = false;
        for (size_t k = 0; k < n; ++k) {
            char c = data[k];
            if (c == '\t') continue;  // the app strips up to the first TAB
            plausible = (c == '{' || c == '[' || (unsigned char)c >= 0x80);
            break;
        }
        printf("  hasWord=%-3s lookUp=%6zu bytes %s : %.*s%s\n", found ? "yes" : "no", n,
               (!n || plausible) ? "" : "(NOT JSON!)", (int)(n > 200 ? 200 : n),
               n ? data : "(empty)", n > 200 ? " ..." : "");
        if (n && !plausible) failures++;
    }

    if (dtor) dtor(manager);
    operator delete(manager);
    return failures ? 6 : 0;
}
