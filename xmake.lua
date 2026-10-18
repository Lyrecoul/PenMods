add_rules('mode.release', 'mode.debug')

add_requires('spdlog        1.15.3')
add_requires('elfio         3.12')
add_requires('nlohmann_json 3.12.0')
add_requires('dobby         2023.4.14')
add_requires('lame          3.100', {
    -- DictPen's buildroot exists lame v3.100,
    -- so we use it as a shared library.
    configs = {shared = true}
})
add_requires('libxcrypt     4.4.38', {
    configs = {shared = true}
})
add_requires("libwebp       1.3.0")
add_requires('librime       1.15.0')

--- options

option('qemu')
    set_default(false)
    set_showmenu(true)
    set_description('Enable build for QEMU.')
    add_defines('PL_QEMU')
option_end()

option('build-platform')
    set_default('YDP02X')
    set_showmenu(true)
    set_description('Enable build for specific devices.')
    set_values('YDP02X', 'YDPG3', 'YDP03X')
option_end()

option('target-channel')
    set_default('dev')
    set_showmenu(true)
    set_description('Tweak the compilation results in release.')
    set_values('dev', 'canary', 'beta', 'stable')
option_end()

option('force-debug-log')
    set_default(false)
    set_showmenu(true)
    set_description('Enable PL_DEBUG define even in release builds (force debug logging).')
option_end()

--- global configs

set_license('GPL-3.0-only')

set_version('2.4.1')

set_allowedarchs('linux|arm64-v8a')

-- The libstdc++ that shipped with DictPen only supports c++14, 
-- but we need more new language features.
-- The standard library combination used by PenMods:
--
--    (dynamic) glibc 2.27 + (static) libc++
--
-- !IMPORTANT! Please refer to the build guide to use the Zig toolchain
--             and specify correct triples to configure PenMods.
set_languages('cxx23', 'c11')

set_warnings('all')
set_exceptions('cxx')

set_configdir('$(builddir)/config')
add_configfiles('src/mod/Version.h.in')

if is_mode('debug') or get_config('force-debug-log') then
    add_defines('PL_DEBUG')
end

if is_mode('release') then
    set_policy('build.optimization.lto', true)
end

--- targets

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
        'dobby',
        'lame',
        -- crypt, src/helper/ServiceManager.cpp
        'libxcrypt',
        'libwebp',
        'librime')
    set_pcxxheader('src/base/Base.h')
    add_includedirs(
        'src',
        'src/base',
        '$(builddir)/config')
    add_links(
        -- dladdr, src/common/util/System.cpp
        'dl')
    
    on_config(function (target)
        target:add('defines', 'PL_BUILD_' .. get_config('build-platform'))
        target:add('defines', 'PL_' .. get_config('target-channel'):upper() .. '_CHANNEL')

        -- 内嵌 CA 证书包：resource/certs/ca-bundle.pem + ca-bundle.hashes
        --   -> $(builddir)/config/CaBundleData.h
        --
        -- 出厂 rootfs 的 /etc/ssl/certs 是空目录（GitHub issue #15），设备上没有任何 TLS 信任锚，
        -- src/helper/CaCertificates 用它兜底。数据不预压缩：原始 PEM 放进 .rodata，OTA 包体积与
        -- zlib 版几乎相同（zip 会再压一遍 .so），而运行时是 file-backed 页、可回收。
        -- CApath 只认 <subject_hash>.0 文件名，设备上没有 openssl/c_rehash，所以 hash 由
        -- scripts/update_ca_bundle.sh 在宿主侧算好，和 PEM 顺序一一对应。
        --
        -- 生成代码必须留在 target 的 on_config 里：xmake 的 target 脚本运行在独立 sandbox，
        -- xmake.lua 主 chunk 没有 `io`/`import`，主 chunk 里定义的函数拿不到这些全局。

        local srcdir = path.join(os.projectdir(), 'resource/certs')
        local pem = io.readfile(path.join(srcdir, 'ca-bundle.pem'))
        assert(pem, 'missing resource/certs/ca-bundle.pem, run scripts/update_ca_bundle.sh first')

        pem = pem:gsub('\r\n', '\n')
        assert(
            pem:find('-----BEGIN CERTIFICATE-----', 1, true),
            'resource/certs/ca-bundle.pem contains no certificate, run scripts/update_ca_bundle.sh'
        )

        local hashes = io.readfile(path.join(srcdir, 'ca-bundle.hashes'))
        assert(hashes, 'missing resource/certs/ca-bundle.hashes, run scripts/update_ca_bundle.sh')

        local hashlist = {}
        for line in (hashes .. '\n'):gmatch('([^\r\n]+)') do
            if line:match('%S') then
                table.insert(hashlist, line:match('^%s*(.-)%s*$'))
            end
        end

        local certcount, pos = 0, 1
        while true do
            local found = pem:find('-----BEGIN CERTIFICATE-----', pos, true)
            if not found then
                break
            end
            certcount = certcount + 1
            pos = found + 1
        end
        assert(
            #hashlist == certcount,
            ('resource/certs/ca-bundle.hashes has %d entries but ca-bundle.pem has %d certificates, ' ..
                're-run scripts/update_ca_bundle.sh'):format(#hashlist, certcount)
        )

        local entries = {}
        for _, hash in ipairs(hashlist) do
            table.insert(entries, ('    "%s",'):format(hash))
        end

        local content = table.concat({
            '#pragma once',
            '',
            '// Generated by xmake from resource/certs/ca-bundle.{pem,hashes} at configure time. Do not edit.',
            '// Refresh the bundle with scripts/update_ca_bundle.sh, then re-run xmake f.',
            '',
            'namespace mod::certs {',
            '',
            'inline constexpr const char kCaBundlePem[] = R"PENMODS_CA(',
        }, '\n')

        content = content ..
            pem ..
            ')PENMODS_CA";' ..
            '\n\n// 每张证书的 X509_NAME hash（CApath 的 <hash>.0 文件名），与上面的 PEM 顺序一致。\n' ..
            'inline constexpr const char* const kCaBundleHashes[] = {\n' ..
            table.concat(entries, '\n') ..
            '\n};\n\n' ..
            'inline constexpr int kCaBundleCount = ' .. certcount .. ';\n' ..
            '\n} // namespace mod::certs\n'

        local dstfile = path.join(path.absolute(import('core.project.config').builddir()), 'config', 'CaBundleData.h')
        if os.isfile(dstfile) and io.readfile(dstfile) == content then
            return
        end

        os.mkdir(path.directory(dstfile))
        io.writefile(dstfile, content)
    end)

    on_run(function(target)
        os.exec(('$(projectdir)/scripts/install.sh %s %s'):format(
            get_config('mode'),
            get_config('build-platform'))
        )
    end)

-- Externalized resource library target
target('PenModsResources')
    set_kind('shared')
    add_files('resource/externalize/**.cpp')
    add_includedirs('resource/models/YDP02X')

target('QrcExporter')
    add_rules('qt.shared')
    add_files('resource/exporter/**.cpp')
    add_packages(
        'spdlog',
        'dobby')

-- 内嵌 FFmpeg 播放器插件（Haikure/ffmpeg-player，vendored 在 external/ffmpeg-player/）
-- 独立 target，产物 qml/FFmpegPlayer/{libffmpegplayerplugin.so,qmldir} 需单独部署：
--   scripts/deploy_ffmpeg_player.sh
includes('external/ffmpeg-player')
