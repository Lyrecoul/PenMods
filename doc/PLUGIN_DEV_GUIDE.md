# PenMods 插件开发指南

## 概述

PenMods 插件是以目录形式安装在 `/userdisk/PenMods/plugins/` 下的扩展包。每个插件可以包含：

- 一个 `metadata.json`（必须）
- 一个编译为 ARM64 的共享库 `.so`（可选）
- 一个或多个 QML 文件（可选）
- 一个图标文件（可选）

插件 `.so` 在主应用启动时由 `PluginManager` 通过 `dlopen` 加载，并通过约定的导出符号获得控制权。插件可以使用 `PluginHookAPI` 钩住主程序的任意函数，也可以通过 `attach_engine` 向 QML 引擎注册自定义类型或上下文属性。

---

## 目录结构

```
/userdisk/PenMods/plugins/
└── com.example.myplugin/       ← 插件目录，名称随意，通常用 ID
    ├── metadata.json            ← 必须
    ├── libmyplugin.so           ← 可选，ARM64 共享库
    ├── Main.qml                 ← 可选，QML 入口
    ├── icon.png                 ← 可选，图标
    ├── .disabled                ← 存在时插件被禁用（由框架管理）
    └── .loading                 ← 崩溃自愈标记（由框架管理，勿手动创建）
```

`.disabled` 和 `.loading` 由 `PluginManager` 自动管理，开发时不需要手动创建。

---

## metadata.json

```json
{
    "id":          "com.example.myplugin",
    "name":        "My Plugin",
    "version":     "1.0.0",
    "author":      "Your Name",
    "description": "一句话描述这个插件做什么",
    "icon":        "icon.png",
    "main_qml":    "Main.qml",
    "main_so":     "libmyplugin.so"
}
```

| 字段 | 类型 | 必须 | 说明 |
|------|------|------|------|
| `id` | string | ✅ | 唯一标识符，建议用反向域名格式 |
| `name` | string | ✅ | 用户可见的插件名称 |
| `version` | string | ✅ | 语义版本号 |
| `author` | string | 否 | 作者名 |
| `description` | string | 否 | 插件简介 |
| `icon` | string | 否 | 相对于插件目录的图标路径；缺省时 UI 显示默认图标 |
| `main_qml` | string | 否 | QML 入口文件的相对路径 |
| `main_so` | string | 否 | 共享库的相对路径；不含此字段则为纯 QML 插件 |

`id` 是插件的唯一键，`togglePlugin` / `uninstallPlugin` 等 API 都以此为索引。保持它全局唯一且不随版本变化。

---

## 导出符号（C ABI）

插件 `.so` 通过导出特定的 C 函数与框架通信。所有函数都必须在 `extern "C"` 块中声明，以避免 C++ 名称修饰。

### `init_plugin` — 基础初始化（必须）

```cpp
extern "C" void init_plugin() {
    // SO 加载完成后立即调用
    // 此时 QQmlEngine 尚未就绪
    // 适合：初始化全局状态、分配资源
}
```

这是 **唯一的必须符号**。`PluginManager` 在 `dlopen` 成功后立即调用它；如果此符号不存在，`.so` 会被卸载并自动禁用该插件。

### `init_plugin_with_hook_api` — Hook 注册（可选）

```cpp
extern "C" void init_plugin_with_hook_api(PluginHookAPI* hook_api) {
    g_hook_api = hook_api;
    // 在此查询符号地址并注册 Hook
    // 调用时机：init_plugin() 之后
}
```

