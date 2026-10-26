// SPDX-License-Identifier: GPL-3.0-only
/*
 * Copyright (C) 2022-present, PenUniverse.
 * This file is part of the PenMods open source project.
 */

#include "shell/ShellCommandPolicy.h"

#include <map>
#include <set>
#include <utility>

// 本文件只依赖 C++ 标准库：不要引入 Qt/平台头，保证可以在主机上单独编译与单测。
// 解析器只建模 POSIX sh（设备为 busybox 1.27.2 ash）的一个保守子集；任何无法
// 精确建模的构造都返回解析失败，调用方必须据此拒绝执行（fail closed）。

namespace mod::shell {

namespace {

constexpr std::size_t kNpos = std::string::npos;

// ---------------------------------------------------------------------------
// 基础字符工具（只处理 ASCII；非 ASCII 字节一律视为普通字面字节）
// ---------------------------------------------------------------------------

bool isBlank(char c) { return c == ' ' || c == '\t'; }
bool isNewline(char c) { return c == '\n'; }
bool isDigitChar(char c) { return c >= '0' && c <= '9'; }
bool isNameStart(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; }
bool isNameChar(char c) { return isNameStart(c) || isDigitChar(c); }

bool isOperatorChar(char c) { return c == ';' || c == '&' || c == '|' || c == '(' || c == ')' || c == '<' || c == '>'; }

bool isWordDelimiter(char c) { return isBlank(c) || isNewline(c) || isOperatorChar(c); }

std::string trimAscii(const std::string& value) {
    std::size_t begin = 0;
    std::size_t end   = value.size();
    while (begin < end && isBlank(value[begin])) ++begin;
    while (end > begin && isBlank(value[end - 1])) --end;
    return value.substr(begin, end - begin);
}

char lowerAsciiChar(char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; }

std::string lowerAscii(std::string value) {
    for (char& c : value) c = lowerAsciiChar(c);
    return value;
}

bool endsWith(const std::string& value, const std::string& suffix) {
    return value.size() >= suffix.size() && value.compare(value.size() - suffix.size(), suffix.size(), suffix) == 0;
}

// 与 Qt::CaseInsensitive 对 ASCII 的行为一致；非 ASCII 字节按原样比较。
bool containsIgnoreCaseAscii(const std::string& haystack, const std::string& needle) {
    if (needle.empty() || needle.size() > haystack.size()) return false;
    for (std::size_t i = 0; i + needle.size() <= haystack.size(); ++i) {
        bool equal = true;
        for (std::size_t j = 0; j < needle.size(); ++j) {
            if (lowerAsciiChar(haystack[i + j]) != lowerAsciiChar(needle[j])) {
                equal = false;
                break;
            }
        }
        if (equal) return true;
    }
    return false;
}

bool hasControlChars(const std::string& value) {
    for (char raw : value) {
        const auto c = static_cast<unsigned char>(raw);
        if (c < 0x20 || c == 0x7f) return true;
    }
    return false;
}

bool isDigitsOnly(const std::string& value) {
    if (value.empty()) return false;
    for (char c : value) {
        if (!isDigitChar(c)) return false;
    }
    return true;
}

// 允许 1 / 1.5 / 0.25 这类非负十进制（curl 的时间/大小参数）。
bool isNonNegativeDecimal(const std::string& value) {
    if (value.empty()) return false;
    bool dotSeen   = false;
    bool digitSeen = false;
    for (char c : value) {
        if (isDigitChar(c)) {
            digitSeen = true;
            continue;
        }
        if (c == '.' && !dotSeen) {
            dotSeen = true;
            continue;
        }
        return false;
    }
    return digitSeen;
}

// curl --limit-rate：数字 + 可选 K/M/G 后缀。
bool isRateValue(const std::string& value) {
    if (value.empty()) return false;
    std::size_t digits = value.size();
    const char  last   = value.back();
    if (last == 'k' || last == 'K' || last == 'm' || last == 'M' || last == 'g' || last == 'G')
        digits = value.size() - 1;
    if (digits == 0) return false;
    return isDigitsOnly(value.substr(0, digits));
}

// head/tail 的 -n / -c：允许 -5 / +3 这种写法。
bool isIndexValue(const std::string& value) {
    if (value.empty()) return false;
    std::size_t i = 0;
    if (value[0] == '+' || value[0] == '-') i = 1;
    if (i >= value.size()) return false;
    for (; i < value.size(); ++i) {
        if (!isDigitChar(value[i])) return false;
    }
    return true;
}

// curl --range：0-99 / 1- / 1,3,5-7。
bool isRangeValue(const std::string& value) {
    if (value.empty()) return false;
    for (char c : value) {
        if (!isDigitChar(c) && c != '-' && c != ',') return false;
    }
    return true;
}

bool isPlainNumber(const std::string& value) { return isDigitsOnly(value); }

// ---------------------------------------------------------------------------
// 结构特征
// ---------------------------------------------------------------------------

void pushFeature(std::vector<Feature>& features, Feature feature) {
    for (Feature existing : features) {
        if (existing == feature) return;
    }
    features.push_back(feature);
}

void collectPartFeatures(const std::vector<WordPart>& parts, std::vector<Feature>& out) {
    for (const WordPart& part : parts) {
        switch (part.kind) {
        case WordPartKind::ParameterExpansion:
            pushFeature(out, Feature::ParameterExpansion);
            break;
        case WordPartKind::CommandSubstitution:
            pushFeature(out, Feature::CommandSubstitution);
            break;
        case WordPartKind::ArithmeticExpansion:
            pushFeature(out, Feature::ArithmeticExpansion);
            break;
        case WordPartKind::Tilde:
            pushFeature(out, Feature::Tilde);
            break;
        case WordPartKind::DoubleQuoted:
            collectPartFeatures(part.inner, out);
            break;
        case WordPartKind::Literal:
        case WordPartKind::SingleQuoted:
            break;
        }
    }
}

void collectWordFeatures(const Word& word, std::vector<Feature>& out) {
    if (word.glob) pushFeature(out, Feature::Glob);
    collectPartFeatures(word.parts, out);
}

bool partHasExpansion(const std::vector<WordPart>& parts) {
    for (const WordPart& part : parts) {
        switch (part.kind) {
        case WordPartKind::ParameterExpansion:
        case WordPartKind::CommandSubstitution:
        case WordPartKind::ArithmeticExpansion:
        case WordPartKind::Tilde:
            return true;
        case WordPartKind::DoubleQuoted:
            if (partHasExpansion(part.inner)) return true;
            break;
        case WordPartKind::Literal:
        case WordPartKind::SingleQuoted:
            break;
        }
    }
    return false;
}

bool partIsFullyLiteral(const std::vector<WordPart>& parts) {
    for (const WordPart& part : parts) {
        switch (part.kind) {
        case WordPartKind::Literal:
        case WordPartKind::SingleQuoted:
            break;
        case WordPartKind::DoubleQuoted:
            if (!partIsFullyLiteral(part.inner)) return false;
            break;
        default:
            return false;
        }
    }
    return true;
}

std::string partsLiteralValue(const std::vector<WordPart>& parts) {
    std::string out;
    for (const WordPart& part : parts) {
        switch (part.kind) {
        case WordPartKind::Literal:
        case WordPartKind::SingleQuoted:
            out += part.text;
            break;
        case WordPartKind::DoubleQuoted:
            out += partsLiteralValue(part.inner);
            break;
        default:
            break;
        }
    }
    return out;
}

std::string partsRender(const std::vector<WordPart>& parts) {
    std::string out;
    for (const WordPart& part : parts) {
        switch (part.kind) {
        case WordPartKind::Literal:
            out += part.text;
            break;
        case WordPartKind::SingleQuoted:
            out += '\'';
            out += part.text;
            out += '\'';
            break;
        case WordPartKind::DoubleQuoted:
            out += '"';
            out += partsRender(part.inner);
            out += '"';
            break;
        case WordPartKind::ParameterExpansion:
        case WordPartKind::ArithmeticExpansion:
            out += part.text;
            break;
        case WordPartKind::CommandSubstitution:
            out += "$(";
            out += part.text;
            out += ')';
            break;
        case WordPartKind::Tilde:
            out += '~';
            break;
        }
    }
    return out;
}

void appendLiteralPart(std::vector<WordPart>& parts, char c) {
    if (!parts.empty() && parts.back().kind == WordPartKind::Literal) {
        parts.back().text.push_back(c);
        return;
    }
    WordPart part;
    part.kind = WordPartKind::Literal;
    part.text.assign(1, c);
    parts.push_back(std::move(part));
}

// 单引号安全引用，用于摘要；不是给 shell 执行用的。
std::string shellQuote(const std::string& value) {
    if (value.empty()) return "''";
    bool needsQuote = false;
    for (char c : value) {
        const bool safe = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || isDigitChar(c) || c == '_' || c == '-'
                       || c == '.' || c == '/' || c == ':' || c == '=' || c == '@' || c == '%' || c == '+' || c == ',';
        if (!safe) {
            needsQuote = true;
            break;
        }
    }
    if (!needsQuote) return value;
    std::string out = "'";
    for (char c : value) {
        if (c == '\'') {
            out += "'\\''";
        } else {
            out += c;
        }
    }
    out += '\'';
    return out;
}

// 是否是合法赋值词（NAME=...，NAME 部分必须未加引号）。不用于放行判断，仅解析。
bool assignmentName(const Word& word, std::string& name) {
    if (word.parts.empty()) return false;
    const WordPart& first = word.parts.front();
    if (first.kind != WordPartKind::Literal) return false;
    const std::size_t eq = first.text.find('=');
    if (eq == std::string::npos || eq == 0) return false;
    if (!isNameStart(first.text[0])) return false;
    for (std::size_t i = 1; i < eq; ++i) {
        if (!isNameChar(first.text[i])) return false;
    }
    name = first.text.substr(0, eq);
    return true;
}

const char* redirKindText(RedirKind kind) {
    switch (kind) {
    case RedirKind::Output:
        return ">";
    case RedirKind::Append:
        return ">>";
    case RedirKind::Input:
        return "<";
    case RedirKind::ReadWrite:
        return "<>";
    case RedirKind::DupOutput:
        return ">&";
    case RedirKind::DupInput:
        return "<&";
    case RedirKind::Heredoc:
        return "<<";
    case RedirKind::HereString:
        return "<<<";
    case RedirKind::OutputAndErr:
        return "&>";
    case RedirKind::AppendAndErr:
        return "&>>";
    }
    return "?";
}

// ---------------------------------------------------------------------------
// Lexer
// ---------------------------------------------------------------------------

struct Token {
    enum class Type { Word, Operator, Redirection, Unsupported, End };
    enum class Op { Newline, Semicolon, Ampersand, AndAnd, OrOr, Pipe, LParen, RParen };

