/* SPDX-License-Identifier: Apache-2.0 OR MIT */
/*
 * Copyright (c) 2024 Rockchip Electronics Co., Ltd.
 */

#ifndef MPP_SYS_CFG_H
#define MPP_SYS_CFG_H

#include "mpp_bit.h"
#include "mpp_list.h"
#include "mpp_frame.h"

typedef struct MppSysBaseCfg_t {
    RK_U32 enable;

    /* input args start */
    MppCodingType type;
    MppFrameFormat fmt_codec;
    RK_U32 fmt_fbc;
    RK_U32 fmt_hdr;

    /* video codec width and height */
    RK_U32 width;
    RK_U32 height;

    /* display crop info */
    RK_U32 crop_top;
    RK_U32 crop_bottom;
    RK_U32 crop_left;
    RK_U32 crop_right;

    /* bit mask for metadata and thumbnail config */
    RK_U32 has_metadata;
    RK_U32 has_thumbnail;

    /* extra protocol config */
    /* H.265 ctu size, VP9/Av1 super block size */
    RK_U32 unit_size;

    /* output args start */
    /* system support capability */
    RK_U32 cap_fbc;
    RK_U32 cap_tile;

    /* 2 horizontal stride for 2 planes like Y/UV */
    RK_U32 h_stride_by_pixel;
    RK_U32 h_stride_by_byte;
    RK_U32 v_stride;
    RK_U32 buf_total_size;

    /* fbc display offset config for some fbc version */
    RK_U32 offset_y;
    RK_U32 size_total;
    RK_U32 size_fbc_hdr;
    RK_U32 size_fbc_bdy;

    /* extra buffer size */
    RK_U32 size_metadata;
    RK_U32 size_thumbnail;
} MppSysDecBufChkCfg;

typedef struct MppSysCfgSet_t {
    MppSysDecBufChkCfg dec_buf_chk;
} MppSysCfgSet;

#ifdef __cplusplus
extern "C" {
#endif

MPP_RET mpp_sys_dec_buf_chk_proc(MppSysDecBufChkCfg *cfg);

#ifdef __cplusplus
}
#endif

#endif /* MPP_SYS_CFG_H */
