-- ============================================================================
-- Haikure/ffmpeg-player —— Qt5 QML 视频播放器插件（已 vendor 进 PenMods）
--   上游   : https://github.com/Haikure/ffmpeg-player  (v1.0.0 / 48ca849, GPL-3.0)
--   改动   : external/ffmpeg-player/UPSTREAM.md
--   分析与实测: doc/FFMPEG_PLAYER_ANALYSIS.md
--   构建   : xmake build ffmpegplayerplugin
--            （产物 build/<plat>/<arch>/<mode>/qml/FFmpegPlayer/）
--   部署   : scripts/deploy_ffmpeg_player.sh
--   注意   : 硬解版本要求内核已启用 VPU（/dev/vpu_service）；出厂固件没有，
--            宿主侧有一条门控（src/mod/Mod.cpp）会在这时强制软解，
--            否则 librockchip_mpp 会在 mpp_init 里段错误并带走宿主进程。
-- ============================================================================

set_license("GPL-3.0")

local FFPLAYER_DIR = path.join(os.projectdir(), "external", "ffmpeg-player")

-- 设备侧 MPP/DRM 动态库（链接用）是否齐全：缺了就只编纯软解版本。
-- 想强制软解（即使库齐全）：PENMODS_FFMPEG_PLAYER_SW=1 xmake f -c ... && xmake build ffmpegplayerplugin
-- 这里不用 xmake option：本机 xmake 在工程加载阶段 get_config() 读不到 CLI 传进来的值。
local function has_device_libs(...)
    for _, name in ipairs({...}) do
        if not os.isfile(path.join(FFPLAYER_DIR, "external", "dictpen", name)) then
            return false
        end
    end
    return true
end
local device_libs_ok = has_device_libs("librockchip_mpp.so.1", "libdrm.so.2")
local force_sw       = os.getenv("PENMODS_FFMPEG_PLAYER_SW")
local use_rkmpp      = device_libs_ok and not (force_sw and force_sw ~= "" and force_sw ~= "0")
if not use_rkmpp then
    -- 注意：工程加载阶段没有 cprint（那是 task 作用域才有的），只能用 print
    print("ffmpeg-player: software-only variant (device libs=" .. tostring(device_libs_ok) ..
          ", PENMODS_FFMPEG_PLAYER_SW=" .. tostring(force_sw) .. ")")
end

package("libass")
    set_homepage("https://github.com/libass/libass")
    set_description("Subtitle renderer, built without a system font provider")
    set_license("ISC")
    add_urls("https://github.com/libass/libass/releases/download/$(version)/libass-$(version).tar.gz")
    add_versions("0.17.4", "a886b3b80867f437bc55cff3280a652bfa0d37b43d2aff39ddf3c4f288b8c5a8")
    local flags = {"-Os", "-fno-sanitize=all", "-ffunction-sections", "-fdata-sections"}
    add_deps("freetype 2.13.3", {configs = {shared = false, pic = true, zlib = false, cxflags = flags}})
    add_deps("fribidi v1.0.16", {configs = {shared = false, pic = true, cxflags = flags}})
    add_deps("harfbuzz 11.3.3", {configs = {shared = false, pic = true, freetype = false, glib = false, icu = false, cxflags = flags}})
    add_links("ass")
    add_syslinks("m")
    on_install("linux", function (package)
        import("package.tools.autoconf")
        local envs = autoconf.buildenvs(package)
        local pcdirs = {}
        for _, name in ipairs({"freetype", "fribidi", "harfbuzz"}) do
            table.insert(pcdirs, package:dep(name):installdir("lib/pkgconfig"))
        end
        envs.PKG_CONFIG_LIBDIR = table.concat(pcdirs, ":")
        envs.PKG_CONFIG_PATH = envs.PKG_CONFIG_LIBDIR
        local configs = {"--prefix=" .. package:installdir(), "--libdir=" .. package:installdir("lib"),
            "--disable-shared", "--enable-static", "--with-pic", "--disable-fontconfig",
            "--disable-directwrite", "--disable-coretext", "--disable-libunibreak",
            "--disable-require-system-font-provider"}
        if package:is_cross() then table.insert(configs, "--host=aarch64-linux-gnu") end
        os.vrunv("./configure", configs, {envs = envs})
        os.vrunv("make", {"-j4"}, {envs = envs})
        os.vrunv("make", {"install"}, {envs = envs})
        -- 测试程序可通过单个 pc 文件链接同一套静态库，不要求猜测包缓存路径。
        local libs = "-L" .. package:installdir("lib") .. " -lass"
        for _, dep in ipairs({{"freetype", "freetype"}, {"fribidi", "fribidi"}, {"harfbuzz", "harfbuzz"}}) do
            libs = libs .. " -L" .. package:dep(dep[1]):installdir("lib") .. " -l" .. dep[2]
        end
        io.writefile(path.join(package:installdir(), "lib", "pkgconfig", "ffplayer-ass.pc"),
            "Name: ffplayer-ass\nDescription: Static subtitle dependencies\nVersion: 0.17.4\nCflags: -I" ..
            package:installdir("include") .. "\nLibs: " .. libs .. " -lm -lpthread\n")
    end)