    Type        type = Type::End;
    Word        word;
    Op          op        = Op::Newline;
    RedirKind   redirKind = RedirKind::Output;
    int         redirFd   = -1;
    std::string unsupported;
    std::size_t offset = 0;
};

class Lexer {
public:
    Lexer(const std::string& input, ParseResult& out) : in_(input), out_(out) {}

    Token next() {
        for (;;) {
            skipBlanksAndContinuations();
            if (pos_ >= in_.size()) return makeToken(Token::Type::End);

            const std::size_t start = pos_;
            const char        c     = in_[pos_];

            if (isNewline(c)) {
                ++pos_;
                Token token;
                token.type   = Token::Type::Operator;
                token.op     = Token::Op::Newline;
                token.offset = start;
                return token;
            }
            if (c == '#') {
                pushFeature(out_.features, Feature::Comment);
                while (pos_ < in_.size() && in_[pos_] != '\n') ++pos_;
                continue;
            }
            if (isDigitChar(c)) {
                std::size_t digitsEnd = pos_;
                while (digitsEnd < in_.size() && isDigitChar(in_[digitsEnd])) ++digitsEnd;
                if (digitsEnd < in_.size() && (in_[digitsEnd] == '<' || in_[digitsEnd] == '>')) {
                    int fd = 0;
                    for (std::size_t i = pos_; i < digitsEnd; ++i) {
                        if (fd > 100000) {
                            fd = 100000;
                            break;
                        }
                        fd = fd * 10 + (in_[i] - '0');
                    }
                    pos_ = digitsEnd;
                    return redirectionToken(fd);
                }
                return wordToken();
            }
            if (c == '<' || c == '>') return redirectionToken(-1);
            if (c == '&' && pos_ + 1 < in_.size() && in_[pos_ + 1] == '>') return redirectionToken(-1);
            if (isOperatorChar(c)) return operatorToken();
            return wordToken();
        }
    }

    bool failed() const { return failed_; }

private:
    const std::string& in_;
    ParseResult&       out_;
    std::size_t        pos_    = 0;
    bool               failed_ = false;

    void setError(std::size_t offset, const std::string& message) {
        if (failed_) return;
        failed_          = true;
        out_.error       = message;
        out_.errorOffset = offset;
    }

    Token makeToken(Token::Type type) const {
        Token token;
        token.type   = type;
        token.offset = pos_;
        return token;
    }

    Token unsupportedToken(std::size_t offset, const std::string& message) const {
        Token token;
        token.type        = Token::Type::Unsupported;
        token.offset      = offset;
        token.unsupported = message;
        return token;
    }

    void skipBlanksAndContinuations() {
        for (;;) {
            while (pos_ < in_.size() && isBlank(in_[pos_])) ++pos_;
            if (pos_ + 1 < in_.size() && in_[pos_] == '\\' && in_[pos_ + 1] == '\n') {
                pos_ += 2;
                continue;
            }
            break;
        }
    }

    Token redirectionToken(int fd) {
        Token token;
        token.type    = Token::Type::Redirection;
        token.offset  = pos_;
        token.redirFd = fd;

        const char c = in_[pos_];
        if (c == '&') {
            // 调用方已确认 '&' 后紧跟 '>'
            pos_ += 2;
            if (pos_ < in_.size() && in_[pos_] == '>') {
                ++pos_;
                token.redirKind = RedirKind::AppendAndErr;
            } else {
                token.redirKind = RedirKind::OutputAndErr;
            }
            return token;
        }
        if (c == '<') {
            ++pos_;
            if (pos_ < in_.size() && in_[pos_] == '<') {
                ++pos_;
                if (pos_ < in_.size() && in_[pos_] == '<') {
                    ++pos_;
                    pushFeature(out_.features, Feature::HereString);
                    return unsupportedToken(token.offset, "不支持 here-string（<<<），已拒绝");
                }
                if (pos_ < in_.size() && in_[pos_] == '-') ++pos_;
                pushFeature(out_.features, Feature::Heredoc);
                return unsupportedToken(token.offset, "不支持 heredoc（<<），无法安全审计，已拒绝");
            }
            if (pos_ < in_.size() && in_[pos_] == '&') {
                ++pos_;
                token.redirKind = RedirKind::DupInput;
            } else if (pos_ < in_.size() && in_[pos_] == '>') {
                ++pos_;
                token.redirKind = RedirKind::ReadWrite;
            } else {
                token.redirKind = RedirKind::Input;
            }
            return token;
        }

        // '>'
        ++pos_;
        if (pos_ < in_.size() && in_[pos_] == '>') {
            ++pos_;
            token.redirKind = RedirKind::Append;
        } else if (pos_ < in_.size() && in_[pos_] == '&') {
            ++pos_;
            token.redirKind = RedirKind::DupOutput;
        } else if (pos_ < in_.size() && in_[pos_] == '|') {
            ++pos_;
            token.redirKind = RedirKind::Output; // >| 覆盖 noclobber，按输出处理
        } else {
            token.redirKind = RedirKind::Output;
        }
        return token;
    }

    Token operatorToken() {
        Token token;
        token.type   = Token::Type::Operator;
        token.offset = pos_;
        const char c = in_[pos_];

        switch (c) {
        case ';':
            ++pos_;
            if (pos_ < in_.size() && in_[pos_] == ';') {
                ++pos_;
                if (pos_ < in_.size() && in_[pos_] == ';') ++pos_;
                return unsupportedToken(token.offset, "不支持 case 分隔符（;;/;;&），已拒绝");
            }
            token.op = Token::Op::Semicolon;
            return token;
        case '&':
            ++pos_;
            if (pos_ < in_.size() && in_[pos_] == '&') {
                ++pos_;
                token.op = Token::Op::AndAnd;
            } else {
                token.op = Token::Op::Ampersand;
            }
            return token;
        case '|':
            ++pos_;
            if (pos_ < in_.size() && in_[pos_] == '|') {
                ++pos_;
                token.op = Token::Op::OrOr;
            } else if (pos_ < in_.size() && in_[pos_] == '&') {
                ++pos_;
                return unsupportedToken(token.offset, "不支持 |& 管道，已拒绝");
            } else {
                token.op = Token::Op::Pipe;
            }
            return token;
        case '(':
            ++pos_;
            token.op = Token::Op::LParen;
            return token;
        case ')':
            ++pos_;
            token.op = Token::Op::RParen;
            return token;
        default:
            ++pos_;
            return unsupportedToken(token.offset, "无法识别的操作符");
        }
    }

    Token wordToken() {
        Token token;
        token.type   = Token::Type::Word;
        token.offset = pos_;

        Word              word;
        const std::size_t wordStart = pos_;
        while (pos_ < in_.size() && !isWordDelimiter(in_[pos_])) {
            const char c = in_[pos_];
            if (c == '\\') {
                word.escaped = true;
                if (pos_ + 1 >= in_.size()) {
                    setError(pos_, "命令以反斜杠结尾");
                    return unsupportedToken(token.offset, out_.error);
                }
                if (in_[pos_ + 1] == '\n') {
                    pos_ += 2;
                    continue;
                }
                appendLiteralPart(word.parts, in_[pos_ + 1]);
                pos_ += 2;
                continue;
            }
            if (c == '\'') {
                const std::size_t close = in_.find('\'', pos_ + 1);
                if (close == kNpos) {
                    setError(pos_, "单引号未闭合");
                    return unsupportedToken(token.offset, out_.error);
                }
                WordPart part;
                part.kind = WordPartKind::SingleQuoted;
                part.text = in_.substr(pos_ + 1, close - pos_ - 1);
                word.parts.push_back(std::move(part));
                pos_ = close + 1;
                continue;
            }
            if (c == '"') {
                ++pos_;
                WordPart quoted;
                quoted.kind = WordPartKind::DoubleQuoted;
                if (!parseDoubleQuoted(quoted)) return unsupportedToken(token.offset, out_.error);
                word.parts.push_back(std::move(quoted));
                continue;
            }
            if (c == '`') {
                if (!parseBacktick(word.parts)) return unsupportedToken(token.offset, out_.error);
                continue;
            }
            if (c == '$') {
                if (!parseDollarInto(word.parts, pos_)) return unsupportedToken(token.offset, out_.error);
                continue;
            }
            if (c == '~' && word.parts.empty() && pos_ == wordStart) {
                WordPart part;
                part.kind = WordPartKind::Tilde;
                part.text = "~";
                word.parts.push_back(std::move(part));
                pushFeature(out_.features, Feature::Tilde);
                ++pos_;
                continue;
            }
            if (c == '*' || c == '?' || c == '[') word.glob = true;
            appendLiteralPart(word.parts, c);
            ++pos_;
        }
        if (pos_ == wordStart) {
            setError(pos_, "空词");
            return unsupportedToken(token.offset, out_.error);
        }
        token.word = std::move(word);
        return token;
    }

