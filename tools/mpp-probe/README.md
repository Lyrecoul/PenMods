# mpp-probe

RK3326 词典笔上检查 Rockchip MPP（硬解）到底能不能用的小探针，配合
[`doc/FFMPEG_PLAYER_ANALYSIS.md`](../../doc/FFMPEG_PLAYER_ANALYSIS.md)。

它一次打印四类事实：

1. `/dev/vpu_service` / `/dev/mpp_service` / `/dev/ion` / `/dev/dma_heap` / `/dev/dri/card0` 是否存在；
2. `mpp_check_support_format()` 对 H.264 / HEVC / VP8 的返回值；
3. `mpp_buffer_group_get_internal()` + `mpp_buffer_get()`（ION 与 DRM 两种类型）能否成功 ——
   与 VPU 无关，用来确认分配器链路（ION 缺失时应回落到 DRM）；
4. `mpp_create()` + `mpp_init(MPP_CTX_DEC, H264)` —— VPU 可用与否的分水岭，**放最后**：
   设备树 `vpu_combo` 被禁用时这里会在 `hal_h264d_init` 断言之后段错误。

## 构建

需要 MPP 头文件（上游 ffmpeg-player 的 `external/rkmpp/include`）和设备 MPP 库
（同仓库 `external/dictpen/librockchip_mpp.so.0`，与设备 `/usr/lib/librockchip_mpp.so.0` 字节一致）。
注意：这两个 `.so` 在 git 里是符号链接，用 zip 下载会变成 20 字节的文本文件，所以**按路径链接 `.so.0`**：

```sh
zig cc -target aarch64-linux-gnu.2.27 -O2 \
  -I<ffmpeg-player>/external/rkmpp/include \
  -o mpp-probe probe.c <ffmpeg-player>/external/dictpen/librockchip_mpp.so.0
adb push mpp-probe /tmp/ && adb shell 'chmod +x /tmp/mpp-probe'
adb shell 'LD_LIBRARY_PATH=/usr/lib /tmp/mpp-probe; echo exit=$?'
```

`LD_LIBRARY_PATH=/userdisk/mpv/lib` 可以顺便对比 mpv 自带的新版 MPP 库（那份 `mpp_init` 是优雅报错，不崩）。

## 刷入 VPU 补丁后的实测结果（同一台设备，2026-10-07）

```
node /dev/vpu_service   present          <- 设备树 vpu_combo.status = okay
node /dev/hevc_service  present
node /dev/mpp_service   MISSING
node /dev/ion           MISSING
node /dev/dma_heap      MISSING
node /dev/dri/card0     present
check_support h264=0 hevc=0 vp8=0
mpp_rt: NOT found ion allocator
mpp_rt: found drm allocator
buffer_group/buffer_get/put [ION] [DRM] 全部 OK
mpp_create ret=0
mpp_init(dec,h264) ret=0 OK               <- 不再段错误，硬解可用
=== done ===
exit=0
```

## 出厂固件（VPU 被设备树禁用）上的实测结果

```
=== MPP probe ===
node /dev/vpu_service   MISSING          <- 设备树 vpu_combo status = disabled
node /dev/hevc_service  MISSING
node /dev/mpp_service   MISSING
node /dev/ion           MISSING          <- /proc/kallsyms 里 ion_ 符号数为 0，压根没编 ION
node /dev/dma_heap      MISSING
node /dev/dri/card0     present
check_support h264=0 hevc=0 vp8=0 (0 == 支持)   <- 只看 SoC 支持表，所以"预检通过"
mpp_rt: NOT found ion allocator
mpp_rt: found drm allocator             <- 分配器自动回落，下面都成功
buffer_group[ION] ret=0 OK
buffer_get[ION]   ret=0 fd=5
buffer_put[ION]   OK
buffer_group_put[ION] OK
buffer_group[DRM] ret=0 OK
buffer_get[DRM]   ret=0 fd=5
buffer_put[DRM]   OK
buffer_group_put[DRM] OK
mpi: mpp version: Without VCS info
mpp_create ret=0
hal_h264d_api: Assertion vcodec_type & ((0x00000200) | (0x00000001) | (0x00000002)) failed at hal_h264d_init:104
hal_h264d_api: hal_h264d_init hard mode error, value=0
hal_h264d_api: Assertion 0 failed at hal_h264d_init:154
mpp_device: mpp_device_init failed to find device for coding 7 type 0
Segmentation fault                       <- 退出码 139
```

要点：

- **能力预检不代表可用**：`mpp_check_support_format()` 返回 0，但 `mpp_init()` 直接段错误。
  进程内的播放器（FFmpeg 的 `rkmppdec`、gst 的 `mppvideodec`）都会走到这一步，
  “`avcodec_open2` 失败就回落软解”根本来不及执行 → 宿主进程一起挂。
  所以硬解尝试必须先看 `/dev/vpu_service` 是否存在。
- **内核侧不需要 ION**：DT 子节点 `allocator = <1>` 在 `vcodec_service.c` 里映射为 `drm`，
  MPP 运行时也会自己从 ION 回落到 DRM（`/dev/dri/card0`），缓冲区申请/释放都正常。
- 打开 VPU（DTB `vpu_combo.status = "okay"`）后，这里的 `mpp_init` 应返回 0 且不再崩。
  注意 `of_device_is_available()` 只认 `okay`/`ok`，写成 `okey` 等于没改。
