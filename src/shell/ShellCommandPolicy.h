// SPDX-License-Identifier: GPL-3.0-only
/*
 * Copyright (C) 2022-present, PenUniverse.
 * This file is part of the PenMods open source project.
 */

#pragma once

// AI 自动执行 shell 命令的安全策略与命令解析器。
//
// 设计约束（见 build/diag/shell/DESIGN.md）：
// - 纯 STL、不依赖 Qt，便于在主机上用普通 g++ 单独编译与单测；
// - 先解析、后判断：把命令拆成可审计结构（词/引号/展开、操作符、重定向、命令替换），
//   解析失败或遇到无法安全建模的构造一律拒绝（fail closed）；
// - automatic 模式只跳过“用户确认”，不跳过黑名单、超时、输出上限、并发限制、
//   取消/会话/requestSeq 校验（后者由 Backend 负责，本模块只负责命令级策略）。
//
// 重要：本模块不是沙箱。automatic 模式的放行范围是显式的只读网页抓取命令白名单，
// 而不是“安全地执行任意命令”。残留风险见 DESIGN.md。

#include <cstddef>
#include <string>
#include <vector>

namespace mod::shell {

// ---------------------------------------------------------------------------
// 上限（解析器与策略共用；超限直接拒绝）
// ---------------------------------------------------------------------------

inline constexpr std::size_t kMaxCommandBytes   = 8192; // 单条命令最大字节数
inline constexpr std::size_t kMaxListItems      = 8;    // ; / & 分隔的命令项上限
inline constexpr std::size_t kMaxPipelineStages = 8;    // 单个管道的段数上限
inline constexpr std::size_t kMaxArguments      = 32;   // 单条命令的 argv 上限
inline constexpr std::size_t kMaxNesting        = 4;    // 展开/嵌套解析深度上限

// ---------------------------------------------------------------------------
// 模式
// ---------------------------------------------------------------------------

enum class ToolMode {
    Off,        // 工具不提供给模型
    Confirm,    // 现有行为：每条命令由用户确认
    Automatic,  // 自动执行，但必须通过只读网页抓取策略
    Restricted, // 无逐条确认，以非特权 UID 执行（不是沙盒）
    Full,       // 无逐条确认，以应用权限执行
};

const char* toolModeName(ToolMode mode); // "off" / "confirm" / "automatic"
bool        toolModeFromName(const std::string& name, ToolMode& out);

// ---------------------------------------------------------------------------
// 命令结构（可审计 AST）
// ---------------------------------------------------------------------------

enum class WordPartKind {
    Literal,             // 未加引号的字面文本（可能含未加引号的 * ? [，由 Word::glob 标记）
    SingleQuoted,        // '...'，内部原样字面
    DoubleQuoted,        // "..."，内部可能含展开，见 inner
    ParameterExpansion,  // $NAME / ${...} / $? 等
    CommandSubstitution, // $(...) / `...`（text 为内部原文）
    ArithmeticExpansion, // $((...))
    Tilde,               // 词首未加引号的 ~
};

struct WordPart {
    WordPartKind          kind = WordPartKind::Literal;
    std::string           text;  // Literal/引号内文本，或展开的原文
    std::vector<WordPart> inner; // DoubleQuoted 的内部部分
};

struct Word {
    std::vector<WordPart> parts;
    bool                  glob    = false; // 未加引号的通配符 * ? [
    bool                  escaped = false; // 含反斜杠转义（命令名审计要求无歧义字面量）

