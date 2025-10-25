-- cross-compilation.lua
toolchain("cross-aarch64")
set_kind("standalone")

-- Use Zig for cross-compilation
set_toolset("cc", "zig cc")
set_toolset("cxx", "zig c++")
set_toolset("ld", "zig c++")
set_toolset("sh", "zig c++")
set_toolset("ar", "zig ar")
set_toolset("ranlib", "zig ranlib")
set_toolset("strip", "zig strip")

-- Specify target triple for aarch64 Linux
add_cxflags("--target=aarch64-linux-gnu")
add_cxxflags("--target=aarch64-linux-gnu")
add_ldflags("--target=aarch64-linux-gnu")
add_shflags("--target=aarch64-linux-gnu")

toolchain_end()
