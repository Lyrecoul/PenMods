// SPDX-License-Identifier: GPL-3.0-only
/*
 * Copyright (C) 2022-present, PenUniverse.
 * This file is part of the PenMods open source project.
 */

#pragma once

/**
 * @brief PenMods Plugin SDK
 *
 * 这是为外部插件开发者提供的 SDK
 * 将此文件下载到你的插件项目中使用
 *
 * 使用方法：
 *   在你的插件项目中 #include "PluginSDK.h"
 *
 *   extern "C" {
 *       void init_plugin() {
 *           // 可选的基础初始化
 *       }
 *
 *       void init_plugin_with_hook_api(PluginHookAPI* hook_api) {
 *           // 设置全局 Hook API 指针
 *           g_hook_api = hook_api;
 *
 *           // 使用 hook_api->querySymbol 和 hook_api->hookFunction
 *           // 来创建你自己的 Hook
 *       }
 *   }
 */

#include <cstdint>

/**
 * @brief Hook API 接口 - 供外部插件使用
 *
 * PluginManager 会在调用插件的 init_plugin_with_hook_api 时注入此接口
 * 插件可以通过这个接口查询符号地址和注册 Hook
 */
typedef struct {
    /**
     * @brief 查询符号地址
     * @param symbolName 符号名称（C++ mangled name）
     * @return 符号地址，失败返回 NULL
     *
     * 示例：
     *   void* addr = hook_api->querySymbol("_ZN11YSystemBase8ocrStartEv");
     */
    void* (*querySymbol)(const char* symbolName);

    /**
     * @brief Hook 一个函数
     * @param targetAddr 目标函数的地址（由 querySymbol 获取）
     * @param detourFunc Detour 函数指针（你自定义的函数）
     * @param originalFunc 输出参数，接收原始函数指针的地址
     * @return 0 表示成功，非 0 表示失败
     *
     * 示例：
     *   typedef uint64_t (*OCRStartFunc)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
     *   OCRStartFunc original_ocrStart = NULL;
     *
     *   uint64_t detour_ocrStart(uint64_t self, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) {
     *       printf("OCR starting!\\n");
     *       return original_ocrStart(self, a2, a3, a4, a5);
     *   }
     *
     *   hook_api->hookFunction(addr, (void*)detour_ocrStart, (void**)&original_ocrStart);
     */
    int (*hookFunction)(void* targetAddr, void* detourFunc, void** originalFunc);
} PluginHookAPI;

// ==================== Media API ====================

/**
 * @brief 媒体会话 ABI 版本，宿主填入 PluginMediaAPI::abiVersion
 */
#define PLUGIN_MEDIA_ABI_VERSION 1u

/**
 * @brief 播放状态，取值与 MediaSession::PlayState 一致
 */
typedef enum PluginMediaPlayState {
    PLUGIN_MEDIA_STOPPED = 0,
    PLUGIN_MEDIA_PLAYING = 1,
    PLUGIN_MEDIA_PAUSED  = 2,
} PluginMediaPlayState;

/**
 * @brief 控制事件回调
 *
 * 面板上的按钮会触发这些回调，全部在 UI 线程调用，回调里不要做阻塞操作。
 * structSize 必须填 sizeof(PluginMediaCallbacks)。
 */
typedef struct PluginMediaCallbacks {
    uint32_t structSize;
    void (*onPlay)(void* user);
    void (*onPause)(void* user);
    void (*onToggle)(void* user);
    void (*onNext)(void* user);
    void (*onPrev)(void* user);
    void (*onStop)(void* user);
    void (*onSeek)(void* user, int64_t positionMs);
    void (*onOpen)(void* user);
    /**
     * @brief 会话被其它插件接管（可选，可留 NULL）
     *
     * 别的插件调用 beginSession() 抢走会话时，宿主回调这里通知你。
     * 收到后应停止自己的播放：此时你的 handle 已经失效，后续
     * setTrack/setPlayState/endSession 都不会生效，也不需要再调 endSession()。
     *
     * 该字段是按 structSize 兼容追加的，旧宿主不认识它（不会被调用），
     * 留 NULL 也不影响其它回调。
     */
    void (*onSessionRevoked)(void* user);
} PluginMediaCallbacks;