    bool parseDoubleQuoted(WordPart& quoted) {
        while (pos_ < in_.size()) {
            const char c = in_[pos_];
            if (c == '"') {
                ++pos_;
                return true;
            }
            if (c == '\\') {
                if (pos_ + 1 >= in_.size()) {
                    setError(pos_, "双引号未闭合");
                    return false;
                }
                const char next = in_[pos_ + 1];
                if (next == '\n') {
                    pos_ += 2;
                    continue;
                }
                if (next == '$' || next == '`' || next == '"' || next == '\\') {
                    appendLiteralPart(quoted.inner, next);
                    pos_ += 2;
                    continue;
                }
                appendLiteralPart(quoted.inner, '\\');
                appendLiteralPart(quoted.inner, next);
                pos_ += 2;
                continue;
            }
            if (c == '`') {
                if (!parseBacktick(quoted.inner)) return false;
                continue;
            }
            if (c == '$') {
                if (!parseDollarInto(quoted.inner, pos_)) return false;
                continue;
            }
            appendLiteralPart(quoted.inner, c);
            ++pos_;
        }
        setError(pos_, "双引号未闭合");
        return false;
    }

    bool parseBacktick(std::vector<WordPart>& parts) {
        const std::size_t start = pos_;
        ++pos_;
        std::string raw;
        while (pos_ < in_.size()) {
            const char c = in_[pos_];
            if (c == '\\' && pos_ + 1 < in_.size()) {
                raw.push_back(c);
                raw.push_back(in_[pos_ + 1]);
                pos_ += 2;
                continue;
            }
            if (c == '`') {
                WordPart part;
                part.kind = WordPartKind::CommandSubstitution;
                part.text = raw;
                parts.push_back(std::move(part));
                pushFeature(out_.features, Feature::CommandSubstitution);
                ++pos_;
                return true;
            }
            raw.push_back(c);
            ++pos_;
        }
        setError(start, "反引号未闭合");
        return false;
    }

    bool parseDollarInto(std::vector<WordPart>& parts, std::size_t start) {
        if (start + 1 >= in_.size()) {
            appendLiteralPart(parts, '$');
            pos_ = start + 1;
            return true;
        }
        const char next = in_[start + 1];
        // busybox ash 支持 $'...'；本解析器无法安全建模其转义，必须拒绝。
        if (next == '\'') {
            setError(start, "不支持 $'...'（ANSI-C 引号），已拒绝");
            return false;
        }
        if (next == '"') {
            setError(start, "不支持 $\"...\"（本地化引号），已拒绝");
            return false;
        }
        if (next == '(') {
            if (start + 2 < in_.size() && in_[start + 2] == '(') {
                const std::size_t after = findArithmeticClose(start + 3);
                if (after == kNpos) {
                    setError(start, "算术展开 $((...)) 未闭合");
                    return false;
                }
                WordPart part;
                part.kind = WordPartKind::ArithmeticExpansion;
                part.text = in_.substr(start, after - start);
                parts.push_back(std::move(part));
                pushFeature(out_.features, Feature::ArithmeticExpansion);
                pos_ = after;
                return true;
            }
            const std::size_t close = findClosingParen(start + 2);
            if (close == kNpos) {
                setError(start, "命令替换 $(...) 未闭合");
                return false;
            }
            WordPart part;
            part.kind = WordPartKind::CommandSubstitution;
            part.text = in_.substr(start + 2, close - (start + 2));
            parts.push_back(std::move(part));
            pushFeature(out_.features, Feature::CommandSubstitution);
            pos_ = close + 1;
            return true;
        }
        if (next == '{') {
            const std::size_t close = findClosingBrace(start + 2);
            if (close == kNpos) {
                setError(start, "参数展开 ${...} 未闭合");
                return false;
            }
            WordPart part;
            part.kind = WordPartKind::ParameterExpansion;
            part.text = in_.substr(start, close + 1 - start);
            parts.push_back(std::move(part));
            pushFeature(out_.features, Feature::ParameterExpansion);
            pos_ = close + 1;
            return true;
        }
        if (isNameStart(next)) {
            std::size_t end = start + 2;
            while (end < in_.size() && isNameChar(in_[end])) ++end;
            WordPart part;
            part.kind = WordPartKind::ParameterExpansion;
            part.text = in_.substr(start, end - start);
            parts.push_back(std::move(part));
            pushFeature(out_.features, Feature::ParameterExpansion);
            pos_ = end;
            return true;
        }
        const bool special = next == '?' || next == '#' || next == '@' || next == '*' || next == '$' || next == '!'
                          || next == '-' || isDigitChar(next);
        if (special) {
            WordPart part;
            part.kind = WordPartKind::ParameterExpansion;
            part.text = in_.substr(start, 2);
            parts.push_back(std::move(part));
            pushFeature(out_.features, Feature::ParameterExpansion);
            pos_ = start + 2;
            return true;
        }
        appendLiteralPart(parts, '$');
        pos_ = start + 1;
        return true;
    }

    // 从 '(' 之后的 i 开始，找到与之配对的 ')'（跳过引号/转义/反引号，统计嵌套括号）。
    std::size_t findClosingParen(std::size_t i) const {
        int depth = 1;
        while (i < in_.size()) {
            const char c = in_[i];
            if (c == '\\') {
                i += 2;
                continue;
            }
            if (c == '\'') {
                ++i;
                while (i < in_.size() && in_[i] != '\'') ++i;
                if (i < in_.size()) ++i;
                continue;
            }
            if (c == '"') {
                ++i;
                while (i < in_.size() && in_[i] != '"') {
                    if (in_[i] == '\\') i += 2;
                    else ++i;
                }
                if (i < in_.size()) ++i;
                continue;
            }
            if (c == '`') {
                ++i;
                while (i < in_.size() && in_[i] != '`') {
                    if (in_[i] == '\\') i += 2;
                    else ++i;
                }
                if (i < in_.size()) ++i;
                continue;
            }
            if (c == '(') {
                ++depth;
                ++i;
                continue;
            }
            if (c == ')') {
                --depth;
                if (depth == 0) return i;
                ++i;
                continue;
            }
            ++i;
        }
        return kNpos;
    }

    std::size_t findClosingBrace(std::size_t i) const {
        int depth = 1;
        while (i < in_.size()) {
            const char c = in_[i];
            if (c == '\\') {
                i += 2;
                continue;
            }
            if (c == '\'' || c == '"' || c == '`') {
                const char quote = c;
                ++i;
                while (i < in_.size() && in_[i] != quote) {
                    if (in_[i] == '\\') i += 2;
                    else ++i;
                }
                if (i < in_.size()) ++i;
                continue;
            }
            if (c == '{') {
                ++depth;
                ++i;
                continue;
            }
            if (c == '}') {
                --depth;
                if (depth == 0) return i;
                ++i;
                continue;
            }
            ++i;
        }
        return kNpos;
    }

    std::size_t findArithmeticClose(std::size_t i) const {
        int depth = 0;
        while (i < in_.size()) {
            const char c = in_[i];
            if (c == '\\') {
                i += 2;
                continue;
            }
            if (c == '\'' || c == '"' || c == '`') {
                const char quote = c;
                ++i;
                while (i < in_.size() && in_[i] != quote) {
                    if (in_[i] == '\\') i += 2;
                    else ++i;
                }
                if (i < in_.size()) ++i;
                continue;
            }
            if (c == '(') {
                ++depth;
                ++i;
                continue;
            }
            if (c == ')') {
                if (depth > 0) {
                    --depth;
                    ++i;
                    continue;
                }
                if (i + 1 < in_.size() && in_[i + 1] == ')') return i + 2;
                ++i;
                continue;
            }
            ++i;
        }
        return kNpos;
    }
};

// ---------------------------------------------------------------------------
// Parser：list := and_or ((';'|'&'|newline) and_or)*
//          and_or := pipeline (('&&'|'||') pipeline)*
//          pipeline := simple ('|' simple)*
//          simple := assignment* word* redirection*
// ---------------------------------------------------------------------------

class Parser {
public:
    Parser(const std::string& input, ParseResult& out) : lexer_(input, out), out_(out) {}

    bool parse() {
        advance();
        for (;;) {
            while (isOp(Token::Op::Newline)) advance();
            if (failed()) return false;
            if (current_.type == Token::Type::End) break;
            if (current_.type == Token::Type::Unsupported) {
                fail(current_.unsupported);
                return false;
            }
            if (out_.list.items.size() >= kMaxListItems) {
                fail("命令项过多，已拒绝");
                return false;
            }

            ListItem item;
            if (!parseAndOr(item.andOr)) return false;

            if (isOp(Token::Op::Semicolon)) {
                advance();
                skipNewlines();
            } else if (isOp(Token::Op::Ampersand)) {
                item.background = true;
                pushFeature(out_.features, Feature::Background);
                advance();
                skipNewlines();
            } else if (isOp(Token::Op::Newline)) {
                advance();
                skipNewlines();
            } else if (current_.type == Token::Type::Unsupported) {
                fail(current_.unsupported);
                return false;
            } else if (current_.type != Token::Type::End) {
                fail("不支持的命令分隔或语法");
                return false;
            }
            if (failed()) return false;
            out_.list.items.push_back(std::move(item));
        }
        return !failed();
    }

private:
    Lexer        lexer_;
    ParseResult& out_;
    Token        current_;
    bool         failed_ = false;

