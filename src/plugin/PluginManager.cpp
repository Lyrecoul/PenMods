#include "PluginManager.h"
#include "PluginSDK.h"
#include "QmlPluginWrapper.h"
#include "common/Event.h"
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLibrary>
#include <QQmlContext>
#include <QQuickView>

namespace mod {

// ============================================================
// 插件 SO 必须导出的函数签名
// ============================================================
//   void init_plugin();                        — 基础初始化（无引擎）
//   void attach_engine(QQmlEngine* engine);    — 可选，接收引擎指针
// ============================================================

PluginManager::PluginManager() : Logger("PluginManager") {
    connect(&Event::getInstance(), &Event::beforeUiInitialization, [this](QQuickView& view, QQmlContext* context) {
        // 1. 暴露 wrapper 给 QML
        auto* wrapper = &QmlPluginWrapper::getInstance();
        context->setContextProperty("pluginManager", wrapper);

        // 2. 捕获引擎指针，并分发给已加载的插件
        setEngine(view.engine());
    });

    scanAndLoadAll();
}

PluginManager::~PluginManager() {
    for (const QString& id : m_loadedLibraries.keys())
        unloadSo(id);
}

// ------------------------------------------------------------------
// Engine 管理
// ------------------------------------------------------------------

void PluginManager::setEngine(QQmlEngine* engine) {
    if (m_engine == engine) return;

    m_engine = engine;
    spdlog::info("QQmlEngine set for PluginManager: {}", fmt::ptr(engine));

    // 引擎就绪后，把它分发给所有已加载但还没拿到引擎的插件
    attachEngineToLoadedPlugins();
}

void PluginManager::attachEngineToLoadedPlugins() {
    if (!m_engine) return;

    for (auto it = m_loadedLibraries.begin(); it != m_loadedLibraries.end(); ++it) {
        QLibrary* lib = it.value();
        if (!lib || !lib->isLoaded()) continue;

        typedef void (*AttachFunc)(QQmlEngine*);
        auto attach = reinterpret_cast<AttachFunc>(lib->resolve("attach_engine"));
        if (attach) {
            spdlog::info("Attaching engine to plugin: {}", it.key().toStdString());
            attach(m_engine);
        } else {
            spdlog::debug("Plugin {} does not export attach_engine(), skipped.", it.key().toStdString());
        }
    }
}

// ------------------------------------------------------------------
// 扫描与加载
// ------------------------------------------------------------------

void PluginManager::scanAndLoadAll() {
    if (m_scanning) {
        spdlog::warn("scanAndLoadAll() is already in progress, skipping.");
        return;
    }
    m_scanning = true;

    QSet<QString> previouslyLoadedIds;
    for (const auto& plugin : m_plugins) {
        if (plugin.isLoaded) {
            previouslyLoadedIds.insert(plugin.id);
        }
    }

    m_plugins.clear();

    QDir dir(m_defaultDir);
    if (!dir.exists()) {
        dir.mkpath(".");
        m_scanning = false;
        return;
    }

    QStringList subDirs = dir.entryList(QDir::Dirs | QDir::NoDotAndDotDot);

    for (const QString& subDirName : subDirs) {
        QString    fullPath = dir.absoluteFilePath(subDirName);
        PluginInfo info;
        if (parseMetadata(fullPath, info)) {
            QFile disabledFlag(fullPath + "/.disabled");
            QFile loadingFlag(fullPath + "/.loading");

            // --- 崩溃自愈 ---
            if (loadingFlag.exists()) {
                spdlog::critical("Plugin {} crashed during last startup. Auto-disabling.", info.id.toStdString());
                setPluginPersistence(info, false);
                loadingFlag.remove();
                info.isEnabled = false;
            } else {
                info.isEnabled = !disabledFlag.exists();
            }

            spdlog::info(
                "Found plugin: {} (ID: {}, Enabled: {})",
                info.name.toStdString(),
                info.id.toStdString(),
                info.isEnabled
            );

            if (info.isEnabled && !info.mainSo.isEmpty()) {
                if (previouslyLoadedIds.contains(info.id)) {
                    spdlog::info("Plugin SO already loaded, skipping reload: {}", info.id.toStdString());
                    info.isLoaded = true;
                } else if (!loadSo(info)) {
                    spdlog::warn("Auto-disabling plugin {} due to load failure.", info.id.toStdString());
                    info.isEnabled = false;
                    info.isLoaded  = false;
                    setPluginPersistence(info, false);
                }
            } else if (info.isEnabled && info.mainSo.isEmpty()) {
                info.isLoaded = true;
            }

            m_plugins.append(info);
        }
    }

    m_scanning = false;

    emit pluginsChanged();
}

// ------------------------------------------------------------------
// 元数据解析
// ------------------------------------------------------------------

bool PluginManager::parseMetadata(const QString& path, PluginInfo& info) {
    QFile file(path + "/metadata.json");
    if (!file.open(QIODevice::ReadOnly)) return false;

    QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
    if (doc.isNull()) return false;

    QJsonObject obj  = doc.object();
    info.id          = obj["id"].toString();
    info.name        = obj["name"].toString();
    info.version     = obj["version"].toString();
    info.author      = obj["author"].toString();
    info.description = obj["description"].toString();
    info.icon        = obj["icon"].toString();
    info.mainQml     = path + "/" + obj["main_qml"].toString();
    info.mainSo      = obj["main_so"].toString();
    info.path        = path;

    return !info.id.isEmpty();
}

// ------------------------------------------------------------------
// SO 加载（核心）
// ------------------------------------------------------------------

bool PluginManager::loadSo(PluginInfo& info) {
    if (info.isLoaded) {
        spdlog::warn("Plugin {} is already loaded, skipping.", info.id.toStdString());
        return true;
    }

    if (m_loadedLibraries.contains(info.id)) {
        spdlog::warn("SO for plugin {} is already in memory, skipping reload.", info.id.toStdString());
        info.isLoaded = true;
        return true;
    }

    QString soPath          = info.path + "/" + info.mainSo;
    QString loadingFlagPath = info.path + "/.loading";

    // 创建加载标记（崩溃自愈用）
    QFile loadingFlag(loadingFlagPath);
    if (!loadingFlag.open(QIODevice::WriteOnly)) {
        spdlog::error("Cannot create loading flag for {}", info.id.toStdString());
        return false;
    }
    loadingFlag.close();

    QLibrary* lib = new QLibrary(soPath);

    if (lib->load()) {
        // ---- 阶段 1: 基础初始化 ----
        typedef void (*InitFunc)();
        auto init = reinterpret_cast<InitFunc>(lib->resolve("init_plugin"));
        if (init) {
            init();
            spdlog::info("init_plugin() called for {}", info.id.toStdString());
        } else {
            spdlog::error("No init_plugin symbol in {}", soPath.toStdString());
            lib->unload();
            delete lib;
            QFile::remove(loadingFlagPath);
            return false;
        }

        // ---- 阶段 2: 如果引擎已就绪，立即 attach ----
        if (m_engine) {
            typedef void (*AttachFunc)(QQmlEngine*);
            auto attach = reinterpret_cast<AttachFunc>(lib->resolve("attach_engine"));
            if (attach) {
                attach(m_engine);
                spdlog::info("attach_engine() called for {}", info.id.toStdString());
            } else {
                spdlog::debug("Plugin {} does not export attach_engine(), skipped.", info.id.toStdString());
            }
        }

        // ---- 阶段 3: 注册到 map，再初始化 Hook API ----
        QFile::remove(loadingFlagPath);
        m_loadedLibraries.insert(info.id, lib);
        info.isLoaded = true;
        initializePluginHookAPI(info.id, lib);
        spdlog::info("Successfully loaded SO: {}", info.id.toStdString());
        return true;
    }

    spdlog::error("Failed to load SO: {}, Error: {}", soPath.toStdString(), lib->errorString().toStdString());
    delete lib;
    QFile::remove(loadingFlagPath);
    return false;
}

// ------------------------------------------------------------------
// 持久化 / 启禁用 / 卸载
// ------------------------------------------------------------------

void PluginManager::setPluginPersistence(const PluginInfo& info, bool enable) {
    QString flagPath = info.path + "/.disabled";
    if (enable) {
        QFile::remove(flagPath);
    } else {
        QFile file(flagPath);
        if (file.open(QIODevice::WriteOnly)) file.close();
    }
}

void PluginManager::unloadSo(const QString& pluginId) {
    QLibrary* lib = m_loadedLibraries.take(pluginId);
    if (!lib) return;
    if (lib->isLoaded()) {
        typedef void (*DestroyFunc)();
        auto destroy = reinterpret_cast<DestroyFunc>(lib->resolve("destroy_plugin"));
        if (destroy) destroy();
        lib->unload();
    }
    delete lib;
}

bool PluginManager::togglePlugin(QString pluginId, bool enable) {
    for (auto& plugin : m_plugins) {
        if (plugin.id == pluginId) {
            if (plugin.isEnabled == enable) {
                spdlog::info(
                    "Plugin {} is already {}. No action needed.",
                    pluginId.toStdString(),
                    enable ? "enabled" : "disabled"
                );
                return true;
            }

            setPluginPersistence(plugin, enable);
            plugin.isEnabled = enable;

            if (enable) {
                if (!plugin.isLoaded && !plugin.mainSo.isEmpty()) {
                    if (!loadSo(plugin)) {
                        plugin.isEnabled = false;
                        setPluginPersistence(plugin, false);
                        return false;
                    }
                } else if (plugin.mainSo.isEmpty()) {
                    plugin.isLoaded = true;
                }
            } else {
                unloadSo(pluginId);
                plugin.isLoaded = false;
            }

            emit pluginsChanged();
            return true;
        }
    }
    return false;
}

QString PluginManager::getPluginMainQml(const QString& pluginId) {
    for (const auto& plugin : m_plugins) {
        if (plugin.id == pluginId) return plugin.mainQml;
    }
    return "";
}

bool PluginManager::uninstallPlugin(QString pluginId) {
    for (auto it = m_plugins.begin(); it != m_plugins.end(); ++it) {
        if (it->id == pluginId) {
            if (it->isEnabled) togglePlugin(pluginId, false);
            unloadSo(pluginId);

            QDir pluginDir(it->path);
            if (pluginDir.exists()) {
                bool success = pluginDir.removeRecursively();
                if (success) {
                    spdlog::info("Successfully uninstalled plugin: {}", pluginId.toStdString());
                    m_plugins.erase(it);
                    emit pluginsChanged();
                    return true;
                } else {
                    spdlog::error("Failed to remove plugin directory: {}", it->path.toStdString());
                    return false;
                }
            } else {
                spdlog::warn("Plugin directory does not exist: {}", it->path.toStdString());
                m_plugins.erase(it);
                emit pluginsChanged();
                return true;
            }
        }
    }

    spdlog::error("Plugin not found for uninstallation: {}", pluginId.toStdString());
    return false;
}

// ------------------------------------------------------------------
// Hook API 实现
// ------------------------------------------------------------------

// Hook API 实现函数 - 供插件调用
static void* querySymbolImpl(const char* symbolName) {
    if (!symbolName) {
        spdlog::error("[PluginHookAPI] Symbol name is null");
        return nullptr;
    }
    
    void* addr = SymDB::getInstance().query(symbolName);
    if (!addr) {
        spdlog::warn("[PluginHookAPI] Symbol not found: {}", symbolName);
    } else {
        spdlog::debug("[PluginHookAPI] Found symbol '{}' at {:#x}", symbolName, reinterpret_cast<uint64_t>(addr));
    }
    return addr;
}

static int hookFunctionImpl(void* targetAddr, void* detourFunc, void** originalFunc) {
    if (!targetAddr || !detourFunc || !originalFunc) {
        spdlog::error("[PluginHookAPI] Invalid parameters for hook (target: {}, detour: {}, original: {})",
                     fmt::ptr(targetAddr), fmt::ptr(detourFunc), fmt::ptr(originalFunc));
        return -1;
    }
    
    int result = DobbyHook(targetAddr, (dobby_dummy_func_t)detourFunc, (dobby_dummy_func_t*)originalFunc);
    if (result != 0) {
        spdlog::error("[PluginHookAPI] Failed to hook at {:#x}", reinterpret_cast<uint64_t>(targetAddr));
    } else {
        spdlog::info("[PluginHookAPI] Successfully hooked at {:#x}", reinterpret_cast<uint64_t>(targetAddr));
    }
    return result;
}

void PluginManager::initializePluginHookAPI(const QString& id, QLibrary* lib) {
    if (!lib || !lib->isLoaded()) return;

    typedef void (*InitPluginWithHookAPIFunc)(PluginHookAPI*);
    auto initWithHookApi = reinterpret_cast<InitPluginWithHookAPIFunc>(
        lib->resolve("init_plugin_with_hook_api")
    );

    if (!initWithHookApi) {
        spdlog::debug("Plugin {} does not export init_plugin_with_hook_api(), skipped.", id.toStdString());
        return;
    }

    // static: 插件持有指向此结构体的指针，其生命周期必须覆盖插件整个运行期
    static PluginHookAPI hookApi = { &querySymbolImpl, &hookFunctionImpl };

    try {
        initWithHookApi(&hookApi);
        spdlog::info("Hook API initialized for plugin: {}", id.toStdString());
    } catch (const std::exception& e) {
        spdlog::error("Exception in init_plugin_with_hook_api for plugin {}: {}", id.toStdString(), e.what());
    }
}

} // namespace mod
