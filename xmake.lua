includes("cross-compilation.lua")

add_rules('mode.release', 'mode.debug')

--- from xmake-repo
add_requires('spdlog        1.14.1')
add_requires('elfio         3.11')
add_requires('nlohmann_json 3.11.3')
add_requires('boost         1.85.0')

--- Local library dependencies
add_requires("autoconf")

set_allowedarchs('linux|arm64-v8a')

option('qemu')
    set_default(false)
    set_showmenu(true)
    set_description('Enable build for QEMU.')

option('build-platform')
    set_default('YDP02X')
    set_showmenu(true)
    set_description('Enable build for specific devices.')
    set_values('YDP02X', 'YDPG3', 'YDP03X')

option('target-channel')
    set_default('priv')
    set_showmenu(true)
    set_description('Tweak the compilation results in release.')
    set_values('priv', 'canary', 'beta', 'stable')

-- Define LAME library target
target("lame_lib")
    set_kind("shared")
    add_includedirs("LAME-3.100/include", {public = true})
    add_includedirs("$(builddir)/LAME-3.100", {public = true}) -- For config.h

    add_files("LAME-3.100/libmp3lame/bitstream.c")
    add_files("LAME-3.100/libmp3lame/encoder.c")
    add_files("LAME-3.100/libmp3lame/fft.c")
    add_files("LAME-3.100/libmp3lame/gain_analysis.c")
    add_files("LAME-3.100/libmp3lame/id3tag.c")
    add_files("LAME-3.100/libmp3lame/lame.c")
    add_files("LAME-3.100/libmp3lame/mpglib_interface.c")
    add_files("LAME-3.100/libmp3lame/newmdct.c")
    add_files("LAME-3.100/libmp3lame/presets.c")
    add_files("LAME-3.100/libmp3lame/psymodel.c")
    add_files("LAME-3.100/libmp3lame/quantize.c")
    add_files("LAME-3.100/libmp3lame/quantize_pvt.c")
    add_files("LAME-3.100/libmp3lame/reservoir.c")
    add_files("LAME-3.100/libmp3lame/set_get.c")
    add_files("LAME-3.100/libmp3lame/tables.c")
    add_files("LAME-3.100/libmp3lame/takehiro.c")
    add_files("LAME-3.100/libmp3lame/util.c")
    add_files("LAME-3.100/libmp3lame/vbrquantize.c")
    add_files("LAME-3.100/libmp3lame/VbrTag.c")
    add_files("LAME-3.100/libmp3lame/version.c")
    -- Fix configMS.h conflicts and copy as config.h before building
    before_build(function (target)
        os.mkdir("$(builddir)/LAME-3.100")
        local config_path = "$(builddir)/LAME-3.100/config.h"
        local new_content = io.readfile("$(projectdir)/LAME-3.100/configMS.h")
        -- Replace conflicting type definitions for GCC to avoid redefinition errors
        new_content = new_content:gsub("#define int8_t signed char", "#ifndef __INT8_TYPE__\n#define int8_t signed char\n#endif")
        new_content = new_content:gsub("#define int16_t signed short", "#ifndef __INT16_TYPE__\n#define int16_t signed short\n#endif")
        new_content = new_content:gsub("#define int32_t signed int", "#ifndef __INT32_TYPE__\n#define int32_t signed int\n#endif")
        new_content = new_content:gsub("#define int64_t signed long long", "#ifndef __INT64_TYPE__\n#define int64_t signed long long\n#endif")
        new_content = new_content:gsub("#define uint8_t unsigned char", "#ifndef __UINT8_TYPE__\n#define uint8_t unsigned char\n#endif")
        new_content = new_content:gsub("#define uint16_t unsigned short", "#ifndef __UINT16_TYPE__\n#define uint16_t unsigned short\n#endif")
        new_content = new_content:gsub("#define uint32_t unsigned int", "#ifndef __UINT32_TYPE__\n#define uint32_t unsigned int\n#endif")
        new_content = new_content:gsub("#define uint64_t unsigned long long", "#ifndef __UINT64_TYPE__\n#define uint64_t unsigned long long\n#endif")

        -- Only write the file if it doesn't exist or the content has changed
        if not os.isfile(config_path) or io.readfile(config_path) ~= new_content then
            io.writefile(config_path, new_content)
        end
    end)
    add_defines("HAVE_CONFIG_H", "LAME_LIBRARY_BUILD", "STDC_HEADERS", "HAVE_ERRNO_H", "HAVE_FCNTL_H", "HAVE_LIMITS_H")
    add_cxflags("-w") -- Suppress warnings from LAME source
    set_languages('c99')
    -- Add system type-specific flags to avoid conflicts with int types
    add_defines("__int8_t_defined", "__stdint_h", "HAVE_INTTYPES_H", "HAVE_STDINT_H")