    bool failed() const { return failed_ || lexer_.failed(); }

    bool isOp(Token::Op op) const { return current_.type == Token::Type::Operator && current_.op == op; }

    void advance() { current_ = lexer_.next(); }

    void skipNewlines() {
        while (isOp(Token::Op::Newline)) advance();
    }

    void fail(const std::string& message) {
        if (failed_) return;
        failed_ = true;
        if (out_.error.empty()) {
            out_.error       = message;
            out_.errorOffset = current_.offset;
        }
    }

    bool parseAndOr(AndOrList& list) {
        Pipeline first;
        if (!parsePipeline(first)) return false;
        list.pipelines.push_back(std::move(first));
        while (isOp(Token::Op::AndAnd) || isOp(Token::Op::OrOr)) {
            const bool isOr = isOp(Token::Op::OrOr);
            advance();
            skipNewlines();
            if (failed()) return false;
            Pipeline next;
            if (!parsePipeline(next)) return false;
            list.orOperators.push_back(isOr);
            list.pipelines.push_back(std::move(next));
            if (list.pipelines.size() > kMaxListItems) {
                fail("&&/|| 命令链过长");
                return false;
            }
        }
        return true;
    }

    bool parsePipeline(Pipeline& pipeline) {
        for (;;) {
            SimpleCommand command;
            if (!parseSimpleCommand(command)) return false;
            pipeline.commands.push_back(std::move(command));
            if (isOp(Token::Op::Pipe)) {
                advance();
                skipNewlines();
                if (failed()) return false;
                if (pipeline.commands.size() >= kMaxPipelineStages) {
                    fail("管道段数过多");
                    return false;
                }
                continue;
            }
            return true;
        }
    }

