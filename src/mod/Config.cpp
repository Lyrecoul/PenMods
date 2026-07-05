// SPDX-License-Identifier: GPL-3.0-only
/*
 * Copyright (C) 2022-present, PenUniverse.
 * This file is part of the PenMods open source project.
 */

#include "mod/Config.h"

#include "common/Utils.h"

#include "common/util/System.h"

#include "Version.h"

#include <set>

namespace fs = std::filesystem;

namespace mod {

std::string get_config_path() { return (util::getModuleFileInfo().absolutePath() + "config.json").toStdString(); }

Config::Config() : Logger("Config") {

    // clang-format off

    mData = {
        {"version", VERSION_CONFIG},
        {"column_db", {
             {"patch", true}
        }},
        {"dev", {
            {"offline_rm", true}
        }},
        {"net", {
            {"proxy_enabled", false},
            {"proxy_type", 0},
            {"proxy_hostname", "127.0.0.1"},
            {"proxy_port", 1270},
            {"proxy_username", ""},
            {"proxy_password", ""},
        }},
        {"logger", {
            {"no_upload_user_action", true},
            {"no_upload_raw_scan_img", true},
            {"no_upload_httplog", true},
            {"filtered_tags", {
                "queue", "sender",
                "YDownloader", "YColumnDb", "YHttpManager",
                "YHistoryManager", "YWordBookManager", "YNetTranslateEngine",
                "YColumnManager", "YMediaManager", "YMediaPlayerManager",
                "YTextBookDb", "YTextBookManager", "YTextBookBlockManager",
                "YTextBookTaskManager", "YStrokeManager", "YInteractiveLearningManager",
                "YReadingBookQuestionOptionManager",
                "YLoginManager", "BlueToothManager",
                "YRecordCenter", "YSystemBase", "YLogManager"
            }},
            {"levels", {
                {"Config", "debug"},
                {"MusicPlayer", "info"},
                {"FileManager", "info"},
                {"TextBook", "info"},
                {"ChatBot", "info"},
                {"Hitokoto", "info"},
                {"RimeWrapper", "info"},
                {"Rime", "info"},
                {"AudioDaemon", "info"},
                {"InputDaemon", "info"},
                {"AudioRecorder", "info"},
                {"SymDB", "info"},
                {"ImageViewer", "info"},
                {"WebPImageProvider", "info"},
                {"WebPAnimatedImage", "info"},
                {"PluginManager", "info"},
                {"Updater", "info"},
                {"ASound", "info"}
            }}
        }},
        {"query", {
            {"lower_scan", false},
            {"type_by_hand", true}
        }},
        {"wordbook", {
            {"phrase_tab", true},
            {"nocase_sensitive", true}
        }},
        {"screen", {
            {"sleep_duration", 30},
            {"intel_sleep", false},
            {"intel_sleep_audio_lock", false}
        }},
        {"battery", {
            {"suspend_duration", 600},
            {"performance_mode", 0}
        }},
        {"locker", {
            {"enabled", false},
            {"password", "abcd"},
            {"scene", {
                {"screen_on", false},
                {"restart", true},
                {"reset_page", true},
                {"dev_setting", false},
                {"filemanager",false}
            }}
        }},
        {"antiembs", {
            {"auto_mute", false},
            {"low_voice", false},
            {"no_auto_pron", false},
            {"fast_hide_music", false},
            {"fast_mute", false}
        }},
        {"serv", {
            {"ssh_autorun", false},
            {"adb_autorun", false},
            {"adb_skip_verification", false}
        }},
        {"fm", {
            {"order", {
                {"basic", 0},
                {"reversed", false}
            }},
            {"hide_paired_lyrics", false},
            {"show_hidden_files", false}
        }},
        {"wallpaper", {
            {"mode", 0},
            {"custom_image_path", ""},
            {"wallpaper_folder", ""},
            {"cycle_interval", 300},
            {"last_wallpaper", ""}
        }},
        {"capture", {
            {"enabled", false}
        }},
        {"ai", {
            {"speech_assistant", false},
            {"streaming", true},
            {"models", json::array({
                json{
                    {"id",          "deepseek-v4-flash"},
                    {"name",        "DeepSeek Chat"},
                    {"provider",    "DeepSeek"},
                    {"endpoint",    "https://api.deepseek.com/v1/chat/completions"},
                    {"modelId",     "deepseek-v4-flash"},
                    {"apiKey",      ""},
                    {"temperature", 0.7},
                    {"maxContextSize", 0},
                    {"capabilities", json{
                        {"text",      true},
                        {"vision",    false},
                        {"audio",     false},
                        {"toolCall",  false},
                        {"reasoning", false}
                    }},
                    {"extraParams", ""},
                    {"proxyVisionModelId", ""},
                    {"proxyVisionPrompt", "请详细描述这张图片的内容。如果图片中有文字，请完整转录。"}
                }
            })},
            {"activeModelId", "deepseek-v4-flash"},
            {"prompts", json::array({
                json{
                    {"id",      "default"},
                    {"name",    "通用助手"},
                    {"content", "你是一个有用的助手，使用中文回复用户的问题。"}
                }
            })},
            {"activePromptId", "default"},
            {"tavily", {
                {"api_key",      ""},
                {"search_depth", "advanced"},
                {"max_results",  5},
                {"enabled",      false}
            }},
            {"shell_tool", {
                {"enabled",   false},
                {"timeout_ms", 10000},
                {"max_output_bytes", 4096},
                {"blocklist", json::array()}
            }},
            {"math_render", {
                {"enabled",     false},
                {"server_path", ""}
            }}
        }}
    };

    // clang-format on

    mDefaults = mData;
    _load();

    // Config 初始化完成，后续 Logger 可以从 Config 读取日志级别
    Logger::s_configLoaded = true;
}

json Config::read(const std::string& name) {
    if (!mData.contains(name)) {
        return {};
    }
    return mData.at(name);
}

bool Config::write(const std::string& name, json content, bool saveImmediately) {
    if (mData.find(name) == mData.end()) {
        return false;
    }
    mData[name] = std::move(content);
    if (saveImmediately) {
        _save();
    }
    return true;
}

bool Config::_update(json& data) {
    if (!data.contains("version") || data.at("version") == VERSION_CONFIG) {
        return false;
    }
    info("Configuration file is being updated...");

    try {

        // v100 -> v110
        if (data["version"] < 110) {
            data["fm"] = {
                {"order", {{"basic", 0}, {"reversed", false}}}
            };
            data["antiembs"]["fast_mute"] = false;
            data["version"]               = 110;
        }

        // v110 -> v116
        if (data["version"] < 116) {
            data["locker"]["scene"]["dev_setting"] = false;
            data["version"]                        = 116;
        }

        // v116 -> v117
        if (data["version"] < 117) {
            data["fm"]["hide_paird_lyrics"]     = false;
            data["battery"]["performance_mode"] = 0;
            data["version"]                     = 117;
        }

        // v117 -> v118
        if (data["version"] < 118) {
            data["wordbook"].erase("mod_exporter");
            data["version"] = 118;
        }

        // v118 -> v120
        if (data["version"] < 120) {
            data["ai"] = {
                {"bing", {{"enabled", false}, {"request_address", ""}, {"chathub_address", ""}}}
            };
            data["version"] = 120;
        }

        // v120 -> v130
        if (data["version"] < 130) {
            data["ai"]["speech_assistant"]   = false;
            data["fm"]["hide_paired_lyrics"] = data["fm"]["hide_paird_lyrics"];
            data["fm"].erase("hide_paird_lyrics");
            data["dev"].erase("wifi_page_show_ip");
            data["column_db"].erase("limit");
            data["column_db"]["patch"] = true;
            data["version"]            = 130;
        }

        // v130 -> v131
        if (data["version"] < 131) {
            if (data.contains("ai")) {
                if (!data["ai"].contains("models") || !data["ai"]["models"].is_array()) {
                    auto& cb                    = data["ai"]["chatbot"];
                    data["ai"]["models"]        = json::array({
                        json{{"id", cb.value("model", "deepseek-chat")},
                             {"name", "DeepSeek Chat"},
                             {"provider", "DeepSeek"},
                             {"endpoint", cb.value("api_endpoint", "https://api.deepseek.com/v1/chat/completions")},
                             {"modelId", cb.value("model", "deepseek-chat")},
                             {"apiKey", cb.value("api_key", "")},
                             {"temperature", cb.value("temperature", 0.7)},
                             {"extraParams", ""}}
                    });
                    data["ai"]["activeModelId"] = cb.value("model", "deepseek-chat");
                }
                if (!data["ai"].contains("prompts") || !data["ai"]["prompts"].is_array()) {
                    std::string defPrompt = "你是一个有用的助手，使用中文回复用户的问题。";
                    if (data["ai"].contains("chatbot"))
                        defPrompt = data["ai"]["chatbot"].value("default_prompt", defPrompt);
                    data["ai"]["prompts"]        = json::array({
                        json{{"id", "default"}, {"name", "通用助手"}, {"content", defPrompt}}
                    });
                    data["ai"]["activePromptId"] = "default";
                }
                // 迁移 streaming 到顶层
                if (!data["ai"].contains("streaming") && data["ai"].contains("chatbot"))
                    data["ai"]["streaming"] = data["ai"]["chatbot"].value("streaming", true);
            }
            data["version"] = 131;
        }

    } catch (...) {
        return false;
    }

    return true;
}

bool Config::_load() {
    info("Loading configuration...");
    auto path = get_config_path();
    if (!fs::exists(path)) {
        warn("Configuration not found, creating...");
        return _save() ? _load() : false;
    }
    json tmp;
    try {
        tmp = json::parse(readFile(path.c_str()));
    } catch (...) {}
    if (tmp.empty() || !tmp.contains("version")) {
        warn("Configuration error, being repaired...");
        return _save() ? _load() : false;
    }
    if (tmp["version"] != VERSION_CONFIG) {
        if (!_update(tmp)) {
            return false;
        }
        info("Saving configuration...");
    }
    mData = std::move(tmp);
    if (_fill_missing_defaults(mData, mDefaults)) {
        info("Filling missing default keys, saving...");
        _save();
    }
    info("Successfully loaded configuration.");
    return true;
}

bool Config::_save() {
    std::ofstream ofile;
    ofile.open(get_config_path());
    if (ofile.good()) {
        ofile << mData.dump(4);
        return true;
    }
    return false;
}

// 递归补充 target 中缺失的默认键，保留 target 中已有的值和多余的键
bool Config::_fill_missing_defaults(json& target, const json& defaults) {
    bool changed = false;
    for (auto it = defaults.begin(); it != defaults.end(); ++it) {
        const auto& key = it.key();
        if (!target.contains(key)) {
            target[key] = it.value();
            changed     = true;
        } else if (it->is_object() && target[key].is_object()) {
            if (_fill_missing_defaults(target[key], *it)) {
                changed = true;
            }
        }
    }
    return changed;
}

// 递归删除 target 中不在 reference（mDefaults）里的字段
void Config::_strip_unknown_keys(json& target, const json& reference) {
    if (!target.is_object() || !reference.is_object()) return;
    std::vector<std::string> to_remove;
    for (auto it = target.begin(); it != target.end(); ++it) {
        if (!reference.contains(it.key())) {
            to_remove.push_back(it.key());
        } else if (it->is_object() && reference[it.key()].is_object()) {
            _strip_unknown_keys(*it, reference[it.key()]);
        }
    }
    for (const auto& key : to_remove) {
        info("清洗配置：移除未知字段 '{}'", key);
        target.erase(key);
    }
}

bool Config::sanitize() {
    _strip_unknown_keys(mData, mDefaults);
    _fill_missing_defaults(mData, mDefaults);
    return _save();
}

} // namespace mod