-- Define Dobby library target built from source
target("dobby_lib")
    set_kind("static")

    -- Ensure -fPIC is used when this static lib may be linked into a shared library
    add_cxxflags("-fPIC")

    add_includedirs("Dobby-0.1.2/include", {public = true})
    add_includedirs("Dobby-0.1.2", {public = true})  -- Add root Dobby directory for relative includes
    add_includedirs("Dobby-0.1.2/source", {public = true})
    add_includedirs("Dobby-0.1.2/source/dobby", {public = true})
    add_includedirs("Dobby-0.1.2/source/Backend/UserMode/ExecMemory", {public = true})
    add_includedirs("Dobby-0.1.2/source/Backend/UserMode/UnifiedInterface", {public = true})
    add_includedirs("Dobby-0.1.2/source/Backend/UserMode/PlatformUtil/Linux", {public = true})
    add_includedirs("Dobby-0.1.2/source/core", {public = true})
    add_includedirs("Dobby-0.1.2/source/core/arch", {public = true})
    add_includedirs("Dobby-0.1.2/source/core/assembler", {public = true})
    add_includedirs("Dobby-0.1.2/source/core/codegen", {public = true})
    add_includedirs("Dobby-0.1.2/source/InstructionRelocation", {public = true})
    add_includedirs("Dobby-0.1.2/builtin-plugin", {public = true})  -- Add builtin-plugin for relative includes
    add_includedirs("Dobby-0.1.2/builtin-plugin/SymbolResolver", {public = true})
    add_includedirs("Dobby-0.1.2/builtin-plugin/SymbolResolver/elf", {public = true})
    add_includedirs("Dobby-0.1.2/external/logging", {public = true})
    add_includedirs("Dobby-0.1.2/external/logging/logging", {public = true})
    add_includedirs("Dobby-0.1.2/source/InterceptRouting", {public = true})
    add_includedirs("Dobby-0.1.2/source/InterceptRouting/Routing", {public = true})
    add_includedirs("Dobby-0.1.2/source/InterceptRouting/Routing/FunctionInlineHook", {public = true})
    add_includedirs("Dobby-0.1.2/source/InterceptRouting/Routing/InstructionInstrument", {public = true})
    add_includedirs("Dobby-0.1.2/source/InterceptRouting/RoutingPlugin", {public = true})
    add_includedirs("Dobby-0.1.2/source/InterceptRouting/RoutingPlugin/NearBranchTrampoline", {public = true})
    add_includedirs("Dobby-0.1.2/source/MemoryAllocator", {public = true})
    add_includedirs("Dobby-0.1.2/source/MemoryAllocator/CodeBuffer", {public = true})
    add_includedirs("Dobby-0.1.2/source/TrampolineBridge", {public = true})
    add_includedirs("Dobby-0.1.2/source/TrampolineBridge/ClosureTrampolineBridge", {public = true})
    add_includedirs("Dobby-0.1.2/source/TrampolineBridge/Trampoline", {public = true})
    add_includedirs("Dobby-0.1.2/external", {public = true})
    
    -- Add source files
    add_files("Dobby-0.1.2/builtin-plugin/SymbolResolver/elf/dobby_symbol_resolver.cc")
    add_files("Dobby-0.1.2/external/logging/logging.cc")
    add_files("Dobby-0.1.2/source/Backend/UserMode/ExecMemory/clear-cache-tool-all.c")
    add_files("Dobby-0.1.2/source/Backend/UserMode/ExecMemory/code-patch-tool-posix.cc")
    add_files("Dobby-0.1.2/source/Backend/UserMode/PlatformUtil/Linux/ProcessRuntimeUtility.cc")
    add_files("Dobby-0.1.2/source/Backend/UserMode/UnifiedInterface/platform-posix.cc")
    add_files("Dobby-0.1.2/source/InstructionRelocation/arm/InstructionRelocationARM.cc")
    add_files("Dobby-0.1.2/source/InstructionRelocation/arm64/InstructionRelocationARM64.cc")
    add_files("Dobby-0.1.2/source/InstructionRelocation/x64/InstructionRelocationX64.cc")
    add_files("Dobby-0.1.2/source/InstructionRelocation/x86/InstructionRelocationX86.cc")
    add_files("Dobby-0.1.2/source/InstructionRelocation/x86/InstructionRelocationX86Shared.cc")
    add_files("Dobby-0.1.2/source/InstructionRelocation/x86/x86_insn_decode/x86_insn_decode.c")
    add_files("Dobby-0.1.2/source/InterceptEntry.cpp")
    add_files("Dobby-0.1.2/source/InterceptRouting/InterceptRouting.cpp")
    add_files("Dobby-0.1.2/source/InterceptRouting/Routing/FunctionInlineHook/FunctionInlineHook.cc")
    add_files("Dobby-0.1.2/source/InterceptRouting/Routing/FunctionInlineHook/RoutingImpl.cc")
    add_files("Dobby-0.1.2/source/InterceptRouting/Routing/InstructionInstrument/InstructionInstrument.cc")
    add_files("Dobby-0.1.2/source/InterceptRouting/Routing/InstructionInstrument/RoutingImpl.cc")
    add_files("Dobby-0.1.2/source/InterceptRouting/Routing/InstructionInstrument/instrument_routing_handler.cc")
    add_files("Dobby-0.1.2/source/InterceptRouting/RoutingPlugin/NearBranchTrampoline/NearBranchTrampoline.cc")
    add_files("Dobby-0.1.2/source/InterceptRouting/RoutingPlugin/NearBranchTrampoline/near_trampoline_arm64.cc")
    add_files("Dobby-0.1.2/source/InterceptRouting/RoutingPlugin/RoutingPlugin.cc")
    add_files("Dobby-0.1.2/source/Interceptor.cpp")
    add_files("Dobby-0.1.2/source/MemoryAllocator/AssemblyCodeBuilder.cc")
    add_files("Dobby-0.1.2/source/MemoryAllocator/CodeBuffer/CodeBufferBase.cc")
    add_files("Dobby-0.1.2/source/MemoryAllocator/MemoryAllocator.cc")
    add_files("Dobby-0.1.2/source/MemoryAllocator/NearMemoryAllocator.cc")
    add_files("Dobby-0.1.2/source/TrampolineBridge/ClosureTrampolineBridge/arm/ClosureTrampolineARM.cc")
    add_files("Dobby-0.1.2/source/TrampolineBridge/ClosureTrampolineBridge/arm/closure_bridge_arm.cc")
    add_files("Dobby-0.1.2/source/TrampolineBridge/ClosureTrampolineBridge/arm/helper_arm.cc")
    add_files("Dobby-0.1.2/source/TrampolineBridge/ClosureTrampolineBridge/arm64/ClosureTrampolineARM64.cc")
    add_files("Dobby-0.1.2/source/TrampolineBridge/ClosureTrampolineBridge/arm64/closure_bridge_arm64.cc")
    add_files("Dobby-0.1.2/source/TrampolineBridge/ClosureTrampolineBridge/arm64/helper_arm64.cc")
    add_files("Dobby-0.1.2/source/TrampolineBridge/ClosureTrampolineBridge/common_bridge_handler.cc")
    add_files("Dobby-0.1.2/source/TrampolineBridge/ClosureTrampolineBridge/x64/ClosureTrampolineX64.cc")
    add_files("Dobby-0.1.2/source/TrampolineBridge/ClosureTrampolineBridge/x64/closure_bridge_x64.cc")
    add_files("Dobby-0.1.2/source/TrampolineBridge/ClosureTrampolineBridge/x64/helper_x64.cc")
    add_files("Dobby-0.1.2/source/TrampolineBridge/ClosureTrampolineBridge/x86/ClosureTrampolineX86.cc")
    add_files("Dobby-0.1.2/source/TrampolineBridge/ClosureTrampolineBridge/x86/closure_bridge_x86.cc")
    add_files("Dobby-0.1.2/source/TrampolineBridge/ClosureTrampolineBridge/x86/helper_x86.cc")
    add_files("Dobby-0.1.2/source/TrampolineBridge/Trampoline/arm/trampoline_arm.cc")
    add_files("Dobby-0.1.2/source/TrampolineBridge/Trampoline/arm64/trampoline_arm64.cc")
    add_files("Dobby-0.1.2/source/TrampolineBridge/Trampoline/x64/trampoline_x64.cc")
    add_files("Dobby-0.1.2/source/TrampolineBridge/Trampoline/x86/trampoline_x86.cc")
    add_files("Dobby-0.1.2/source/core/arch/CpuFeature.cc")
    add_files("Dobby-0.1.2/source/core/arch/CpuRegister.cc")
    add_files("Dobby-0.1.2/source/core/assembler/assembler-arm.cc")
    add_files("Dobby-0.1.2/source/core/assembler/assembler-arm64.cc")
    add_files("Dobby-0.1.2/source/core/assembler/assembler-ia32.cc")
    add_files("Dobby-0.1.2/source/core/assembler/assembler-x64.cc")
    add_files("Dobby-0.1.2/source/core/assembler/assembler.cc")
    add_files("Dobby-0.1.2/source/core/codegen/codegen-arm.cc")
    add_files("Dobby-0.1.2/source/core/codegen/codegen-arm64.cc")
    add_files("Dobby-0.1.2/source/core/codegen/codegen-ia32.cc")
    add_files("Dobby-0.1.2/source/core/codegen/codegen-x64.cc")
    add_files("Dobby-0.1.2/source/dobby.cpp")