    bool parseSimpleCommand(SimpleCommand& command) {
        for (;;) {
            if (failed()) return false;
            if (current_.type == Token::Type::Unsupported) {
                fail(current_.unsupported);
                return false;
            }
            if (current_.type == Token::Type::Word) {
                if (command.argv.empty()) {
                    std::string name;
                    if (assignmentName(current_.word, name)) {
                        command.assignments.push_back(current_.word.render());
                        pushFeature(out_.features, Feature::Assignment);
                        advance();
                        continue;
                    }
                }
                command.argv.push_back(current_.word);
                if (command.argv.size() > kMaxArguments) {
                    fail("参数过多");
                    return false;
                }
                advance();
                continue;
            }
            if (current_.type == Token::Type::Redirection) {
                Redirection redir;
                redir.kind = current_.redirKind;
                redir.fd   = current_.redirFd;
                advance();
                if (current_.type != Token::Type::Word) {
                    fail("重定向缺少目标");
                    return false;
                }
                redir.target = current_.word;
                advance();
                if (redir.kind == RedirKind::Heredoc || redir.kind == RedirKind::HereString) {
                    fail("不支持 heredoc / here-string");
                    return false;
                }
                command.redirections.push_back(std::move(redir));
                pushFeature(out_.features, Feature::Redirection);
                continue;
            }
            if (isOp(Token::Op::LParen) || isOp(Token::Op::RParen)) {
                fail("不支持子 shell / 函数定义 / 分组语法");
                return false;
            }
            break;
        }
        if (command.argv.empty() && command.redirections.empty() && command.assignments.empty()) {
            fail("空命令");
            return false;
        }
        return true;
    }
};

// ---------------------------------------------------------------------------
// 摘要 / 警告
// ---------------------------------------------------------------------------

std::string summarizeSimpleCommand(const SimpleCommand& command) {
    std::string out;
    for (const std::string& assignment : command.assignments) {
        if (!out.empty()) out += ' ';
        out += assignment;
    }
    for (const Word& word : command.argv) {
        if (!out.empty() && out.back() != ' ') out += ' ';
        out += word.isFullyLiteral() ? shellQuote(word.value()) : word.render();
    }
    for (const Redirection& redir : command.redirections) {
        if (!out.empty()) out += ' ';
        if (redir.fd >= 0) out += std::to_string(redir.fd);
        out += redirKindText(redir.kind);
        out += redir.target.isFullyLiteral() ? shellQuote(redir.target.value()) : redir.target.render();
    }
    return out;
}

std::string summarizeCommandList(const CommandList& list) {
    std::string out;
    for (std::size_t i = 0; i < list.items.size(); ++i) {
        if (i > 0) out += "; ";
        const ListItem& item = list.items[i];
        for (std::size_t p = 0; p < item.andOr.pipelines.size(); ++p) {
            if (p > 0)
                out += (p - 1 < item.andOr.orOperators.size() && item.andOr.orOperators[p - 1]) ? " || " : " && ";
            const Pipeline& pipeline = item.andOr.pipelines[p];
            for (std::size_t c = 0; c < pipeline.commands.size(); ++c) {
                if (c > 0) out += " | ";
                out += summarizeSimpleCommand(pipeline.commands[c]);
            }
        }
        if (item.background) out += '&';
    }
    if (out.size() > 512) out = out.substr(0, 512) + "…";
    return out;
}

const std::set<std::string>& riskyCommandNames() {
    static const std::set<std::string> names = {
        "sh",       "bash",     "ash",   "dash",     "zsh",    "busybox",  "env",     "eval",      "exec",
        "xargs",    "find",     "nohup", "setsid",   "disown", "perl",     "python",  "python3",   "ruby",
        "node",     "awk",      "gawk",  "mawk",     "sed",    "chmod",    "chown",   "rm",        "rmdir",
        "dd",       "mkfs",     "mount", "umount",   "reboot", "poweroff", "halt",    "shutdown",  "kill",
        "killall",  "pkill",    "tee",   "cp",       "mv",     "ln",       "install", "systemctl", "init",
        "iptables", "ifconfig", "route", "scp",      "ssh",    "telnet",   "nc",      "ncat",      "socat",
        "base64",   "xxd",      "od",    "truncate", "shred",  "wipefs",   "parted",  "fdisk",     "blkid",
    };
    return names;
}

const std::set<std::string>& reservedWords() {
    static const std::set<std::string> words = {
        "if",
        "then",
        "else",
        "elif",
        "fi",
        "for",
        "while",
        "until",
        "do",
        "done",
        "case",
        "esac",
        "in",
        "select",
        "function",
        "time",
    };
    return words;
}

std::vector<std::string> confirmWarnings(const ParseResult& parsed) {
    std::vector<std::string> warnings;
    std::set<std::string>    risky;
    std::set<std::string>    reserved;
    std::vector<std::string> redirectionTargets;

    for (Feature feature : parsed.features) {
        switch (feature) {
        case Feature::CommandSubstitution:
            warnings.push_back("包含命令替换（$() 或反引号）：会执行你看不到的隐藏命令，实际行为可能远超此处显示");
            break;
        case Feature::ParameterExpansion:
        case Feature::ArithmeticExpansion:
            warnings.push_back("包含变量/算术展开：实际执行的参数可能与显示文本不同（可能被用于绕过关键词检查）");
            break;
        case Feature::Background:
            warnings.push_back("包含后台执行（&）：命令可能在本次工具调用返回后继续运行");
            break;
        case Feature::Assignment:
            warnings.push_back("包含环境变量赋值：可能影响命令解析、动态链接库（LD_*）或后续命令行为");
            break;
        case Feature::Glob:
            warnings.push_back("包含未加引号的通配符（* ? [）：会被展开成本地文件列表，可能读取/枚举文件");
            break;
        case Feature::Tilde:
            warnings.push_back("包含 ~ 主目录展开");
            break;
        case Feature::Heredoc:
        case Feature::HereString:
            warnings.push_back("包含 heredoc / here-string");
            break;
        default:
            break;
        }
    }

    if (!parsed.list.items.empty()) {
        for (const ListItem& item : parsed.list.items) {
            for (const Pipeline& pipeline : item.andOr.pipelines) {
                for (const SimpleCommand& command : pipeline.commands) {
                    if (!command.argv.empty()) {
                        const Word&       first = command.argv.front();
                        const std::string name  = first.isFullyLiteral() ? first.value() : first.render();
                        if (riskyCommandNames().count(name)) risky.insert(name);
                        if (reservedWords().count(name)) reserved.insert(name);
                    }
                    for (const Redirection& redir : command.redirections) {
                        if (redir.target.isFullyLiteral()) redirectionTargets.push_back(redir.target.value());
                    }
                }
            }
        }
    }

    if (!risky.empty()) {
        std::string list;
        for (const std::string& name : risky) {
            if (!list.empty()) list += "、";
            list += name;
        }
        warnings.push_back("包含高风险/解释器/包装命令：" + list + "（请确认你信任其完整参数）");
    }
    if (!reserved.empty()) {
        std::string list;
        for (const std::string& name : reserved) {
            if (!list.empty()) list += "、";
            list += name;
        }
        warnings.push_back("包含 shell 复合语法关键字：" + list + "（实际语义可能比单行显示复杂）");
    }
    if (!redirectionTargets.empty()) {
        std::string list;
        for (const std::string& target : redirectionTargets) {
            if (target == "/dev/null") continue;
            if (!list.empty()) list += "、";
            list += target;
        }
        if (!list.empty()) warnings.push_back("包含文件重定向，目标：" + list + "（可能读取或写入本地文件）");
    }
    return warnings;
}

// ---------------------------------------------------------------------------
// 允许列表与参数审计
// ---------------------------------------------------------------------------

bool isAutomaticCommandAllowed(const std::string& name) {
    static const std::set<std::string> allowed =
        {"cat", "curl", "cut", "grep", "head", "sort", "tail", "tr", "uniq", "wc"};
    return allowed.count(name) != 0;
}

std::string allowedCommandList() { return "curl、head、tail、grep、cut、tr、sort、uniq、wc、cat"; }

bool isPrivateIpv4(const unsigned parts[4]) {
    if (parts[0] == 0 || parts[0] == 10 || parts[0] == 127) return true;
    if (parts[0] == 169 && parts[1] == 254) return true;
    if (parts[0] == 172 && parts[1] >= 16 && parts[1] <= 31) return true;
    if (parts[0] == 192 && parts[1] == 168) return true;
    if (parts[0] == 100 && parts[1] >= 64 && parts[1] <= 127) return true;
    if (parts[0] == 192 && parts[1] == 0 && parts[2] == 0) return true;
    if (parts[0] == 198 && (parts[1] == 18 || parts[1] == 19)) return true;
    if (parts[0] >= 224) return true;
    return false;
}

// 只允许 http/https、公网（非字面内网地址）、无 userinfo 的 URL。
// 注意：这是尽力而为的检查，DNS 解析结果/HTTP 重定向到内网不在本函数覆盖范围内。
bool isSafeHttpUrl(const std::string& url, std::string& why) {
    if (url.empty()) {
        why = "URL 为空";
        return false;
    }
    if (url.size() > 2048) {
        why = "URL 过长";
        return false;
    }
    for (char raw : url) {
        const auto c = static_cast<unsigned char>(raw);
        if (c <= 0x20 || c == 0x7f) {
            why = "URL 含空白或控制字符";
            return false;
        }
    }
    if (url.find('\\') != std::string::npos) {
        why = "URL 含反斜杠";
        return false;
    }
    const std::size_t schemeEnd = url.find("://");
    if (schemeEnd == std::string::npos) {
        why = "URL 缺少 scheme（仅允许 http/https）";
        return false;
    }
    const std::string scheme = lowerAscii(url.substr(0, schemeEnd));
    if (scheme != "http" && scheme != "https") {
        why = "仅允许 http/https URL（不支持 " + scheme + "）";
        return false;
    }
    const std::size_t authorityStart = schemeEnd + 3;
    std::size_t       authorityEnd   = url.find_first_of("/?#", authorityStart);
    if (authorityEnd == std::string::npos) authorityEnd = url.size();
    const std::string authority = url.substr(authorityStart, authorityEnd - authorityStart);
    if (authority.empty()) {
        why = "URL 缺少主机名";
        return false;
    }
    if (authority.find('@') != std::string::npos) {
        why = "URL 不允许 userinfo（@）";
        return false;
    }
    if (authority.front() == '[') {
        why = "不支持 IPv6 字面量 URL";
        return false;
    }

    std::string       host;
    const std::size_t colon = authority.rfind(':');
    if (colon != std::string::npos) {
        host                   = authority.substr(0, colon);
        const std::string port = authority.substr(colon + 1);
        if (port.empty() || !isDigitsOnly(port) || port.size() > 5) {
            why = "URL 端口格式非法";
            return false;
        }
        long value = 0;
        for (char c : port) value = value * 10 + (c - '0');
        if (value <= 0 || value > 65535) {
            why = "URL 端口超出范围";
            return false;
        }
    } else {
        host = authority;
    }
    if (host.empty()) {
        why = "URL 缺少主机名";
        return false;
    }
    if (host.size() > 253) {
        why = "主机名过长";
        return false;
    }
    const std::string lowerHost = lowerAscii(host);
    for (char c : lowerHost) {
        const bool ok = (c >= 'a' && c <= 'z') || isDigitChar(c) || c == '.' || c == '-';
        if (!ok) {
            why = "主机名包含不允许的字符";
            return false;
        }
    }
    std::string checkHost = lowerHost;
    if (!checkHost.empty() && checkHost.back() == '.') checkHost.pop_back();
    if (checkHost.empty()) {
        why = "主机名为空";
        return false;
    }
    if (checkHost == "localhost" || checkHost.rfind("localhost.", 0) == 0 || endsWith(checkHost, ".localhost")) {
        why = "不允许访问 localhost";
        return false;
    }
    static const char* kBlockedSuffixes[] = {".local", ".localdomain", ".internal", ".lan", ".home", ".home.arpa"};
    for (const char* suffix : kBlockedSuffixes) {
        if (endsWith(checkHost, suffix)) {
            why = std::string("不允许访问内网域名（") + suffix + "）";
            return false;
        }
    }

    bool ipv4Candidate = true;
    for (char c : checkHost) {
        if (!isDigitChar(c) && c != '.') {
            ipv4Candidate = false;
            break;
        }
    }
    if (ipv4Candidate) {
        unsigned    parts[4] = {0, 0, 0, 0};
        int         count    = 0;
        std::size_t start    = 0;
        for (;;) {
            if (count >= 4) {
                why = "非法 IPv4 地址";
                return false;
            }
            const std::size_t dot = checkHost.find('.', start);
            const std::string segment =
                checkHost.substr(start, dot == std::string::npos ? std::string::npos : dot - start);
            if (segment.empty() || segment.size() > 3) {
                why = "非法 IPv4 地址";
                return false;
            }
            unsigned value = 0;
            for (char c : segment) value = value * 10 + (c - '0');
            if (value > 255) {
                why = "非法 IPv4 地址";
                return false;
            }
            parts[count++] = value;
            if (dot == std::string::npos) break;
            start = dot + 1;
        }
        if (count != 4) {
            why = "非法 IPv4 地址";
            return false;
        }
        if (isPrivateIpv4(parts)) {
            why = "不允许访问内网/保留 IPv4 地址";
            return false;
        }
    } else if (checkHost.find('.') == std::string::npos) {
        why = "不允许单段主机名（可能解析到内网主机）";
        return false;
    }
    return true;
}

struct CurlOptionSpec {
    bool        takesValue = false;
    const char* valueKind  = nullptr; // "number" / "decimal" / "rate" / "range" / "text" / "url"
};

const std::map<std::string, CurlOptionSpec>& curlLongOptions() {
    static const std::map<std::string, CurlOptionSpec> options = {
        {"--silent",          {false, nullptr} },
        {"--show-error",      {false, nullptr} },
        {"--location",        {false, nullptr} },
        {"--compressed",      {false, nullptr} },
        {"--fail",            {false, nullptr} },
        {"--include",         {false, nullptr} },
        {"--head",            {false, nullptr} },
        {"--verbose",         {false, nullptr} },
        {"--globoff",         {false, nullptr} },
        {"--http1.0",         {false, nullptr} },
        {"--http1.1",         {false, nullptr} },
        {"--ipv4",            {false, nullptr} },
        {"--disable",         {false, nullptr} },
        {"--progress-bar",    {false, nullptr} },
        {"--no-buffer",       {false, nullptr} },
        {"--max-time",        {true, "decimal"}},
        {"--connect-timeout", {true, "decimal"}},
        {"--retry",           {true, "number"} },
        {"--retry-delay",     {true, "number"} },
        {"--retry-max-time",  {true, "number"} },
        {"--max-redirs",      {true, "number"} },
        {"--max-filesize",    {true, "number"} },
        {"--limit-rate",      {true, "rate"}   },
        {"--range",           {true, "range"}  },
        {"--user-agent",      {true, "text"}   },
        {"--header",          {true, "text"}   },
        {"--referer",         {true, "text"}   },
        {"--write-out",       {true, "text"}   },
        {"--url",             {true, "url"}    },
    };
    return options;
}

const std::map<char, std::string>& curlShortValueOptions() {
    static const std::map<char, std::string> options = {
        {'A', "--user-agent"},
        {'e', "--referer"   },
        {'m', "--max-time"  },
        {'w', "--write-out" },
        {'r', "--range"     },
        {'H', "--header"    },
    };
    return options;
}

const std::set<char>& curlShortFlagOptions() {
    static const std::set<char> flags = {'s', 'S', 'L', 'f', 'i', 'I', 'v', 'g', 'q', '#'};
    return flags;
}

bool auditCurlValue(const std::string& option, const CurlOptionSpec& spec, const std::string& value, std::string& why) {
    if (value.empty()) {
        why = "curl 选项 " + option + " 缺少取值";
        return false;
    }
    if (value.front() == '@') {
        why = "curl 选项 " + option + " 不允许 @ 文件读取";
        return false;
    }
    const std::string kind = spec.valueKind == nullptr ? "text" : spec.valueKind;
    if (kind == "number" || kind == "decimal") {
        const bool ok = (kind == "number") ? isPlainNumber(value) : isNonNegativeDecimal(value);
        if (!ok) {
            why = "curl 选项 " + option + " 的取值必须是数字";
            return false;
        }
        return true;
    }
    if (kind == "rate") {
        if (!isRateValue(value)) {
            why = "curl 选项 " + option + " 的速率格式非法";
            return false;
        }
        return true;
    }
    if (kind == "range") {
        if (!isRangeValue(value)) {
            why = "curl 选项 " + option + " 的范围格式非法";
            return false;
        }
        return true;
    }
    if (kind == "url") {
        std::string urlWhy;
        if (!isSafeHttpUrl(value, urlWhy)) {
            why = "curl --url 参数不被允许：" + urlWhy;
            return false;
        }
        return true;
    }
    // text：请求头/user-agent/write-out 等；拒绝控制字符（防 CRLF 注入）与过长值
    if (hasControlChars(value) || value.size() > 2048) {
        why = "curl 选项 " + option + " 的取值包含控制字符或过长";
        return false;
    }
    return true;
}

enum class ArgAuditResult { Ok, BadArguments, BadUrl };

ArgAuditResult auditCurlArguments(const std::vector<std::string>& args, std::string& why) {
    std::size_t urlCount     = 0;
    bool        endOfOptions = false;

    for (std::size_t i = 0; i < args.size(); ++i) {
        const std::string& arg = args[i];
        if (endOfOptions) {
            std::string urlWhy;
            if (!isSafeHttpUrl(arg, urlWhy)) {
                why = "URL 不被允许：" + urlWhy;
                return ArgAuditResult::BadUrl;
            }
            ++urlCount;
            continue;
        }
        if (arg == "--") {
            endOfOptions = true;
            continue;
        }
        if (arg.size() >= 2 && arg[0] == '-' && arg[1] == '-') {
            std::string       name = arg;
            std::string       value;
            bool              hasValue = false;
            const std::size_t eq       = arg.find('=');
            if (eq != std::string::npos) {
                name     = arg.substr(0, eq);
                value    = arg.substr(eq + 1);
                hasValue = true;
            }
            const auto it = curlLongOptions().find(name);
            if (it == curlLongOptions().end()) {
                why = "automatic 模式不允许 curl 选项 " + name;
                return ArgAuditResult::BadArguments;
            }
            const CurlOptionSpec& spec = it->second;
            if (!spec.takesValue) {
                if (hasValue) {
                    why = "curl 选项 " + name + " 不接受取值";
                    return ArgAuditResult::BadArguments;
                }
                continue;
            }
            if (!hasValue) {
                if (i + 1 >= args.size()) {
                    why = "curl 选项 " + name + " 缺少取值";
                    return ArgAuditResult::BadArguments;
                }
                value = args[++i];
            }
            if (!auditCurlValue(name, spec, value, why)) {
                return name == "--url" ? ArgAuditResult::BadUrl : ArgAuditResult::BadArguments;
            }
            if (name == "--url") ++urlCount;
            continue;
        }
        if (arg.size() >= 2 && arg[0] == '-') {
            std::size_t j = 1;
            while (j < arg.size()) {
                const char c = arg[j];
                if (curlShortFlagOptions().count(c)) {
                    ++j;
                    continue;
                }
                const auto it = curlShortValueOptions().find(c);
                if (it == curlShortValueOptions().end()) {
                    why = std::string("automatic 模式不允许 curl 选项 -") + c;
                    return ArgAuditResult::BadArguments;
                }
                std::string value = arg.substr(j + 1);
                if (value.empty()) {
                    if (i + 1 >= args.size()) {
                        why = std::string("curl 选项 -") + c + " 缺少取值";
                        return ArgAuditResult::BadArguments;
                    }
                    value = args[++i];
                }
                const auto specIt = curlLongOptions().find(it->second);
                if (specIt == curlLongOptions().end()) {
                    why = "内部策略表错误";
                    return ArgAuditResult::BadArguments;
                }
                if (!auditCurlValue(it->second, specIt->second, value, why)) {
                    return it->second == "--url" ? ArgAuditResult::BadUrl : ArgAuditResult::BadArguments;
                }
                break;
            }
            continue;
        }
        // 普通参数：只允许 http(s) 公网 URL
        std::string urlWhy;
        if (!isSafeHttpUrl(arg, urlWhy)) {
            why = "URL 不被允许：" + urlWhy;
            return ArgAuditResult::BadUrl;
        }
        ++urlCount;
    }

    if (urlCount == 0) {
        why = "curl 缺少 http/https URL";
        return ArgAuditResult::BadUrl;
    }
    if (urlCount > 4) {
        why = "单条 curl 命令的 URL 数量过多";
        return ArgAuditResult::BadArguments;
    }
    return ArgAuditResult::Ok;
}

// ---- 文本过滤命令 ----

enum class FilterValueKind { Text, Number, Index };

struct FilterSpec {
    std::set<char>                         shortNoValue;
    std::map<char, FilterValueKind>        shortValue;
    std::set<std::string>                  longNoValue;
    std::map<std::string, FilterValueKind> longValue;
    bool                                   allowLegacyCount = false;
};

FilterSpec headTailSpec() {
    FilterSpec spec;
    spec.shortNoValue = {'q', 'v'};
    spec.shortValue   = {
        {'n', FilterValueKind::Index},
        {'c', FilterValueKind::Index}
    };
    spec.longNoValue = {"--quiet", "--verbose"};
    spec.longValue   = {
        {"--lines", FilterValueKind::Index},
        {"--bytes", FilterValueKind::Index}
    };
    spec.allowLegacyCount = true;
    return spec;
}

FilterSpec grepSpec() {
    FilterSpec spec;
    spec.shortNoValue = {'i', 'v', 'c', 'n', 'H', 'h', 'o', 'E', 'F', 'G', 'P', 'w',
                         'x', 's', 'q', 'a', 'b', 'l', 'L', 'I', 'U', 'Z', 'z'};
    spec.shortValue   = {
        {'e', FilterValueKind::Text  },
        {'m', FilterValueKind::Number},
        {'A', FilterValueKind::Number},
        {'B', FilterValueKind::Number},
        {'C', FilterValueKind::Number}
    };
    spec.longNoValue = {
        "--ignore-case",
        "--invert-match",
        "--count",
        "--line-number",
        "--with-filename",
        "--no-filename",
        "--only-matching",
        "--extended-regexp",
        "--fixed-strings",
        "--word-regexp",
        "--line-regexp",
        "--silent",
        "--quiet",
        "--text",
        "--byte-offset",
        "--files-with-matches",
        "--files-without-match"
    };
    spec.longValue = {
        {"--regexp",         FilterValueKind::Text  },
        {"--max-count",      FilterValueKind::Number},
        {"--after-context",  FilterValueKind::Number},
        {"--before-context", FilterValueKind::Number},
        {"--context",        FilterValueKind::Number}
    };
    return spec;
}

FilterSpec cutSpec() {
    FilterSpec spec;
    spec.shortNoValue = {'s'};
    spec.shortValue   = {
        {'b', FilterValueKind::Text},
        {'c', FilterValueKind::Text},
        {'f', FilterValueKind::Text},
        {'d', FilterValueKind::Text}
    };
    spec.longNoValue = {"--complement"};
    spec.longValue   = {
        {"--output-delimiter", FilterValueKind::Text}
    };
    return spec;
}

FilterSpec sortSpec() {
    FilterSpec spec;
    spec.shortNoValue = {'n', 'r', 'u', 'f', 'i', 'b', 'M', 'h', 'V', 's', 'g', 'c', 'C', 'z'};
    spec.shortValue   = {
        {'k', FilterValueKind::Text},
        {'t', FilterValueKind::Text}
    };
    spec.longNoValue = {
        "--numeric-sort",
        "--reverse",
        "--unique",
        "--ignore-case",
        "--ignore-leading-blanks",
        "--month-sort",
        "--human-numeric-sort",
        "--version-sort",
        "--stable",
        "--general-numeric-sort",
        "--check",
        "--zero-terminated"
    };
    spec.longValue = {
        {"--key",             FilterValueKind::Text},
        {"--field-separator", FilterValueKind::Text}
    };
    return spec;
}

FilterSpec uniqSpec() {
    FilterSpec spec;
    spec.shortNoValue = {'c', 'd', 'u', 'i', 'z'};
    spec.shortValue   = {
        {'w', FilterValueKind::Number},
        {'s', FilterValueKind::Number},
        {'f', FilterValueKind::Number}
    };
    spec.longNoValue = {"--count", "--repeated", "--unique", "--ignore-case", "--zero-terminated"};
    spec.longValue   = {
        {"--skip-fields", FilterValueKind::Number},
        {"--skip-chars",  FilterValueKind::Number},
        {"--check-chars", FilterValueKind::Number}
    };
    return spec;
}

FilterSpec wcSpec() {
    FilterSpec spec;
    spec.shortNoValue = {'l', 'w', 'c', 'm', 'L'};
    spec.longNoValue  = {"--lines", "--words", "--bytes", "--chars", "--max-line-length"};
    return spec;
}

FilterSpec catSpec() {
    FilterSpec spec;
    spec.shortNoValue = {'n', 'b', 's', 'v', 'E', 'T', 'A', 'e', 't', 'u'};
    spec.longNoValue  = {
        "--number",
        "--number-nonblank",
        "--squeeze-blank",
        "--show-nonprinting",
        "--show-ends",
        "--show-tabs",
        "--show-all"
    };
    return spec;
}

bool auditFilterValue(
    FilterValueKind    kind,
    const std::string& commandName,
    const std::string& option,
    const std::string& value,
    std::string&       why
) {
    if (value.empty()) {
        why = commandName + " 选项 " + option + " 缺少取值";
        return false;
    }
    if (kind == FilterValueKind::Number || kind == FilterValueKind::Index) {
        const bool ok = (kind == FilterValueKind::Number) ? isPlainNumber(value) : isIndexValue(value);
        if (!ok) {
            why = commandName + " 选项 " + option + " 的取值必须是数字";
            return false;
        }
        return true;
    }
    if (hasControlChars(value) || value.size() > 4096) {
        why = commandName + " 选项 " + option + " 的取值包含控制字符或过长";
        return false;
    }
    return true;
}

struct FilterParseOutcome {
    std::vector<std::string> operands;
    bool                     patternFromOption = false;
};

bool auditFilterArguments(
    const std::string&              commandName,
    const FilterSpec&               spec,
    const std::vector<std::string>& args,
    FilterParseOutcome&             outcome,
    std::string&                    why
) {
    bool endOfOptions = false;
    for (std::size_t i = 0; i < args.size(); ++i) {
        const std::string& arg = args[i];
        if (endOfOptions) {
            outcome.operands.push_back(arg);
            continue;
        }
        if (arg == "--") {
            endOfOptions = true;
            continue;
        }
        if (spec.allowLegacyCount && arg.size() > 1 && arg[0] == '-' && isDigitsOnly(arg.substr(1))) continue;
        if (arg.size() >= 2 && arg[0] == '-' && arg[1] == '-') {
            std::string       name = arg;
            std::string       value;
            bool              hasValue = false;
            const std::size_t eq       = arg.find('=');
            if (eq != std::string::npos) {
                name     = arg.substr(0, eq);
                value    = arg.substr(eq + 1);
                hasValue = true;
            }
            if (spec.longNoValue.count(name)) {
                if (hasValue) {
                    why = commandName + " 选项 " + name + " 不接受取值";
                    return false;
                }
                continue;
            }
            const auto it = spec.longValue.find(name);
            if (it == spec.longValue.end()) {
                why = "automatic 模式不允许 " + commandName + " 选项 " + name;
                return false;
            }
            if (!hasValue) {
                if (i + 1 >= args.size()) {
                    why = commandName + " 选项 " + name + " 缺少取值";
                    return false;
                }
                value = args[++i];
            }
            if (!auditFilterValue(it->second, commandName, name, value, why)) return false;
            if (name == "--regexp") outcome.patternFromOption = true;
            continue;
        }
        if (arg.size() >= 2 && arg[0] == '-') {
            std::size_t j = 1;
            while (j < arg.size()) {
                const char c = arg[j];
                if (spec.shortNoValue.count(c)) {
                    ++j;
                    continue;
                }
                const auto it = spec.shortValue.find(c);
                if (it == spec.shortValue.end()) {
                    why = std::string("automatic 模式不允许 ") + commandName + " 选项 -" + c;
                    return false;
                }
                std::string value = arg.substr(j + 1);
                if (value.empty()) {
                    if (i + 1 >= args.size()) {
                        why = std::string(commandName) + " 选项 -" + c + " 缺少取值";
                        return false;
                    }
                    value = args[++i];
                }
                const std::string option = std::string("-") + c;
                if (!auditFilterValue(it->second, commandName, option, value, why)) return false;
                if (c == 'e') outcome.patternFromOption = true;
                break;
            }
            continue;
        }
        outcome.operands.push_back(arg);
    }

    if (commandName == "cat") {
        if (outcome.operands.size() > 1) {
            why = "cat 只允许 '-' 表示标准输入";
            return false;
        }
        for (const std::string& operand : outcome.operands) {
            if (operand != "-") {
                why = "automatic 模式不允许 cat 读取文件参数";
                return false;
            }
        }
        return true;
    }
    if (commandName == "grep") {
        const std::size_t maxOperands = outcome.patternFromOption ? 0 : 1;
        if (!outcome.patternFromOption && outcome.operands.empty()) {
            why = "grep 缺少匹配模式";
            return false;
        }
        if (outcome.operands.size() > maxOperands) {
            why = "automatic 模式不允许 grep 读取文件/目录参数";
            return false;
        }
        return true;
    }
    if (!outcome.operands.empty()) {
        why = "automatic 模式不允许 " + commandName + " 读取文件/目录参数";
        return false;
    }
    return true;
}

bool auditRedirection(const Redirection& redir, std::string& why) {
    if (redir.kind == RedirKind::Heredoc || redir.kind == RedirKind::HereString || redir.kind == RedirKind::ReadWrite
        || redir.kind == RedirKind::OutputAndErr || redir.kind == RedirKind::AppendAndErr) {
        why = "automatic 模式禁止该类型的重定向";
        return false;
    }
    if (redir.fd != -1 && (redir.fd < 0 || redir.fd > 2)) {
        why = "automatic 模式只允许 0/1/2 号文件描述符重定向";
        return false;
    }
    if (!redir.target.isFullyLiteral()) {
        why = "重定向目标必须是字面路径";
        return false;
    }
    if (redir.target.glob) {
        why = "重定向目标不允许通配符";
        return false;
    }
    const std::string target = redir.target.value();
    if (redir.kind == RedirKind::DupOutput || redir.kind == RedirKind::DupInput) {
        if (target.size() != 1 || target[0] < '0' || target[0] > '2') {
            why = "automatic 模式只允许 2>&1 / 1>&2 这类 fd 复制";
            return false;
        }
        return true;
    }
    if (target != "/dev/null") {
        why = "automatic 模式禁止文件重定向/输入重定向（仅允许 /dev/null）";
        return false;
    }
    return true;
}

Decision makeDeny(DecisionCode code, std::string reason) {
    Decision decision;
    decision.allowed      = false;
    decision.needsConfirm = false;
    decision.code         = code;
    decision.reason       = std::move(reason);
    return decision;
}

Decision evaluateAutomatic(const ParseResult& parsed) {
    if (parsed.list.items.empty()) return makeDeny(DecisionCode::DeniedEmpty, "命令为空，已拒绝");

    for (const ListItem& item : parsed.list.items) {
        if (item.background) {
            return makeDeny(
                DecisionCode::DeniedBackground,
                "automatic 模式禁止后台执行（& / nohup / setsid / disown 等），已拒绝"
            );
        }
        if (item.andOr.pipelines.size() > kMaxListItems) {
            return makeDeny(DecisionCode::DeniedTooComplex, "命令链过长，已拒绝");
        }
        for (const Pipeline& pipeline : item.andOr.pipelines) {
            if (pipeline.commands.size() > kMaxPipelineStages) {
                return makeDeny(DecisionCode::DeniedTooComplex, "管道段数过多，已拒绝");
            }
            for (const SimpleCommand& command : pipeline.commands) {
                if (!command.assignments.empty()) {
                    return makeDeny(
                        DecisionCode::DeniedAssignment,
                        "automatic 模式禁止环境变量赋值（可能影响命令解析或动态链接），已拒绝"
                    );
                }
                for (const Redirection& redirection : command.redirections) {
                    std::string why;
                    if (!auditRedirection(redirection, why)) return makeDeny(DecisionCode::DeniedRedirection, why);
                }
                if (command.argv.empty()) {
                    return makeDeny(DecisionCode::DeniedArguments, "命令缺少可执行程序（仅含重定向），已拒绝");
                }
                for (const Word& word : command.argv) {
                    if (word.hasExpansion()) {
                        return makeDeny(
                            DecisionCode::DeniedExpansion,
                            "automatic 模式禁止变量/命令/算术/主目录展开，已拒绝"
                        );
                    }
                    if (word.glob) {
                        return makeDeny(
                            DecisionCode::DeniedGlob,
                            "automatic 模式禁止未加引号的通配符（* ? [），已拒绝"
                        );
                    }
                }
                if (command.argv.size() - 1 > kMaxArguments) {
                    return makeDeny(DecisionCode::DeniedTooComplex, "参数过多，已拒绝");
                }
                const Word& nameWord = command.argv.front();
                if (nameWord.parts.size() != 1 || nameWord.parts.front().kind != WordPartKind::Literal
                    || nameWord.escaped) {
                    return makeDeny(
                        DecisionCode::DeniedCommandNotAllowed,
                        "automatic 模式要求命令名为无引号、无转义的字面命令名，已拒绝"
                    );
                }
                const std::string name = nameWord.parts.front().text;
                if (name.find('/') != std::string::npos) {
                    return makeDeny(
                        DecisionCode::DeniedCommandNotAllowed,
                        "automatic 模式不允许以路径形式调用命令，已拒绝"
                    );
                }
                if (!isAutomaticCommandAllowed(name)) {
                    return makeDeny(
                        DecisionCode::DeniedCommandNotAllowed,
                        "命令 " + name + " 不在 automatic 模式允许列表（" + allowedCommandList() + "）内，已拒绝"
                    );
                }

                std::vector<std::string> args;
                args.reserve(command.argv.size());
                for (std::size_t i = 1; i < command.argv.size(); ++i) args.push_back(command.argv[i].value());

                if (name == "tr") continue; // tr 只读写标准输入输出，无文件/执行参数
                std::string why;
                if (name == "curl") {
                    const ArgAuditResult result = auditCurlArguments(args, why);
                    if (result == ArgAuditResult::BadUrl) return makeDeny(DecisionCode::DeniedUrl, why);
                    if (result == ArgAuditResult::BadArguments) return makeDeny(DecisionCode::DeniedArguments, why);
                    continue;
                }
                FilterSpec spec;
                if (name == "head" || name == "tail") spec = headTailSpec();
                else if (name == "grep") spec = grepSpec();
                else if (name == "cut") spec = cutSpec();
                else if (name == "sort") spec = sortSpec();
                else if (name == "uniq") spec = uniqSpec();
                else if (name == "wc") spec = wcSpec();
                else if (name == "cat") spec = catSpec();
                else return makeDeny(DecisionCode::DeniedCommandNotAllowed, "命令不在允许列表内，已拒绝");

                FilterParseOutcome outcome;
                if (!auditFilterArguments(name, spec, args, outcome, why)) {
                    return makeDeny(DecisionCode::DeniedArguments, why);
                }
            }
        }
    }

    Decision decision;
    decision.allowed      = true;
    decision.needsConfirm = false;
    decision.code         = DecisionCode::AllowedAutomatic;
    decision.reason       = "automatic 模式策略放行（只读网页抓取）";
    return decision;
}

} // namespace

// ---------------------------------------------------------------------------
// Word 方法
// ---------------------------------------------------------------------------

bool Word::hasExpansion() const { return partHasExpansion(parts); }

bool Word::isFullyLiteral() const { return partIsFullyLiteral(parts); }

std::string Word::value() const { return partsLiteralValue(parts); }

std::string Word::render() const { return partsRender(parts); }

// ---------------------------------------------------------------------------
// 公开接口
// ---------------------------------------------------------------------------

const char* toolModeName(ToolMode mode) {
    switch (mode) {
    case ToolMode::Off:
        return "off";
    case ToolMode::Confirm:
        return "confirm";
    case ToolMode::Automatic:
        return "automatic";
    }
    return "off";
}

bool toolModeFromName(const std::string& name, ToolMode& out) {
    const std::string normalized = lowerAscii(trimAscii(name));
    if (normalized == "off") {
        out = ToolMode::Off;
        return true;
    }
    if (normalized == "confirm") {
        out = ToolMode::Confirm;
        return true;
    }
    if (normalized == "automatic") {
        out = ToolMode::Automatic;
        return true;
    }
    return false;
}

const char* featureName(Feature feature) {
    switch (feature) {
    case Feature::CommandSubstitution:
        return "command_substitution";
    case Feature::ParameterExpansion:
        return "parameter_expansion";
    case Feature::ArithmeticExpansion:
        return "arithmetic_expansion";
    case Feature::Heredoc:
        return "heredoc";
    case Feature::HereString:
        return "here_string";
    case Feature::Background:
        return "background";
    case Feature::Redirection:
        return "redirection";
    case Feature::Assignment:
        return "assignment";
    case Feature::Glob:
        return "glob";
    case Feature::Tilde:
        return "tilde";
    case Feature::Pipeline:
        return "pipeline";
    case Feature::Sequence:
        return "sequence";
    case Feature::Comment:
        return "comment";
    }
    return "unknown";
}

const char* decisionCodeName(DecisionCode code) {
    switch (code) {
    case DecisionCode::AllowedAutomatic:
        return "allowed_automatic";
    case DecisionCode::RequiresConfirm:
        return "requires_confirm";
    case DecisionCode::DeniedOff:
        return "denied_off";
    case DecisionCode::DeniedBlocklist:
        return "denied_blocklist";
    case DecisionCode::DeniedEmpty:
        return "denied_empty";
    case DecisionCode::DeniedTooLarge:
        return "denied_too_large";
    case DecisionCode::DeniedParseError:
        return "denied_parse_error";
    case DecisionCode::DeniedUnsupportedSyntax:
        return "denied_unsupported_syntax";
    case DecisionCode::DeniedExpansion:
        return "denied_expansion";
    case DecisionCode::DeniedGlob:
        return "denied_glob";
    case DecisionCode::DeniedBackground:
        return "denied_background";
    case DecisionCode::DeniedAssignment:
        return "denied_assignment";
    case DecisionCode::DeniedRedirection:
        return "denied_redirection";
    case DecisionCode::DeniedCommandNotAllowed:
        return "denied_command_not_allowed";
    case DecisionCode::DeniedArguments:
        return "denied_arguments";
    case DecisionCode::DeniedUrl:
        return "denied_url";
    case DecisionCode::DeniedTooComplex:
        return "denied_too_complex";
    }
    return "denied_unknown";
}

std::vector<Feature> featuresOf(const CommandList& list) {
    std::vector<Feature> features;
    if (list.items.size() > 1) pushFeature(features, Feature::Sequence);
    for (const ListItem& item : list.items) {
        if (item.background) pushFeature(features, Feature::Background);
        for (const Pipeline& pipeline : item.andOr.pipelines) {
            if (pipeline.commands.size() > 1) pushFeature(features, Feature::Pipeline);
            for (const SimpleCommand& command : pipeline.commands) {
                if (!command.assignments.empty()) pushFeature(features, Feature::Assignment);
                if (!command.redirections.empty()) pushFeature(features, Feature::Redirection);
                for (const Word& word : command.argv) collectWordFeatures(word, features);
                for (const Redirection& redirection : command.redirections) {
                    collectWordFeatures(redirection.target, features);
                }
            }
        }
    }
    return features;
}

ParseResult parseCommand(const std::string& command) {
    ParseResult result;
    if (command.size() > kMaxCommandBytes) {
        result.error       = "命令过长（超过 8 KiB）";
        result.errorOffset = kMaxCommandBytes;
        return result;
    }
    if (command.find('\0') != std::string::npos) {
        result.error = "命令包含 NUL 字符";
        return result;
    }
    Parser parser(command, result);
    if (!parser.parse()) {
        if (result.error.empty()) result.error = "解析失败";
        result.ok = false;
        return result;
    }
    result.ok                               = true;
    const std::vector<Feature> listFeatures = featuresOf(result.list);
    for (Feature feature : listFeatures) pushFeature(result.features, feature);
    return result;
}

Decision evaluateCommand(const std::string& command, const PolicyConfig& config) {
    if (command.size() > kMaxCommandBytes) {
        return makeDeny(DecisionCode::DeniedTooLarge, "命令过长（超过 8 KiB），已拒绝");
    }
    const std::string trimmed = trimAscii(command);
    if (config.mode == ToolMode::Off) {
        return makeDeny(DecisionCode::DeniedOff, "Shell 工具当前为关闭模式，不会执行任何命令");
    }
    if (trimmed.empty()) return makeDeny(DecisionCode::DeniedEmpty, "命令为空，已拒绝");
    if (trimmed.find('\0') != std::string::npos) {
        return makeDeny(DecisionCode::DeniedParseError, "命令包含 NUL 字符，已拒绝");
    }
    for (const std::string& pattern : config.blocklist) {
        if (pattern.empty()) continue;
        if (containsIgnoreCaseAscii(trimmed, pattern)) {
            return makeDeny(DecisionCode::DeniedBlocklist, "命令命中安全策略黑名单，已拒绝");
        }
    }

    const ParseResult parsed = parseCommand(trimmed);
    if (parsed.ok && parsed.list.items.empty()) {
        return makeDeny(DecisionCode::DeniedEmpty, "命令为空，已拒绝");
    }
    const std::string summary = parsed.ok ? summarizeCommandList(parsed.list) : std::string();

    if (config.mode == ToolMode::Confirm) {
        Decision decision;
        decision.allowed      = false;
        decision.needsConfirm = true;
        decision.code         = DecisionCode::RequiresConfirm;
        decision.reason       = "需要用户确认后执行";
        decision.summary      = summary;
        if (!parsed.ok) {
            decision.warnings.push_back("命令无法被安全解析：" + parsed.error + "；请仔细核对后再决定");
        } else {
            decision.warnings = confirmWarnings(parsed);
        }
        return decision;
    }

    if (!parsed.ok) {
        return makeDeny(
            DecisionCode::DeniedParseError,
            "命令无法被安全解析（" + parsed.error + "），automatic 模式已拒绝"
        );
    }
    Decision decision = evaluateAutomatic(parsed);
    decision.summary  = summary;
    return decision;
}

AuditReport auditCommand(const std::string& command) {
    AuditReport report;
    if (command.size() > kMaxCommandBytes) {
        report.error = "命令过长（超过 8 KiB）";
        report.warnings.push_back("命令过长，无法安全解析");
        return report;
    }
    const std::string trimmed = trimAscii(command);
    const ParseResult parsed  = parseCommand(trimmed);
    if (!parsed.ok) {
        report.error = parsed.error;
        report.warnings.push_back("命令无法被安全解析：" + parsed.error + "；若仍需执行请特别小心");
        return report;
    }
    if (parsed.list.items.empty()) {
        report.error = "命令为空";
        report.warnings.push_back("命令为空，没有可审计的内容");
        return report;
    }
    report.ok      = true;
    report.summary = summarizeCommandList(parsed.list);
    for (Feature feature : parsed.features) report.features.push_back(featureName(feature));
    report.warnings = confirmWarnings(parsed);
    for (const ListItem& item : parsed.list.items) {
        for (const Pipeline& pipeline : item.andOr.pipelines) {
            for (const SimpleCommand& commandEntry : pipeline.commands) {
                if (commandEntry.argv.empty()) continue;
                const Word& first = commandEntry.argv.front();
                report.commands.push_back(first.isFullyLiteral() ? first.value() : first.render());
            }
        }
    }
    return report;
}

std::vector<std::string> automaticAllowedCommands() {
    return {"cat", "curl", "cut", "grep", "head", "sort", "tail", "tr", "uniq", "wc"};
}

const char* policyVersion() { return "1"; }

} // namespace mod::shell
