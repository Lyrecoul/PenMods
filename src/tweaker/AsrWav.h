// SPDX-License-Identifier: GPL-3.0-only
/*
 * Copyright (C) 2022-present, PenUniverse.
 * This file is part of the PenMods open source project.
 */

#pragma once

#include <QByteArray>
#include <QtGlobal>

/// WAV 读写的小工具（纯函数，不碰 Qt 音频/网络）。
///
/// DeepSeek 语音识别只接受 16-bit PCM WAV，所以录音那一条路必须自己拼出合规的 RIFF 头，
/// 并且能在多声道设备上把左声道抽成单声道。
namespace mod::asr {

/// 从交错 PCM 里抽出一个声道。ASR 只识别左声道，双声道源固定取 channel = 0。
QByteArray extractChannel(const QByteArray& interleaved, int channels, int channel);

/// 给裸 PCM 套一个 44 字节 RIFF 头。
QByteArray buildWav(const QByteArray& pcm, int sampleRate, int channels, int bitsPerSample = 16);

} // namespace mod::asr