target('PenMods')
    add_rules('qt.shared')
    add_files('src/**.cpp')
    add_files('src/**.h')
    add_frameworks(
        'QtNetwork',
        'QtQuick',
        'QtQml',
        'QtGui',
        'QtMultimedia',
        'QtWebSockets',
        'QtSql')
    add_packages(
        'spdlog',
        'elfio',
        'nlohmann_json',
        'boost')
    add_deps('lame_lib', 'dobby_lib') -- Add local LAME and Dobby as dependencies
    add_includedirs(
        'src',
        'src/base',
        '$(builddir)/config',
        '$(builddir)')
    add_links( -- system
        'crypt', 
        'dl',
        'pthread')
    set_warnings('all')
    set_languages('cxx14', 'c99')
    set_exceptions('cxx')
    set_pcxxheader('src/base/Base.h')
    set_configdir('$(builddir)/config')
    add_configfiles('src/mod/Version.h.in')
    set_configvar('TARGET_CHANNEL', get_config('target-channel'))

    if is_mode('release') then
        if is_config('target-channel', 'priv') then
            set_symbols('debug')
        else
            set_symbols('hidden')
            set_optimize('smallest')
            set_strip('all')
        end
    end
    
    if is_mode('debug') then
        add_defines('DEBUG')
        set_symbols('debug')
        set_optimize('none')
    end

    if has_config('qemu') then
        add_defines('QEMU')
    end

    if is_config('build-platform', 'YDP02X') then
        add_defines('DICTPEN_YDP02X')
    end

    on_run(function(target)
        os.exec(('$(projectdir)/scripts/install.sh %s %s'):format(
            get_config('mode'),
            get_config('build-platform')))
    end)
    
target('QrcExporter')
    add_rules("qt.shared")
    add_files('resource/exporter/**.cpp')
    add_packages('spdlog')
    add_deps('dobby_lib')
    set_warnings('all')
    set_languages('cxx14', 'c99')
    set_exceptions('cxx')
