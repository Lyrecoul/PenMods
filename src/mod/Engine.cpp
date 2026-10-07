// SPDX-License-Identifier: GPL-3.0-only
/*
 * Copyright (C) 2022-present, PenUniverse.
 * This file is part of the PenMods open source project.
 */

#include "spdlog/spdlog.h"
#if PL_BUILD_YDP02X
#include "../resource/models/YDP02X/qrc_qml.h"
#endif

#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickView>

#include <QFile>
#include <dlfcn.h>

#include "base/YPointer.h"

#include "common/Event.h"

PEN_HOOK(void, _ZN22YGuiApplicationPrivate6initUiEv, QWindow** self) {

    auto& view    = *(QQuickView*)*self;
    auto* context = view.rootContext();

    mod::YPointer<QQuickView>::setInstance(&view);

    // 外部 QML 模块（如内嵌 FFmpeg 播放器插件）的部署目录，缺失时静默跳过
    const char* ExternalQmlDir = "/userdata/PenMods/qml";
    if (QFile::exists(ExternalQmlDir)) {
        view.engine()->addImportPath(ExternalQmlDir);
        spdlog::info("Added external QML import path: {}", ExternalQmlDir);
    }

    emit mod::Event::getInstance().beforeUiInitialization(view, context);

    bool                 using_external_resources = false;
    const char*          ResourceLibPath          = "/userdata/PenMods/libPenModsResources.so";
    const unsigned char* new_qt_resource_struct;
    const unsigned char* new_qt_resource_data;
    const unsigned char* new_qt_resource_name;
    if (QFile::exists(ResourceLibPath)) {
        void* lib = dlopen(ResourceLibPath, RTLD_NOW);
        if (!lib) {
            spdlog::error("Can't dlopen libPenModsResources.so");
        } else {
            using get_res_t = const unsigned char* (*)();

            auto get_struct = (get_res_t)dlsym(lib, "get_qt_resource_struct");
            auto get_data   = (get_res_t)dlsym(lib, "get_qt_resource_data");
            auto get_name   = (get_res_t)dlsym(lib, "get_qt_resource_name");

            // Q_ASSERT 在 release 构建里会被编译掉，这里显式校验并回落到内置资源
            if (!get_struct || !get_data || !get_name) {
                spdlog::error("libPenModsResources.so is missing resource symbols, falling back to built-in resources");
            } else {
                new_qt_resource_struct = get_struct();
                new_qt_resource_data   = get_data();
                new_qt_resource_name   = get_name();

                spdlog::info("Using external Qt res.");
                using_external_resources = true;
            }
        }
    }

    // Replace QResources
    PEN_CALL(void*, "_Z21qCleanupResources_qmlv")();
    bool res;
    if (using_external_resources) {
        res = PEN_CALL(bool, "_Z21qRegisterResourceDataiPKhS0_S0_", int, const uchar*, const uchar*, const uchar*)(
            0x3,
            new_qt_resource_struct,
            new_qt_resource_name,
            new_qt_resource_data
        );
    } else {
        res = PEN_CALL(bool, "_Z21qRegisterResourceDataiPKhS0_S0_", int, const uchar*, const uchar*, const uchar*)(
            0x3,
            qt_resource_struct,
            qt_resource_name,
            qt_resource_data
        );
    }
    if (res) {
        spdlog::info("Resource files have been replaced!");
    } else {
        spdlog::error("The resource file replacement failed, reload the original resource file.");
        PEN_CALL(void*, "_Z18qInitResources_qmlv")();
    }

    origin(self);
}