框架在 `init_plugin()` 完成后调用此函数并注入 `PluginHookAPI*`。只需要 Hook 系统函数时才需要导出此符号。详见 [Hook API](#hook-api) 一节。

### `attach_engine` — QML 引擎就绪（可选）

```cpp
extern "C" void attach_engine(QQmlEngine* engine) {
    // QQmlEngine 就绪后调用
    // 适合：注册 QML 类型、设置 context property
}
```

`QQmlEngine` 在 `beforeUiInitialization` 事件触发后才可用，晚于 `.so` 加载。框架会在引擎就绪时调用此函数——如果插件在引擎就绪前加载，会在引擎就绪后补调；如果在引擎就绪后加载（热加载场景），则立即调用。

### `destroy_plugin` — 清理（可选）

```cpp
extern "C" void destroy_plugin() {
    // 插件被禁用或卸载时调用
    // 适合：释放资源、清理全局状态
}
```

---

## 加载生命周期

```
dlopen(libmyplugin.so)
    │
    ├─ init_plugin()                        ← 必须
    │
    ├─ [如果 QQmlEngine 已就绪]
    │       attach_engine(engine)           ← 可选
    │
    └─ init_plugin_with_hook_api(hook_api)  ← 可选

... 运行中 ...

togglePlugin(id, false) 或 uninstallPlugin(id)
    │
    └─ destroy_plugin()                     ← 可选
       dlclose(libmyplugin.so)
```

`attach_engine` 也可能在 `init_plugin_with_hook_api` **之后**被调用（当引擎晚于 `.so` 就绪时），实现中不能依赖两者的相对顺序。

---

## Hook API

`PluginHookAPI` 允许插件在运行时拦截主程序的任意函数。

```cpp
#include "PluginSDK.h"

PluginHookAPI* g_hook_api = nullptr;

// 原始函数指针
typedef void* (*TargetFunc)(void* self, int arg);
TargetFunc original_target = nullptr;

// Detour 函数
void* detour_target(void* self, int arg) {
    // 前置逻辑
    void* result = original_target(self, arg);
    // 后置逻辑
    return result;
}

extern "C" void init_plugin_with_hook_api(PluginHookAPI* hook_api) {
    g_hook_api = hook_api;

    void* addr = hook_api->querySymbol("_ZN11YSystemBase8someFunc Ev");
    if (!addr) return;

    hook_api->hookFunction(addr, (void*)detour_target, (void**)&original_target);
}
```

### PluginHookAPI 接口

| 函数 | 说明 | 返回值 |
|------|------|--------|
| `querySymbol(name)` | 在主程序 `.symtab` 中查找符号地址 | 地址指针，失败返回 `nullptr` |
| `hookFunction(target, detour, original)` | 用 Dobby 在 `target` 处安装 detour | `0` 成功，非 `0` 失败 |

便利宏 `PLUGIN_SYM(sym)` 和 `PLUGIN_HOOK(target, detour, original)` 等价于直接调用上述函数，内部均检查 `g_hook_api` 非空。

`querySymbol` 接受 C++ mangled name。可以用 `nm -D YoudaoDictPen` 或 IDA Pro（`binary/YoudaoDictPen.i64`）查找目标符号。常用符号见 `doc/PLUGIN_HOOK_DEV_GUIDE.md`。

> **注意：** Hook 一旦安装无法卸载（Dobby 限制）。`destroy_plugin` 中不需要也无法移除 Hook。

---

## QML 集成

### 注册 context property

```cpp
extern "C" void attach_engine(QQmlEngine* engine) {
    auto* ctx = engine->rootContext();
    ctx->setContextProperty("myPlugin", &MyPluginObject::getInstance());
}
```

注册后，QML 侧即可直接访问 `myPlugin.someMethod()`。

### 加载插件自身的 QML

`metadata.json` 中的 `main_qml` 路径会被 `PluginManager::getPluginMainQml()` 返回为绝对路径，QML 侧通过 `pluginManager.getPluginInfo(index).mainQmlUrl` 以 `file://` URL 形式获得，可直接传给 `Loader`：

```qml
Loader {
    source: pluginManager.getPluginInfo(index).mainQmlUrl
}
```

插件 QML 文件中可以使用所有已注册的 context property，包括插件自己在 `attach_engine` 中注册的对象。

**屏幕尺寸：** 目标设备屏幕为 320×170，QML 布局须适配此分辨率。优先使用 Youdao 原生 QML 组件而非标准 Qt Quick Controls。

---

## 崩溃自愈

`PluginManager` 在加载 `.so` 前会创建一个 `.loading` 标记文件。如果加载过程中进程崩溃，下次启动时发现 `.loading` 文件存在，框架会认为该插件导致了崩溃，自动将其禁用（写入 `.disabled`）并删除 `.loading`。

这意味着：
- `init_plugin` 中的崩溃（包括全局对象构造函数）会触发自动禁用。
- 如果插件在 `init_plugin` 返回后才崩溃（例如在 detour 函数中），`.loading` 已被删除，不会触发自愈。

---

## 构建插件

插件是独立的 CMake / xmake 项目，不在 PenMods 主仓库内构建。

### 工具链要求

- 目标架构：`aarch64-linux-gnu`（ARM64）
- ABI：glibc 2.27（与设备一致，避免使用更新的 glibc 符号）
- C++ 标准：C++14 或以上
- 推荐工具链：Zig（`zig c++ --target=aarch64-linux-gnu.2.27`）或标准 `aarch64-linux-gnu-g++`

### 编译选项

```sh
# 以 Zig 工具链为例
zig c++ \
    -target aarch64-linux-gnu.2.27 \
    -shared -fPIC \
    -std=c++17 \
    -O2 \
    -I /path/to/PluginSDK \
    src/myplugin.cpp \
    -o libmyplugin.so
```

关键标志：
- `-shared -fPIC`：输出共享库
- `-Wl,--no-undefined`：检查未解析符号（推荐）
- `-Wl,-z,defs`：同上，更严格

### Qt 依赖

如果插件使用 `QQmlEngine*`（在 `attach_engine` 中），需要链接 Qt 头文件。使用与 PenMods 相同的 Qt sysroot：`aarch64-linux-qt-5.15.2`。

不建议在插件中直接链接 `libQt5Core.so` 等 Qt 库——这些库已在主进程中加载，`dlopen` 时会自动共享，无需重复链接。只需引入头文件即可。

---

## 安装与调试

### 安装

```sh
# 创建插件目录
adb shell mkdir -p /userdisk/PenMods/plugins/com.example.myplugin

# 推送文件
adb push metadata.json /userdisk/PenMods/plugins/com.example.myplugin/
adb push libmyplugin.so /userdisk/PenMods/plugins/com.example.myplugin/

# 重启主进程使插件生效
adb shell killall YoudaoDictPen
```

### 查看日志

`PluginManager` 通过 spdlog 输出详细日志，关键词包括：

```
Found plugin: My Plugin (ID: com.example.myplugin, Enabled: true)
init_plugin() called for com.example.myplugin
attach_engine() called for com.example.myplugin
Hook API initialized for plugin: com.example.myplugin
```

失败情形：
```
No init_plugin symbol in /userdisk/.../libmyplugin.so   ← 缺少必须符号
Failed to load SO: ...                                   ← dlopen 失败，检查依赖和架构
Plugin com.example.myplugin crashed during last startup. Auto-disabling.  ← 触发崩溃自愈
```

### 手动禁用 / 重新启用

```sh
# 禁用
adb shell touch /userdisk/PenMods/plugins/com.example.myplugin/.disabled

# 重新启用
adb shell rm /userdisk/PenMods/plugins/com.example.myplugin/.disabled
```

重启主进程后生效。也可以通过 UI 中的插件管理页面操作，底层调用的是 `pluginManager.setPluginEnabled(id, bool)`。

---

## QML 侧插件管理 API

QML context property `pluginManager`（类型 `QmlPluginWrapper`）暴露以下接口：

| 方法 | 说明 |
|------|------|
| `getPluginCount() → int` | 已发现的插件数量 |
| `getPluginInfo(index) → object` | 返回插件信息对象（见下表） |
| `setPluginEnabled(id, bool) → bool` | 启用或禁用插件 |
| `uninstallPlugin(id)` | 卸载并删除插件目录 |
| `requestPluginList()` | 重新扫描插件目录 |

`getPluginInfo` 返回的对象字段：

| 字段 | 类型 | 说明 |
|------|------|------|
| `id` | string | 插件 ID |
| `name` | string | 插件名 |
| `description` | string | 简介 |
| `version` | string | 版本号 |
| `author` | string | 作者 |
| `enabled` | bool | 是否已启用（用户偏好） |
| `loaded` | bool | 是否实际在运行（`enabled && 加载成功`） |
| `icon` | string | 图标路径或内置图标名 |
| `mainQmlUrl` | string | QML 入口的 `file://` URL |

`enabled` 与 `loaded` 的区别：`enabled=true, loaded=false` 表示插件已启用但加载失败（可在 UI 中显示警告）。

信号 `pluginListUpdated()` 在插件列表变化时触发，UI 应监听此信号刷新列表。

---

## 常见问题

**Q: `querySymbol` 返回 `nullptr`，符号名称看起来是对的。**

SymDB 从主程序的 `.symtab` 解析符号。如果主程序以 strip 版本部署，`.symtab` 可能不存在（只有 `.dynsym`）。请用 `readelf -s YoudaoDictPen` 确认符号存在，或改为查询 `.dynsym` 中的导出符号。也可以在 IDA Pro 中直接取地址并用 `PEN_HOOK_ADDR` 宏硬编码。

**Q: `attach_engine` 没有被调用。**

确认函数名拼写无误（`attach_engine`，全小写下划线）且在 `extern "C"` 块中。用 `nm -D libmyplugin.so | grep attach` 验证符号是否导出。

**Q: 插件加载后主程序崩溃，重启后变成禁用状态。**

触发了崩溃自愈机制。在 `init_plugin` 或全局对象构造函数中发生了崩溃。用 `adb logcat` 或 `/userdata/PenMods/penmod.log` 查看崩溃原因。修复后删除 `.disabled` 文件重新启用。

**Q: Detour 函数中可以调用 Qt API 吗？**

可以。主进程已经加载了 Qt，插件可以直接使用 Qt 类型。但要注意线程安全——`QObject` 和大多数 Qt GUI 操作必须在主线程执行。