/**
 * @brief 媒体会话接口 - 供外部插件使用
 *
 * PluginManager 会在调用插件的 init_plugin_with_media_api 时注入此接口。
 * 插件把自己的播放内容上报到这里，系统下拉面板的音乐控制区就会显示并控制它；
 * 插件停止时调用 endSession()，面板会自动回落宿主播放器。
 *
 * 所有函数都可以从任意线程调用，宿主会自行 marshal 到 UI 线程。
 *
 * 示例：
 *   static void* g_session = NULL;
 *
 *   static void on_media_pause(void* user) { my_player_pause(); }
 *
 *   void start() {
 *       g_session = g_media_api->beginSession("com.example.myplugin");
 *       g_media_api->setTrack(g_session, "Song", "Artist", "Album", NULL);
 *       g_media_api->setDuration(g_session, 180000);
 *       g_media_api->setPlayState(g_session, PLUGIN_MEDIA_PLAYING);
 *
 *       PluginMediaCallbacks cbs = { 0 };
 *       cbs.structSize = sizeof(cbs);
 *       cbs.onPause = on_media_pause;
 *       g_media_api->setCallbacks(g_session, &cbs, NULL);
 *   }
 */
typedef struct PluginMediaAPI {
    uint32_t structSize;   //!< 由宿主填充，插件只读
    uint32_t abiVersion;   //!< 由宿主填充，插件只读；当前为 PLUGIN_MEDIA_ABI_VERSION

    /**
     * @brief 声明媒体会话
     * @param pluginId 插件唯一标识（与 metadata.json 的 id 一致），不能为 NULL
     * @return 会话 handle（不透明令牌），失败返回 NULL
     *
     * 同一时刻只允许一个插件持有会话；后调用者会接管，旧持有者收到 onSessionRevoked 回调。
     * 被接管后旧 handle 立即失效，后续用它的调用一律被丢弃（可重新 beginSession 拿新 handle）。
     */
    void* (*beginSession)(const char* pluginId);

    /**
     * @brief 释放媒体会话，面板回落到宿主播放器
     *
     * 只有当前属主的 handle 有效，陈旧 handle 调用是 no-op。
     */
    void (*endSession)(void* handle);

    /**
     * @brief 上报曲目信息（每次调用会整体替换，NULL 或空串表示该字段为空）
     * @param cover 封面 URL 或本地路径，可为 NULL
     */
    void (*setTrack)(void* handle, const char* title, const char* artist, const char* album, const char* cover);

    /**
     * @brief 上报播放状态
     * @param state PluginMediaPlayState 取值
     */
    void (*setPlayState)(void* handle, int state);

    /**
     * @brief 上报播放进度；会话激活期间宿主会自行按秒推进，插件只需在开始 / 跳转时校正
     */
    void (*setPosition)(void* handle, int64_t positionMs);

    /**
     * @brief 上报总时长，0 表示未知
     */
    void (*setDuration)(void* handle, int64_t durationMs);

    /**
     * @brief 上报当前歌词行，NULL 表示清空
     * @param transLyric 翻译歌词，可为 NULL
     */
    void (*setLyrics)(void* handle, const char* mainLyric, const char* transLyric);

    /**
     * @brief 注册控制事件回调
     * @return 0 表示成功，非 0 表示 handle 已失效
     */
    int (*setCallbacks)(void* handle, const PluginMediaCallbacks* callbacks, void* user);
} PluginMediaAPI;

// ==================== 便利宏定义（供插件使用） ====================

/**
 * @brief 全局 Hook API 指针（插件应该在 init_plugin_with_hook_api 中初始化）
 *
 * 示例：
 *   extern PluginHookAPI* g_hook_api;
 *
 *   extern "C" {
 *       void init_plugin_with_hook_api(PluginHookAPI* hook_api) {
 *           g_hook_api = hook_api;
 *       }
 *   }
 */
extern PluginHookAPI* g_hook_api;

/**
 * @brief 全局媒体接口指针（插件应该在 init_plugin_with_media_api 中初始化）
 *
 * 示例：
 *   extern PluginMediaAPI* g_media_api;
 *
 *   extern "C" {
 *       void init_plugin_with_media_api(PluginMediaAPI* media_api) {
 *           g_media_api = media_api;
 *       }
 *   }
 */
extern PluginMediaAPI* g_media_api;

/**
 * @brief 查询符号地址的便利宏
 *
 * 示例：
 *   void* addr = PLUGIN_SYM("_ZN11YSystemBase8ocrStartEv");
 */
#define PLUGIN_SYM(sym) (g_hook_api ? g_hook_api->querySymbol(sym) : NULL)

/**
 * @brief 注册 Hook 的便利宏
 *
 * 示例：
 *   typedef uint64_t (*OriginalFunc)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
 *   OriginalFunc original = NULL;
 *
 *   uint64_t detour(uint64_t self, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) {
 *       return original(self, a2, a3, a4, a5);
 *   }
 *
 *   PLUGIN_HOOK(addr, detour, original);
 */
#define PLUGIN_HOOK(target, detour, original) \
    (g_hook_api ? g_hook_api->hookFunction(target, (void*)(detour), (void**)&(original)) : -1)
