// SPDX-License-Identifier: GPL-3.0-only
/*
 * Rockchip MPP 可用性探针 —— 配合 doc/FFMPEG_PLAYER_ANALYSIS.md
 *
 * 目的: 在设备上区分三件事,避免"硬解崩了宿主"式的误判
 *   1. MPP 能力预检 (mpp_check_support_format) 是否通过 —— 它只看 SoC 支持表,不查内核设备
 *   2. mpp_init() 是否真的能拿到 /dev/vpu_service —— 设备树 vpu_combo 被禁用时这里会崩
 *   3. buf_group/buf 的分配器链路是否正常 (ION 缺失时应自动回落到 DRM)
 *
 * 构建 (仓库外即可,需要 external/rkmpp 头文件和设备 MPP 库;库文件是 git 符号链接,zip 下载后
 * 会变成文本文件,所以直接按路径链接 .so.0):
 *
 *   zig cc -target aarch64-linux-gnu.2.27 -O2 \
 *     -I<ffmpeg-player>/external/rkmpp/include \
 *     -o mpp-probe probe.c <ffmpeg-player>/external/dictpen/librockchip_mpp.so.0
 *
 * 运行 (设备上):
 *   adb push mpp-probe /tmp/ && adb shell 'chmod +x /tmp/mpp-probe'
 *   adb shell 'LD_LIBRARY_PATH=/usr/lib /tmp/mpp-probe; echo exit=$?'
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "rockchip/mpp_buffer.h"
#include "rockchip/mpp_err.h"
#include "rockchip/rk_mpi.h"

static void check_devices(void) {
    const char *nodes[] = {"/dev/vpu_service", "/dev/hevc_service", "/dev/mpp_service", "/dev/ion",
                           "/dev/dma_heap",    "/dev/dri/card0",    NULL};
    for (int i = 0; nodes[i]; ++i)
        printf("node %-18s %s\n", nodes[i], access(nodes[i], F_OK) == 0 ? "present" : "MISSING");
}

static void try_group(const char *name, MppBufferType type) {
    MppBufferGroup group = NULL;
    MPP_RET ret          = mpp_buffer_group_get_internal(&group, type);
    printf("buffer_group[%s] ret=%d %s\n", name, ret, ret ? "FAIL" : "OK");
    if (ret != MPP_OK) return;

    MppBuffer buffer = NULL;
    ret              = mpp_buffer_get(group, &buffer, 1280 * 720 * 3 / 2);
    printf("buffer_get[%s]   ret=%d fd=%d\n", name, ret, buffer ? mpp_buffer_get_fd(buffer) : -1);
    if (buffer) {
        mpp_buffer_put(buffer);
        printf("buffer_put[%s]   OK\n", name);
    }
    mpp_buffer_group_put(group);
    printf("buffer_group_put[%s] OK\n", name);
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);

    printf("=== MPP probe ===\n");
    check_devices();

    printf("check_support h264=%d hevc=%d vp8=%d (0 == 支持)\n",
           mpp_check_support_format(MPP_CTX_DEC, MPP_VIDEO_CodingAVC),
           mpp_check_support_format(MPP_CTX_DEC, MPP_VIDEO_CodingHEVC),
           mpp_check_support_format(MPP_CTX_DEC, MPP_VIDEO_CodingVP8));

    /* 分配器链路与 VPU 无关,先测它:ION 缺失时 MPP 会打印 found drm allocator */
    try_group("ION", MPP_BUFFER_TYPE_ION);
    try_group("DRM", MPP_BUFFER_TYPE_DRM);

    /* 这一步在 VPU 被设备树禁用时会断言后段错误(整个进程挂掉),所以放最后 */
    MppCtx ctx = NULL;
    MppApi *mpi = NULL;
    MPP_RET ret = mpp_create(&ctx, &mpi);
    printf("mpp_create ret=%d\n", ret);
    if (ret == MPP_OK) {
        ret = mpp_init(ctx, MPP_CTX_DEC, MPP_VIDEO_CodingAVC);
        printf("mpp_init(dec,h264) ret=%d %s\n", ret, ret ? "FAIL" : "OK");
        if (ret == MPP_OK && mpi && mpi->reset) mpi->reset(ctx);
        mpp_destroy(ctx);
    }

    printf("=== done ===\n");
    return 0;
}