package_end()

add_requires("libass 0.17.4", {configs = {shared = false, pic = true,
    cxflags = {"-Os", "-fno-sanitize=all", "-ffunction-sections", "-fdata-sections"}}})

package("ffmpeg")
    set_homepage("https://www.ffmpeg.org")
    set_description("FFmpeg 3.4.8")
    set_license("GPL-3.0")

    add_urls("https://ffmpeg.org/releases/ffmpeg-$(version).tar.bz2", {alias = "home"})

    add_versions("home:3.4.8", "904ddc5276ab605dd430c2981da55a2ceb67087eb186ce1bf47336a1a42719b4")
    -- OpenSSL 3 使用 Apache-2.0，与本项目 GPLv3 兼容；0003 更新旧版检测。
    -- OpenSSL 配方通过 make CFLAGS 覆盖上游优化参数，必须显式指定发布优化。
    -- Zig 未优化构建会默认插入 UBSan 检查，静态链接后产生大量代码和重定位数据。
    local release_cflags = {"-Os", "-fno-sanitize=all", "-ffunction-sections", "-fdata-sections"}
    add_deps("openssl3 3.5.6", {configs = {shared = false, pic = true, cxflags = release_cflags}})
    add_deps("libxml2 v2.13.4", {configs = {shared = false, pic = true, cxflags = release_cflags}})

    add_links("avfilter", "avdevice", "avformat", "avcodec", "swscale", "swresample", "avutil","postproc")
    if is_plat("linux") then add_syslinks("dl", "pthread", "m") end

    -- Rockchip MPP 硬解（h264/hevc/vp8_rkmpp；RK3326 的 VPU 不支持 VP9）
    -- 依赖全部放在项目的 external/ 下：
    --   external/rkmpp/include      mpp 与 libdrm 头文件（交叉编译版，与设备 ABI 兼容）
    --   external/rkmpp/pkgconfig    rockchip_mpp.pc / libdrm.pc（相对路径）
    --   external/dictpen            从设备上拉取的 .so（librockchip_mpp.so.1、libdrm.so.2 …）
    -- 设备库缺失时自动退回纯软解构建，播放器运行时同样会走软解。
    -- 需要较新的 MPP（已移除 MPP_DEC_GET_FREE_PACKET_SLOT_COUNT），见 patches/0002。
    add_configs("rkmpp", {description = "Enable Rockchip MPP hardware decoders.", default = false, type = "boolean"})
    add_configs("network_revision", {description = "Patched FFmpeg recipe revision (invalidates old builds).", default = 7, type = "number"})

    on_install("linux", "macosx", "android", "iphoneos", function (package)
        import("package.tools.autoconf")
        local envs = autoconf.buildenvs(package)
        
        -- 按文件名顺序应用所有补丁（0001 binutils 兼容，0002 rkmppdec 适配新版 MPP）
        local patches = os.files(path.join(FFPLAYER_DIR, "patches", "*.patch"))
        table.sort(patches)
        for _, patchfile in ipairs(patches) do
            os.runv("patch", {"-p1", "-i", patchfile})
        end

        -- 1. 修复现代 glibc 缺失 sys/sysctl.h 问题
        if os.isfile("libavutil/cpu.c") then
            io.replace("libavutil/cpu.c", "#include <sys/sysctl.h>", "// #include <sys/sysctl.h>", {plain = true})
        end

        -- 2. 编译器环境准备 (Zig/AArch64)
        local cc = package:build_envs().CC or package:tool("cc")
        local cxx = package:build_envs().CXX or package:tool("cxx")
        local strip = "strip"
        local target_str = ""
        if cc:match("zig") then
            local arch = package:arch()
            if arch:match("arm64") or arch:match("aarch64") then
                target_str = "aarch64-linux-gnu.2.27"
                strip = "aarch64-linux-gnu-strip"
            else
                target_str = arch .. "-linux-gnu"
            end
        end

        -- 3. rkmpp：由 add_requires 根据 external/ 中的设备库是否齐全决定
        local rkmpp_pcdir = nil
        if package:config("rkmpp") and package:is_plat("linux") and package:is_arch("arm64", "arm64-v8a", "aarch64") then
            rkmpp_pcdir = path.join(FFPLAYER_DIR, "external", "rkmpp", "pkgconfig")
        else
            cprint("${color.warning}rkmpp disabled (needs linux/arm64 and external/dictpen/librockchip_mpp.so.1 + libdrm.so.2), building software-only FFmpeg")
        end

        local decoders = "h264,hevc,vp8,vp9,mpeg4,mpeg2video,mpeg1video,vc1,wmv3,rv40,mjpeg,aac,mp3,opus,flac,vorbis,ac3,eac3,dca,ass,mov_text,srt,webvtt,pgssub"
        if rkmpp_pcdir then
            decoders = decoders .. ",h264_rkmpp,hevc_rkmpp,vp8_rkmpp"
        end

        -- 4. 构建配置参数
        local configs = {
            "--prefix=" .. package:installdir(),
            "--enable-version3",
            "--disable-doc",
            "--disable-debug",
            "--disable-everything",
            "--disable-programs",
            "--disable-shared",
            "--enable-static",
            "--enable-pic",
            "--disable-autodetect",
            "--enable-gpl",
            "--enable-openssl",
            "--enable-libxml2",
            "--enable-asm",
            "--enable-neon",
            "--enable-postproc",
            -- 解码器/格式支持
            "--enable-decoder=" .. decoders,
            "--enable-demuxer=mov,dash,matroska,flv,mpegts,mpegps,avi,asf,rm,mp3,ogg,wav,aac,flac,mjpeg,mpegvideo,h264,hevc,image2",
            "--enable-muxer=mp4,mov,matroska,flv,mpegts,mpegps,avi,asf,mp3,ogg,wav,image2",
            "--enable-parser=h264,hevc,vp8,vp9,mpeg4video,mpeg1video,mpeg2video,vc1,aac,opus,flac,vorbis,ac3,dca,mpegaudio",
            -- rkmpp 需要 Annex-B 码流（decoder 内部自动挂 bsf）
            "--enable-bsf=h264_mp4toannexb,hevc_mp4toannexb",
            "--enable-protocol=file,http,https,tls_openssl,tcp,udp,rtp,rtmp,rtmps,concat,data,pipe",
            "--enable-filter=scale,overlay,pad,crop,yadif,setpts,atempo,abuffer,abuffersink,aformat,aresample,fps,format,transpose",
        }

        -- 如果是 AArch64，最好指定 CPU 类型以优化汇编
        if package:arch():match("aarch64") then
            table.insert(configs, "--cpu=generic") -- 或者特定的 cortex-a72 等
        end

        -- strip：优先 PATH，其次 ~/build/zig-toolchain 包装器，都没有就跳过 strip
        import("lib.detect.find_program")
        local strip_program = find_program(strip, {paths = {path.join(os.getenv("HOME") or "", "build", "zig-toolchain")}})
        if strip_program then
            table.insert(configs, "--strip=" .. strip_program)
        else
            table.insert(configs, "--disable-stripping")
        end

        -- 5. 交叉编译处理（如果你有 sysroot，可追加 --sysroot=）
        if package:is_cross() then
            table.insert(configs, "--enable-cross-compile")
            local arch = package:targetarch()
            if arch:match("arm64") or arch:match("aarch64") then arch = "aarch64" end
            table.insert(configs, "--arch=" .. arch)
            table.insert(configs, "--target-os=linux")
        end

        -- 6. 编译器路径注入
        if target_str ~= "" then
            table.insert(configs, "--cc=" .. cc .. " -target " .. target_str)
            table.insert(configs, "--cxx=" .. cxx .. " -target " .. target_str)
        else
            table.insert(configs, "--cc=" .. cc)
            table.insert(configs, "--cxx=" .. cxx)
        end

        -- 7. 注入容错参数
        local cflags = "-Wno-incompatible-function-pointer-types -fno-sanitize=all -ffunction-sections -fdata-sections"
        local ldflags = "-Wl,--undefined-version -Wl,-z,defs"
        local dependency_pcdirs = {}
        for _, name in ipairs({"openssl3", "libxml2"}) do
            local dep = package:dep(name)
            table.insert(dependency_pcdirs, dep:installdir("lib/pkgconfig"))
            cflags = cflags .. " -I" .. dep:installdir("include")
            ldflags = ldflags .. " -L" .. dep:installdir("lib")
        end
        if target_str ~= "" then
            cflags = cflags .. " -target " .. target_str
            ldflags = ldflags .. " -target " .. target_str
        end

        -- 8. rkmpp 依赖：pkg-config 只在 external/rkmpp 内查找，避免误用宿主机的 libdrm
        if rkmpp_pcdir then
            table.insert(configs, "--enable-libdrm")
            table.insert(configs, "--enable-rkmpp")
            table.insert(dependency_pcdirs, rkmpp_pcdir)
        end
        envs.PKG_CONFIG_LIBDIR = table.concat(dependency_pcdirs, ":")
        envs.PKG_CONFIG_PATH = envs.PKG_CONFIG_LIBDIR

        table.insert(configs, "--extra-cflags=" .. cflags)
        table.insert(configs, "--extra-ldflags=" .. ldflags)

        -- 9. 执行安装
        os.vrunv("./configure", configs, {envs = envs})
        local enabled = io.readfile("config.h")
        for _, feature in ipairs({"HTTPS_PROTOCOL", "TLS_OPENSSL_PROTOCOL", "DASH_DEMUXER", "ATEMPO_FILTER", "AFORMAT_FILTER", "ARESAMPLE_FILTER"}) do
            assert(enabled:find("#define CONFIG_" .. feature .. " 1", 1, true), "FFmpeg feature missing: " .. feature)
        end
        os.vrunv("make", {"-j" .. os.default_njob()})
        os.vrunv("make", {"install"})
    end)
