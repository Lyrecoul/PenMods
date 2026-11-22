// SPDX-License-Identifier: GPL-3.0-only
/*
 * Copyright (C) 2022-present, PenUniverse.
 * This file is part of the PenMods open source project.
 */

#include "tweaker/AsrWav.h"

#include <QtEndian>

namespace mod::asr {

namespace {

void appendU16(QByteArray& out, quint16 value) {
    uchar buffer[2];
    qToLittleEndian<quint16>(value, buffer);
    out.append(reinterpret_cast<const char*>(buffer), 2);
}

void appendU32(QByteArray& out, quint32 value) {
    uchar buffer[4];
    qToLittleEndian<quint32>(value, buffer);
    out.append(reinterpret_cast<const char*>(buffer), 4);
}

} // namespace

QByteArray extractChannel(const QByteArray& interleaved, int channels, int channel) {
    if (channels <= 1 || channel < 0 || channel >= channels) {
        QByteArray mono = interleaved;
        if (mono.size() & 1) mono.chop(1); // 丢掉可能被截断的半个采样
        return mono;
    }

    const int  frameBytes = channels * 2;
    const int  frames     = static_cast<int>(interleaved.size() / frameBytes);
    QByteArray mono;
    mono.resize(frames * 2);
    const char* source = interleaved.constData();
    char*       target = mono.data();
    for (int frame = 0; frame < frames; ++frame) {
        const int offset      = frame * frameBytes + channel * 2;
        target[frame * 2]     = source[offset];
        target[frame * 2 + 1] = source[offset + 1];
    }
    return mono;
}

QByteArray buildWav(const QByteArray& pcm, int sampleRate, int channels, int bitsPerSample) {
    const int  blockAlign = channels * (bitsPerSample / 8);
    QByteArray out;
    out.reserve(44 + pcm.size());
    out.append(QByteArrayLiteral("RIFF"));
    appendU32(out, static_cast<quint32>(36 + pcm.size()));
    out.append(QByteArrayLiteral("WAVE"));
    out.append(QByteArrayLiteral("fmt "));
    appendU32(out, 16);                                            // fmt 块长度
    appendU16(out, 1);                                             // PCM
    appendU16(out, static_cast<quint16>(channels));                //
    appendU32(out, static_cast<quint32>(sampleRate));              //
    appendU32(out, static_cast<quint32>(sampleRate * blockAlign)); // 字节率
    appendU16(out, static_cast<quint16>(blockAlign));              //
    appendU16(out, static_cast<quint16>(bitsPerSample));           //
    out.append(QByteArrayLiteral("data"));
    appendU32(out, static_cast<quint32>(pcm.size()));
    out.append(pcm);
    return out;
}

} // namespace mod::asr