    bool        hasExpansion() const;   // 含参数/命令/算术/主目录展开
    bool        isFullyLiteral() const; // 所有部分是字面量（含引号），可安全取 value()
    std::string value() const;          // 引号/转义处理后的 argv 值；仅 isFullyLiteral() 时完整
    std::string render() const;         // 可读的 shell 形式（用于摘要/日志）
};

enum class RedirKind {
    Output,       // >
    Append,       // >>
    Input,        // <
    ReadWrite,    // <>
    DupOutput,    // >&
    DupInput,     // <&
    Heredoc,      // << / <<-（解析器已拒绝，仅保留枚举）
    HereString,   // <<<（解析器已拒绝）
    OutputAndErr, // &>（ash 支持，但 automatic 拒绝）
    AppendAndErr, // &>>
};

struct Redirection {
    RedirKind kind = RedirKind::Output;
    int       fd   = -1; // 显式 fd（2>...）或 -1
    Word      target;
};

struct SimpleCommand {
    std::vector<std::string> assignments; // NAME=value（原文）
    std::vector<Word>        argv;
    std::vector<Redirection> redirections;
};

struct Pipeline {
    std::vector<SimpleCommand> commands; // 以 | 连接
};

struct AndOrList {
    std::vector<Pipeline> pipelines;   // 以 && / || 连接
    std::vector<bool>     orOperators; // 长度 = pipelines.size()-1，true 表示 ||
};

struct ListItem {
    AndOrList andOr;
    bool      background = false; // 以 & 收尾
};

struct CommandList {
    std::vector<ListItem> items;
};

enum class Feature {
    CommandSubstitution,
    ParameterExpansion,
    ArithmeticExpansion,
    Heredoc,
    HereString,
    Background,
    Redirection,
    Assignment,
    Glob,
    Tilde,
    Pipeline,
    Sequence,
    Comment,
};

const char*          featureName(Feature feature);
std::vector<Feature> featuresOf(const CommandList& list);

struct ParseResult {
    bool                 ok = false;
    std::string          error; // 解析失败原因（中文）
    std::size_t          errorOffset = 0;
    CommandList          list;
    std::vector<Feature> features; // 去重后的结构特征
};

// 解析单条 `/bin/sh -c` 命令。注意：本解析器只建模 POSIX sh 的一个保守子集，
// 遇到子 shell、函数、heredoc、$'...' 等构造会返回 ok=false（调用方必须拒绝）。
ParseResult parseCommand(const std::string& command);

// ---------------------------------------------------------------------------
// 策略判断
// ---------------------------------------------------------------------------

enum class DecisionCode {
    AllowedAutomatic,        // automatic 放行
    RequiresConfirm,         // confirm 模式：交给用户确认
    DeniedOff,               // 工具关闭
    DeniedBlocklist,         // 命中用户黑名单
    DeniedEmpty,             // 空命令
    DeniedTooLarge,          // 超长
    DeniedParseError,        // 无法解析
    DeniedUnsupportedSyntax, // 不支持的语法（子 shell 等）
    DeniedExpansion,         // 变量/命令/算术/主目录展开
    DeniedGlob,              // 未加引号的通配符
    DeniedBackground,        // 后台执行
    DeniedAssignment,        // 环境变量赋值
    DeniedRedirection,       // 文件重定向
    DeniedCommandNotAllowed, // 命令不在白名单 / 命令名形式不安全
    DeniedArguments,         // 参数不满足审计规则
    DeniedUrl,               // URL 不满足 http(s) 公网约束
    DeniedTooComplex,        // 命令结构过于复杂
};

const char* decisionCodeName(DecisionCode code); // 稳定英文 id，用于日志

struct PolicyConfig {
    ToolMode                 mode = ToolMode::Off;
    std::vector<std::string> blocklist; // 兼容旧配置的子串黑名单（大小写不敏感）
};

struct Decision {
    bool                     allowed      = false; // 可以立即执行，无需用户确认
    bool                     needsConfirm = false; // confirm 模式：应弹出确认框
    DecisionCode             code         = DecisionCode::DeniedOff;
    std::string              reason;   // 面向用户/模型的中文原因
    std::string              summary;  // 命令结构摘要（日志/工具卡）
    std::vector<std::string> warnings; // confirm 模式的附加风险提示
};

Decision evaluateCommand(const std::string& command, const PolicyConfig& config);

// confirm 对话框用的结构审计信息（不参与放行判断）。
struct AuditReport {
    bool                     ok = false;
    std::string              error;
    std::string              summary;
    std::vector<std::string> commands; // 顶层命令名（按出现顺序）
    std::vector<std::string> warnings; // 风险提示（中文）
    std::vector<std::string> features; // 结构特征（英文 id）
};

AuditReport auditCommand(const std::string& command);

// automatic 模式允许的命令（用于设置页/日志/文档展示）。
std::vector<std::string> automaticAllowedCommands();

// 策略/解析器版本号，随规则变化递增，便于日志区分。
const char* policyVersion();

} // namespace mod::shell