package_end()

add_requires("ffmpeg        3.4.8", {
    configs = {
        shared = false,
        pic = true,
        -- 该字段参与包哈希：改动它即可强制重编 ffmpeg 包。
        -- 不要把 has_device_libs() 的返回值直接喂给 rkmpp：本机出现过同一份工程
        -- 解析出 rkmpp=false 的旧变体并被链接、导致产物没有硬解的情况，
        -- 见 doc/FFMPEG_PLAYER_ANALYSIS.md §8.5。
        network_revision = 8,
        rkmpp = use_rkmpp,
    }
})

target("ffmpegplayerplugin")
    add_rules('qt.shared')
    -- 目标级编译设置：根工程是 c++23 / LTO / 无错误警告，本插件保持上游的 c++17
    set_languages("c11", "c++17")
    set_optimize("fastest")
    set_warnings("all", "error")
    -- 根工程的 PCH（src/base/Base.h）不适用于本插件
    set_pcxxheader()
    add_files(path.join(FFPLAYER_DIR, "src", "*.cpp"))
    add_files(path.join(FFPLAYER_DIR, "src", "FFmpegPlayerPlugin.hpp"),
              path.join(FFPLAYER_DIR, "src", "FFmpegVideoPlayerQml.hpp"))
    add_headerfiles(path.join(FFPLAYER_DIR, "src", "*.hpp"))
    add_packages("ffmpeg", "libass")
    add_frameworks(
        'QtNetwork',
        'QtQuick',
        'QtQml',
        'QtGui')
    add_includedirs(path.join(FFPLAYER_DIR, "external", "alsa-lib-1.1.5", "include"))
    add_linkdirs(path.join(FFPLAYER_DIR, "external", "dictpen"))
    add_links("asound")
    if use_rkmpp and is_plat("linux") and is_arch("arm64", "arm64-v8a", "aarch64") then
        add_links("rockchip_mpp", "drm")
    end
    -- 将静态依赖的符号设为本地，宿主已加载的旧 FFmpeg/OpenSSL 不能抢占它们。
    add_shflags("-Wl,--version-script=" .. path.join(FFPLAYER_DIR, "src", "ffmpegplayer.exports"),
                "-Wl,-z,defs", "-Wl,--gc-sections", {force = true})
    add_cxflags("-ffunction-sections", "-fdata-sections", {force = true})
    if is_mode("release") then add_shflags("-s", {force = true}) end
    on_load(function (target)
        target:data_set("linkdepfiles", {path.join(FFPLAYER_DIR, "src", "ffmpegplayer.exports")})
        -- qt.moc 生成的 moc 单元在重建时可能拿不到 add_packages 的 include 目录，
        -- 显式补一遍（否则 moc_FFmpegVideoPlayerQml.cpp 找不到 libavcodec/avcodec.h）
        for _, name in ipairs({"ffmpeg", "libass"}) do
            local pkg = target:pkgs()[name]
            if pkg then
                target:add("includedirs", pkg:installdir("include"))
            end
        end
    end)
    before_build(function (target)
        import("core.project.depend")
        -- Xmake qt.moc 的缓存只记录被 moc 的头文件，遗漏其包含的播放器实现。
        -- 任一实现头变化时重建两个 moc 对象，避免旧类布局与新代码混合链接。
        depend.on_changed(function ()
            for _, name in ipairs({"FFmpegPlayerPlugin", "FFmpegVideoPlayerQml"}) do
                local generated = target:autogenfile(path.join("src", "moc_" .. name .. ".cpp"))
                os.tryrm(target:objectfile(generated))
            end
        end, {files = os.files(path.join(os.projectdir(), "src", "*.hpp")),
              dependfile = target:dependfile("ffplayer-moc-headers")})
    end)
    -- libavcodec 依赖设备上的 librockchip_mpp/libdrm，链接时从这里解析
    add_ldflags("-Wl,-rpath-link," .. path.join(FFPLAYER_DIR, "external", "dictpen"), {force = true})
    after_build(function (target)
        local module_dir = path.join(target:targetdir(), "qml", "FFmpegPlayer")
        os.mkdir(module_dir)
        os.cp(target:targetfile(), module_dir)
        os.cp(path.join(FFPLAYER_DIR, "src", "qml", "*"), module_dir)
    end)
    on_install(function (target)
        local module_dir = path.join(target:installdir(), "qml", "FFmpegPlayer")
        os.mkdir(module_dir)
        os.cp(target:targetfile(), module_dir)
        os.cp(path.join(FFPLAYER_DIR, "src", "qml", "*"), module_dir)
    end)
    if is_plat("linux") then
        add_syslinks("pthread", "dl", "rt")
    end
