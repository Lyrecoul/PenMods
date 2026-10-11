const express = require('express');
const fs = require('fs');
const os = require('os');
const path = require('path');

let Resvg;
try {
    Resvg = require('@resvg/resvg-js').Resvg;
} catch (err) {
    console.error('致命错误：无法加载 @resvg/resvg-js native 模块。');
    console.error('请确保已安装正确的平台原生包：');
    console.error('  npm install @resvg/resvg-js-linux-arm64-gnu  (词典笔)');
    console.error('  npm install @resvg/resvg-js-linux-x64-gnu   (x86_64 Linux)');
    console.error('  npm install @resvg/resvg-js-darwin-arm64     (macOS ARM)');
    console.error('');
    console.error(`原始错误：${err.message}`);
    process.exit(1);
}

const { mathjax } = require('mathjax-full/js/mathjax.js');
const { TeX } = require('mathjax-full/js/input/tex.js');
const { SVG } = require('mathjax-full/js/output/svg.js');
const { liteAdaptor } = require('mathjax-full/js/adaptors/liteAdaptor.js');
const { RegisterHTMLHandler } = require('mathjax-full/js/handlers/html.js');

const DEBUG = process.env.DEBUG === '1' || process.env.DEBUG === 'true';
const log = DEBUG ? console.log.bind(console) : () => { };

// --- 屏幕与渲染限制 ---
const SCREEN_WIDTH = 320;
const SCREEN_HEIGHT = 170;
const MIN_PNG_DIM = 16;
const MAX_LATEX_LENGTH = 4000;
const REF_EM = 16;        // 用于自然尺寸计算的概念字号
const SCRIPT_SCALE = '0.55';     // 上标/下标尺寸缩放倍数（默认 0.707）
const MAX_RENDER_WIDTH = 300;    // 单张公式图的最大宽度
const MAX_WRAP_LINES = 8;        // 单个公式最多拆成几行
const LINE_GAP = 4;              // 换行后行与行的间距（客户端用同一个值）
const RENDERER_VERSION = 3;      // 3 = 默认改用设备上的系统字体渲染中文（不再随包分发字体）
                                 // 2 = 统一基准字号 + 长公式换行（客户端据此提示旧服务）
const RATE_LIMIT_PER_SECOND = 100; // 一条长回答可能带几十个公式，限流只用来兜住失控客户端
const SVG_EX_RE = /(?:width|height)="([0-9.]+)ex"/g;

// --- 公式里的中文字体 ---
// 背景：MathJax 只为自己字体里没有的字（中文等）输出 <text font-family="serif">，数学符号/字母
// 一律是 <path>。resvg 找不到字形就把这些 <text> 整段丢掉——真机上
// \boxed{D.\ Y\text{ 的转化率为 }60\%} 会只剩 "D. Y" 和 "60%"，中文全没了。
//
// 设备**自带**中文字体：/usr/share/fonts 是指向 /usr/lib/fonts 的符号链接，里面有
// NotoSansSC-*.otf 与 HYQiHei-*.ttf（fc-list 也能列出 "Noto Sans SC"、"汉仪旗黑"）。
// resvg-js 2.6.2 打开了 fontdb 的 fontconfig feature：它自己解析 /etc/fonts/fonts.conf、
// 跟随符号链接、递归扫描，不需要 fontconfig cache，因此默认就直接用系统字体
// （loadSystemFonts: true），服务端不再随包分发任何字体文件。
// 实测：一个完全不做字体处理的旧版服务端在真机上渲染 \text{中文测试} 正常（118×28，
// 1018 个墨迹点），说明这条路本来就走得通。
//
// 系统字体是默认值；只有明确给了字体文件（MATHJAX_FONT，或设备目录里放好的同名文件）时才
// 改用它，留给那些确实没有中文字体的设备（换了固件/精简系统）：这时才把 SVG 里 <text> 的
// font-family 指到该字体的真实 family 上。这条链路上可能失败的环节：
//   1. 显式字体只认真实文件路径：MATHJAX_FONT、/userdata/PenMods、/userdisk/PenMods，以及可写
//      目录里已经落盘好的副本；pkg 快照里的路径不再是候选（已经不打包装字体了）；
//   2. 找到的字体先原子复制进一个可写目录（MATHJAX_FONT_DIR → os.tmpdir() → /userdata/PenMods
//      → /userdisk/PenMods，成功为止；os.tmpdir() 在设备上可能是 /tmp，空间小甚至只读）；
//      已存在且大小一致就直接复用，最终路径进日志与 /health；
//   3. 落盘必须原子（先写 .tmp 校验大小再 rename），否则重启时可能读到半截文件；
//   4. 字体判定只做一次：成功/失败都定下来，不会每个请求重试；
//   5. 字体不可用只意味着"没有中文"，绝不因此让渲染失败，其它公式照常出图；
//   6. family 名不靠硬编码猜：直接解析字体 name 表拿真实 family，再用 resvg 真渲染一个"汉"字
//      看有没有墨迹来确认（probeCjkInk），确认通过的名字才写进 SVG 与 /health。
const SYSTEM_FONT_FAMILY = 'Noto Sans SC';   // 设备自带的中文字体族，作为 resvg 的 defaultFontFamily
const CJK_FONT_FILE = 'wqy-microhei.ttc';    // 用户自己放字体文件时沿用的文件名（不再随包分发）
const CJK_FONT_FAMILY_FALLBACK = 'WenQuanYi Micro Hei';   // name 表读不出来时的兜底名，仍要过探针

const server = { instance: null, port: 0 };

// 中文字体状态机：pending → file / system / failed。任何一步定下来就不会再重来。
//   file   —— 用了显式指定的字体文件，path/family 可用，SVG 里的 <text> 会改写成 family
//   system —— 用了设备自带的系统字体（默认路径），SVG 保持 MathJax 原样，交给 fontconfig 兜住
//   failed —— 两条路都不通，中文会是空白（reason 说明原因）
const fontState = {
    status: 'pending',   // pending | file | system | failed
    path: null,          // 显式字体落盘后的真实文件路径（给 resvg 的 fontFiles）
    family: null,        // 实测能画出汉字的 family 名（写进 SVG 的 <text> 与 resvg 缺省族）
    dir: null,           // 最终选中的可写目录（仅 file 模式）
    source: null,        // 显式字体数据从哪读到的（仅 file 模式）
    cached: false,       // 是否复用了上一次落盘出来的副本
    fileReason: null,    // 显式字体没用上的原因（仅日志；完全没给字体文件时为 null）
    reason: null,        // 中文整体不可用的原因（cjkFont=false 时进 /health）
    verdict: null        // 判定依据（探针渲染结果，便于排查）
};

// 可写目录候选：环境变量 → os.tmpdir() → 设备上两个一定存在的 PenMods 目录
function fontDirCandidates() {
    const raw = [];
    if (process.env.MATHJAX_FONT_DIR) raw.push(process.env.MATHJAX_FONT_DIR);
    try { raw.push(os.tmpdir()); } catch (err) { /* os.tmpdir() 在某些环境下会抛 */ }
    raw.push('/userdata/PenMods', '/userdisk/PenMods');
    const dirs = [];
    for (const dir of raw) {
        if (typeof dir === 'string' && dir !== '' && dirs.indexOf(dir) < 0) dirs.push(dir);
    }
    return dirs;
}

// 显式字体文件的候选来源（按可靠性排序）：MATHJAX_FONT 指定的路径优先，其次是设备上放好的同名
// 文件，最后是可写目录里上一次落盘出来的副本（临时目录还在时能省一次读）。
// 不再包含 __dirname / process.cwd() 下的打包字体：服务端已经不随包分发字体了。
function fontSourceCandidates() {
    const list = [
        process.env.MATHJAX_FONT,
        '/userdata/PenMods/' + CJK_FONT_FILE,
        '/userdisk/PenMods/' + CJK_FONT_FILE
    ];
    for (const dir of fontDirCandidates()) list.push(path.join(dir, CJK_FONT_FILE));
    return list.filter((item) => typeof item === 'string' && item !== '');
}

// 从 TTF/TTC 的 name 表里读出真实的 family 名（nameID 1 = family，nameID 16 = typographic family）。
// 只读字体集合里的第一份字体；解析失败返回 []，不影响后面用兜底名字。
function readFontFamilyNames(buf) {
    try {
        const dv = new DataView(buf.buffer, buf.byteOffset, buf.byteLength);
        const offsets = [];
        if (buf.readUInt32BE(0) === 0x74746366) {           // 'ttcf'
            const numFonts = dv.getUint32(8);
            if (numFonts === 0 || numFonts > 64) return [];
            for (let i = 0; i < numFonts; i++) offsets.push(dv.getUint32(12 + i * 4));
        } else {
            offsets.push(0);
        }
        const utf16be = (start, len) => {
            let text = '';
            for (let i = 0; i + 1 < len; i += 2) text += String.fromCharCode((buf[start + i] << 8) | buf[start + i + 1]);
            return text;
        };
        const offset = offsets[0];
        const numTables = dv.getUint16(offset + 4);
        for (let t = 0; t < numTables; t++) {
            const record = offset + 12 + t * 16;
            if (buf.toString('latin1', record, record + 4) !== 'name') continue;
            const tableOffset = dv.getUint32(record + 8);
            const count = dv.getUint16(tableOffset + 2);
            const stringsOffset = dv.getUint16(tableOffset + 4);
            const names = [];
            for (let n = 0; n < count; n++) {
                const item = tableOffset + 6 + n * 12;
                const platform = dv.getUint16(item);
                const nameId = dv.getUint16(item + 6);
                if (nameId !== 1 && nameId !== 16) continue;   // family / typographic family
                const len = dv.getUint16(item + 8);
                const start = tableOffset + stringsOffset + dv.getUint16(item + 10);
                if (len === 0 || start + len > buf.length) continue;
                const name = (platform === 3 || platform === 0)
                    ? utf16be(start, len)
                    : buf.toString('latin1', start, start + len);
                if (name !== '' && names.indexOf(name) < 0) names.push(name);
            }
            return names;
        }
        return [];
    } catch (err) {
        return [];
    }
}

// XML/SVG 属性里不能出现裸引号
function xmlAttr(value) {
    return String(value).replace(/&/g, '&amp;').replace(/"/g, '&quot;').replace(/</g, '&lt;');
}

// 真渲染一个"汉"字数墨迹点。只判断"文件存在/加载没报错"是不够的：
// 真正的故障是字体在、但 resvg 拿不到字形，输出一片空白（系统字体与指定字体都会这样）。
// fontOptions 是 resvg 的 font 配置：指定字体时喂 fontFiles，系统字体时为 fontFiles: []。
// 已知盲区：只数墨迹点认不出字体里的 .notdef 方框——字体完全没有汉字、但带 .notdef 时，
// "汉"会画成方框、同样有墨迹，探针会通过（实测 MATHJAX_FONT 指到 DejaVu Sans 就是 4 个方框）。
function probeCjkInk(family, fontOptions) {
    const svg = '<svg xmlns="http://www.w3.org/2000/svg" width="48" height="48">'
        + '<rect width="48" height="48" fill="#000000"/>'
        + '<g fill="#ffffff"><text x="4" y="34" font-size="28" font-family="' + xmlAttr(family) + '">汉</text></g>'
        + '</svg>';
    const image = new Resvg(svg, {
        font: Object.assign({}, fontOptions, { defaultFontFamily: family })
    }).render();
    const pixels = image.pixels;
    let ink = 0;
    for (let i = 0; i < pixels.length; i += 4) {
        if (pixels[i] > 128) ink++;
    }
    return ink;
}

// 原子落盘：已存在且大小一致就直接复用，否则写 .tmp → 校验大小 → rename。
// 任何一步失败都清掉 .tmp：设备上 /tmp 可能很小，失败还留下半个大文件会雪上加霜。
function dropFontFile(dir, data) {
    const target = path.join(dir, CJK_FONT_FILE);
    try {
        if (fs.statSync(target).size === data.length) return { path: target, cached: true };
    } catch (err) { /* 不存在或不完整，往下走重写 */ }
    try { fs.mkdirSync(dir, { recursive: true }); } catch (err) { /* 目录已存在最常见，真不可写由下面写文件报错 */ }
    const tmp = `${target}.${process.pid}.tmp`;
    try {
        fs.writeFileSync(tmp, data);
        if (fs.statSync(tmp).size !== data.length) throw new Error('写入不完整');
        fs.renameSync(tmp, target);
    } catch (err) {
        try { fs.unlinkSync(tmp); } catch (cleanupErr) { /* 可能压根没建成 */ }
        throw err;
    }
    return { path: target, cached: false };
}

/**
 * 找一份显式指定的中文字体文件并确认它真的能画出中文：读候选来源 → 原子落盘到可写目录 →
 * 解析 name 表拿真实 family → 用 resvg 真渲染一个"汉"字验证。
 * 没有文件、或验证不过都返回 false（原因记在 fontState.fileReason），由调用方退回系统字体。
 * 成功时 status='file'，path/family 可用。
 */
function useExplicitFont() {
    // 一、读字体数据（MATHJAX_FONT / 设备目录里放好的同名文件 / 上次落盘的副本）
    let data = null;
    const readErrors = [];
    for (const candidate of fontSourceCandidates()) {
        try {
            const buf = fs.readFileSync(candidate);
            if (buf.length < 1024) { readErrors.push(`${candidate}(内容过小)`); continue; }
            data = buf;
            fontState.source = candidate;
            break;
        } catch (err) {
            readErrors.push(`${candidate}(${err.code || err.message})`);
        }
    }
    if (!data) {
        log(`没有显式中文字体文件，使用系统字体（已试 ${readErrors.slice(0, 3).join('、')}${readErrors.length > 3 ? ' 等' : ''}）`);
        return false;
    }

    // 二、落盘到第一个写得进去的目录（只做一次）
    const writeErrors = [];
    let extracted = null;
    for (const dir of fontDirCandidates()) {
        try {
            extracted = dropFontFile(dir, data);
            fontState.dir = dir;
            break;
        } catch (err) {
            writeErrors.push(`${dir}(${err.code || err.message})`);
        }
    }
    if (!extracted) {
        fontState.fileReason = `字体落盘失败，目录都不可写：${writeErrors.join('、')}`;
        return false;
    }
    fontState.path = extracted.path;
    fontState.cached = extracted.cached;

    // 三、确定 family：先用 name 表里读到的真名，再兜底硬编码名，逐个真渲染验证
    const candidates = readFontFamilyNames(data);
    if (candidates.indexOf(CJK_FONT_FAMILY_FALLBACK) < 0) candidates.push(CJK_FONT_FAMILY_FALLBACK);
    const probeErrors = [];
    for (const family of candidates) {
        try {
            const ink = probeCjkInk(family, { fontFiles: [extracted.path], loadSystemFonts: false });
            if (ink > 0) {
                fontState.family = family;
                fontState.status = 'file';
                fontState.verdict = `指定字体探针渲染 "汉" 得到 ${ink} 个墨迹点`;
                return true;
            }
            probeErrors.push(`${family}(无墨迹)`);
        } catch (err) {
            probeErrors.push(`${family}(${err.message})`);
        }
    }
    fontState.fileReason = `字体已落盘到 ${extracted.path}，但 resvg 没能用它渲染出中文：${probeErrors.join('、')}`;
    return false;
}

/**
 * 决定用哪种字体：优先显式指定的字体文件，没有或不可用就用设备上的系统字体（默认路径）。
 * 整条流程只跑一次（含失败），返回 fontState：
 *   status==='file'   指定字体可用，path/family 是实测结果；
 *   status==='system' 系统字体可用，family 是缺省族名；
 *   status==='failed' 两条路都不通，中文会空白，reason 说明原因。
 */
function ensureCjkFont() {
    if (fontState.status !== 'pending') return fontState;
    fontState.status = 'failed';   // 先钉成 failed：任何异常路径都不会变成每次请求重试

    try {
        // 一、显式指定的字体文件优先（留给确实没有中文字体的设备）
        if (!useExplicitFont()) {
            // 二、默认路径：设备自带的系统字体（/usr/lib/fonts 里的 Noto Sans SC / HYQiHei）。
            //     同一个探针：连系统字体都画不出汉字的机器要如实报出来，客户端才会提示换字体。
            if (fontState.fileReason)
                console.warn(`警告：指定的中文字体不可用（${fontState.fileReason}），改用系统字体。`);
            const ink = probeCjkInk(SYSTEM_FONT_FAMILY, systemFontOptions());
            if (ink > 0) {
                fontState.status = 'system';
                fontState.family = SYSTEM_FONT_FAMILY;
                fontState.verdict = `系统字体探针渲染 "汉" 得到 ${ink} 个墨迹点`;
            } else {
                fontState.reason = '系统字体渲染不出中文（探针 0 个墨迹点）'
                    + (process.env.MATHJAX_NO_SYS_FONTS === '1' ? '，且 MATHJAX_NO_SYS_FONTS=1 关掉了系统字体' : '')
                    + (fontState.fileReason ? `；显式字体也不可用：${fontState.fileReason}` : '');
            }
        }
    } catch (err) {
        fontState.reason = `确定中文字体时异常：${err.message}`;
    }
    finishFont();
    return fontState;
}

function finishFont() {
    if (fontState.status === 'file') {
        console.log(`中文字体：使用指定字体文件。family="${fontState.family}" 路径=${fontState.path}`
            + `${fontState.cached ? '（复用上次落盘出的副本）' : '（本次写入）'}`
            + `，来源 ${fontState.source}，${fontState.verdict}`);
    } else if (fontState.status === 'system') {
        console.log(`中文字体：使用系统字体（缺省族 "${fontState.family}"，设备自带 Noto Sans SC / HYQiHei），${fontState.verdict}`);
    } else {
        console.warn(`警告：中文字体不可用，公式里的中文会显示为空白。原因：${fontState.reason}`);
        console.warn(`  可用 MATHJAX_FONT=<字体文件路径> 指定字体，或把字体文件放到 /userdata/PenMods、`
            + `/userdisk/PenMods（文件名 ${CJK_FONT_FILE}）后重试。`);
    }
}

/**
 * 只把 <text> 元素的 font-family 换成中文字体。
 * MathJax 的数学符号全是 <path>，<text> 只用于"它自己字体里没有的字"，也就是中文这类回退字形，
 * 所以改这里既能让中文出来，又不会动到算式本身的排版（各元素的 translate 由 MathJax 固定给出）。
 */
function applyCjkFamily(svg, family) {
    const attr = xmlAttr(family);
    return svg.replace(/<text\b[^>]*>/g, (tag) => {
        if (/font-family\s*=\s*"[^"]*"/.test(tag)) return tag.replace(/font-family\s*=\s*"[^"]*"/, `font-family="${attr}"`);
        return tag.replace(/^<text\b/, `<text font-family="${attr}"`);
    });
}

// 默认（系统字体）的 resvg 字体选项。设备自带 Noto Sans SC / HYQiHei，resvg 经 fontdb 的
// fontconfig 支持自己找到它们；MATHJAX_NO_SYS_FONTS=1 是关掉系统字体的逃生开关
// （此时只有显式指定的字体文件能用，中文多半会空白）。
function systemFontOptions() {
    return {
        fontFiles: [],
        loadSystemFonts: process.env.MATHJAX_NO_SYS_FONTS !== '1',
        defaultFontFamily: SYSTEM_FONT_FAMILY
    };
}

// resvg 的字体选项。
// 注意：resvg-js 2.6.2 会把 serifFamily / sansSerifFamily / cursiveFamily / fantasyFamily /
// monospaceFamily 解析进结构体，但渲染时从不读它们，只有 fontFiles / loadSystemFonts /
// defaultFontFamily 起作用，所以这里不再传那五个字段。
function resvgFontOptions() {
    // 显式指定了字体文件：只喂这一份，设备上有哪些系统字体不再影响结果
    if (fontState.status === 'file') {
        return {
            fontFiles: [fontState.path],
            loadSystemFonts: false,
            defaultFontFamily: fontState.family
        };
    }
    // 默认路径：系统字体
    return systemFontOptions();
}

// /health 与 /render 里如实报告字体状态
function cjkReport() {
    const available = fontState.status === 'file' || fontState.status === 'system';
    return {
        cjkFont: available,
        // 只有显式字体文件才有路径；系统字体模式没有单一路径
        cjkFontPath: fontState.status === 'file' ? fontState.path : null,
        cjkFontFamily: fontState.family,
        cjkFontSource: fontState.status === 'file' ? 'file'
            : (fontState.status === 'system' ? 'system' : 'none'),
        cjkFontError: available ? null : fontState.reason
    };
}

// --- 简单的异步互斥锁 ---
class Mutex {
    constructor() { this._queue = []; this._locked = false; }
    async acquire() {
        if (!this._locked) { this._locked = true; return; }
        return new Promise(resolve => this._queue.push(resolve));
    }
    release() {
        if (this._queue.length > 0) { this._queue.shift()(); }
        else { this._locked = false; }
    }
}

// --- 简单的每 IP 限速器 ---
class RateLimiter {
    constructor(windowMs = 1000, maxRequests = 10) {
        this.windowMs = windowMs;
        this.maxRequests = maxRequests;
        this.hits = new Map();
        setInterval(() => this._cleanup(), windowMs * 2).unref();
    }
    _cleanup() {
        const now = Date.now();
        for (const [ip, timestamps] of this.hits) {
            const fresh = timestamps.filter(t => now - t < this.windowMs);
            if (fresh.length === 0) this.hits.delete(ip);
            else this.hits.set(ip, fresh);
        }
    }
    allow(ip) {
        const now = Date.now();
        const timestamps = (this.hits.get(ip) || []).filter(t => now - t < this.windowMs);
        if (timestamps.length >= this.maxRequests) return false;
        timestamps.push(now);
        this.hits.set(ip, timestamps);
        return true;
    }
}

const renderMutex = new Mutex();
const rateLimiter = new RateLimiter(1000, RATE_LIMIT_PER_SECOND);

/**
 * 解析 SVG 中以 ex 为单位的原始宽高。
 * MathJax 输出：<svg width="1.294ex" height="1.025ex" viewBox="...">
 */
function getSvgExSize(svgStr) {
    SVG_EX_RE.lastIndex = 0;
    const wm = SVG_EX_RE.exec(svgStr);
    const hm = SVG_EX_RE.exec(svgStr);
    if (!wm || !hm) return null;
    return { w_ex: parseFloat(wm[1]), h_ex: parseFloat(hm[1]) };
}

/**
 * 根据 SVG 原始尺寸计算最优的 Resvg fitTo 尺寸。
 *
 * 策略：一律按 REF_EM 下的自然尺寸出图，只做缩小，不做放大：
 *   MAX_WIDTH  — 宽度不超出气泡可用宽
 *   MAX_HEIGHT — 高度不超出屏幕
 *
 * 一律按 REF_EM 基准字号出图，只缩不放：把过小的公式"放大到可读尺寸"会让同一段回答里
 * 单个字符的公式（自然尺寸很小）与长公式（已被宽度上限压过）字号相差数倍。
 */
function calcFitTo(svgStr, screenW, screenH) {
    const size = getSvgExSize(svgStr);
    const maxW = Math.min(Math.round(screenW) - 20, MAX_RENDER_WIDTH);
    if (!size) return { mode: 'width', value: Math.max(maxW, MIN_PNG_DIM) };

    // 下面的高度上限目前在真实请求里不会生效：rasterize 只在拿不到尺寸（size 为空或宽 0）时
    // 才调用 calcFitTo，那种 SVG 的 ex 尺寸同样为空（或高宽都是 0），所以上面那句就返回了；
    // 真正的公式一律走 rasterize 的"按宽度"分支。实测 \begin{pmatrix}a\\…\\h\end{pmatrix}
    // 会原样返回 42x195（> 一屏 170、更 > 60% 的 102），这是有意的：只缩宽度能保住统一基准字号，
    // 长式子竖排后本来就允许超过一屏、由客户端滚动。留着这段是为了万一将来有别的调用方。
    const MAX_H = Math.round(screenH * 0.6);

    // 概念字号 REF_EM 下的自然像素尺寸
    // MathJax 默认 ex = em/2，因此 1ex = REF_EM/2 px
    const natW = size.w_ex * REF_EM / 2;
    const natH = size.h_ex * REF_EM / 2;

    const scale = Math.min(1, maxW / natW, MAX_H / natH);

    const finalW = Math.max(1, Math.round(natW * scale));
    return { mode: 'width', value: finalW };
}

// -----------------------------------------------------------------------
// 长公式换行
//
// 一条式子放不下时不再整体压小（压小后字号只有别的公式的几分之一），而是在顶层
// 运算符处断成多行，每行都按同一个基准字号出图，由客户端竖排显示。断点只取顶层
// 的运算符：括号内、\left…\right 内、\begin…\end 环境内都不拆。
//
// 断点类别：
//   关系符（=、\le、\implies…）→ 箭头（\xrightarrow、\to、\Rightarrow…，断在箭头之前）
//   → 加减 → 乘除 → \text{} 内的中文标点/空白 → 顶层中文标点 → \quad/\qquad 与 "\ "
// 优先级数值见下面两张表；行数超过上限时按优先级从弱到强合并相邻行。
// -----------------------------------------------------------------------

// 断点在"宏之前"的命令。优先级越小越优先，行数超过上限时按优先级从弱到强合并相邻行，
// 所以关系符最不容易被合并掉，间距类最先被合并。
const BREAK_BEFORE_COMMANDS = {
    // 0 关系符
    '\\approx': 0, '\\equiv': 0, '\\neq': 0, '\\ne': 0, '\\sim': 0, '\\simeq': 0, '\\cong': 0,
    '\\le': 0, '\\leq': 0, '\\ge': 0, '\\geq': 0, '\\ll': 0, '\\gg': 0, '\\propto': 0,
    '\\implies': 0, '\\iff': 0, '\\therefore': 0, '\\because': 0, '\\in': 0, '\\notin': 0,
    '\\subset': 0, '\\subseteq': 0, '\\supset': 0, '\\supseteq': 0,
    // 1 箭头类：断在箭头之前，箭头落在下一行行首。反应式、推导链靠箭头连接，通常最长，
    //   优先级排在关系符之后、加减之前。
    '\\xrightarrow': 1, '\\xleftarrow': 1, '\\xleftrightarrow': 1, '\\xRightarrow': 1, '\\xLeftarrow': 1,
    '\\longrightarrow': 1, '\\longleftarrow': 1, '\\longleftrightarrow': 1,
    '\\Longrightarrow': 1, '\\Longleftarrow': 1, '\\Longleftrightarrow': 1,
    '\\rightarrow': 1, '\\leftarrow': 1, '\\leftrightarrow': 1,
    '\\Rightarrow': 1, '\\Leftarrow': 1, '\\Leftrightarrow': 1,
    '\\to': 1, '\\gets': 1, '\\mapsto': 1, '\\longmapsto': 1,
    '\\hookrightarrow': 1, '\\hookleftarrow': 1, '\\rightleftharpoons': 1, '\\leftharpoonup': 1,
    // 2 加减
    '\\pm': 2, '\\mp': 2,
    // 3 乘除
    '\\times': 3, '\\cdot': 3, '\\div': 3, '\\cup': 3, '\\cap': 3, '\\ast': 3
};

// "宏之后"可以断的间距类命令（断在它们之后，免得下一行以空格开头）
const BREAK_AFTER_COMMANDS = { '\\quad': 6, '\\qquad': 6 };

// 这些宏的参数是"文本"，里面的中文标点/空格也可以断行（把宏拆成两段重新闭合）。
// 嵌套时（\text{…\textbf{…}}）断开位置两侧会按顺序补全所有打开的文本宏，见 splitAtPoints。
const TEXT_MACROS = [
    '\\text', '\\textrm', '\\textnormal', '\\textbf', '\\textit', '\\textsf', '\\texttt',
    '\\textmd', '\\textup', '\\emph', '\\mathrm', '\\mathbf', '\\mathit', '\\mathsf', '\\mathtt',
    '\\mbox', '\\operatorname'
];
// \text{} 内部：中文标点、ASCII 标点、空白之后都能断
const CJK_BREAK_AFTER = /[，。；、：！？,;:\s]/;
// 公式顶层（宏外）直接写中文标点时也能断
const TOP_CJK_BREAK_AFTER = /[，。；、：！？]/;

function readCommand(latex, start) {
    let i = start + 1;
    let name = '';
    if (i < latex.length && /[a-zA-Z]/.test(latex[i])) {
        while (i < latex.length && /[a-zA-Z]/.test(latex[i])) name += latex[i++];
    } else {
        name = latex[i] || '';
        i++;
    }
    return { name, end: i };
}

/**
 * 扫描可换行位置。返回 [{index, reopen, priority}]：
 *   index    —— 在该下标处切分
 *   reopen   —— 非空时表示这里把 \text{…} 切开了，后一段要补回 "\text{"
 *   priority —— 越小越优先
 *
 * 断点分三类：
 *   A. 顶层运算符/箭头/间距命令：只在整个式子的最外层（depth===0 && 不在 \left…\right 里、
 *      不在 \begin…\end 里）才产生，绝不切开括号、\left…\right、环境；
 *   B. \text{} 等文本宏内部的中文标点与空白：允许切，且切完会补回闭合（见 splitAtPoints）。
 *      但要求此刻所有打开的花括号都属于文本宏本身（plainOpen===0）——否则切点会落在
 *      "\xrightarrow{…}" 这类外层分组的中间，切出来的两段各自都不完整；
 *   C. 公式顶层直接写的中文标点（，；、 等）：在它之后断。
 */
function scanBreakPoints(latex) {
    const points = [];
    let depth = 0;          // 花括号深度
    let plainOpen = 0;      // 其中"不属于文本宏"的打开花括号数
    let leftRight = 0;
    let envDepth = 0;
    const braceIsText = [];  // 每个 { 是否属于文本宏
    const textStack = [];    // 当前打开的文本宏（最内层在栈顶）
    let pendingText = null;

    for (let i = 0; i < latex.length;) {
        const ch = latex[i];

        if (ch === '\\') {
            const { name, end } = readCommand(latex, i);

            if (name === 'begin') { envDepth++; i = end; continue; }
            if (name === 'end') { envDepth = Math.max(0, envDepth - 1); i = end; continue; }
            if (name === 'left') { leftRight++; i = end; continue; }
            if (name === 'right') { leftRight = Math.max(0, leftRight - 1); i = end; continue; }

            if (TEXT_MACROS.indexOf('\\' + name) >= 0) {
                pendingText = '\\' + name;
                i = end;
                continue;
            }

            const top = depth === 0 && leftRight === 0 && envDepth === 0;
            if (top && BREAK_BEFORE_COMMANDS['\\' + name] !== undefined) {
                // 断在命令之前：箭头落在下一行行首，读起来像"下一步"
                points.push({ index: i, reopen: '', priority: BREAK_BEFORE_COMMANDS['\\' + name] });
            } else if (top && BREAK_AFTER_COMMANDS['\\' + name] !== undefined) {
                points.push({ index: end, reopen: '', priority: BREAK_AFTER_COMMANDS['\\' + name] });
            } else if (top && name === ' ') {
                // 转义空格 "\ "：断在它之后（它本身是"空格"，断在之前会把这行末尾的空格甩到下一行行首）
                points.push({ index: end, reopen: '', priority: 6 });
            }
            i = end;
            continue;
        }

        if (ch === '{') {
            depth++;
            // 只有"紧跟文本宏的 {"算文本宏的花括号；文本宏里面的裸 {…}、^{…} 之类一律算普通分组，
            // 那样它们中间就不允许断开（断开要跨过它们，两边都得补括号，得不偿失）
            const isText = pendingText !== null;
            braceIsText.push(isText);
            if (!isText) plainOpen++;
            if (pendingText) {
                textStack.push({ macro: pendingText, depth: depth });
                pendingText = null;
            }
            i++;
            continue;
        }
        if (ch === '}') {
            if (textStack.length > 0 && textStack[textStack.length - 1].depth === depth)
                textStack.pop();
            if (braceIsText.pop() === false) plainOpen = Math.max(0, plainOpen - 1);
            depth = Math.max(0, depth - 1);
            i++;
            continue;
        }

        // B. 文本宏内部：中文/ASCII 标点或空格之后可以断行，后一段会补回宏名重新闭合。
        //    plainOpen===0 保证切点处打开的花括号全部是文本宏自己的：
        //    断开时"全部关掉、下一段按原顺序全部重开"（见 splitAtPoints），两边都是完整 LaTeX。
        if (textStack.length > 0 && plainOpen === 0 && leftRight === 0 && envDepth === 0
                && CJK_BREAK_AFTER.test(ch) && i + 1 < latex.length && latex[i + 1] !== '}') {
            // 最后一个条件：标点正好是 \text{…} 的最后一个字符时不切，否则会多出一段空的 \text{}
            // reopen 用数组：\text{…\textbf{…}} 这种嵌套时要把两层宏都重开
            points.push({
                index: i + 1,
                reopen: textStack.map((item) => item.macro),
                priority: 4
            });
        }

        // C. 顶层（宏外）的全角标点之后
        if (depth === 0 && leftRight === 0 && envDepth === 0
                && TOP_CJK_BREAK_AFTER.test(ch) && i + 1 < latex.length) {
            points.push({ index: i + 1, reopen: '', priority: 5 });
        }

        if (depth === 0 && leftRight === 0 && envDepth === 0
                && (ch === '=' || ch === '+' || ch === '-' || ch === ',' || ch === ';')) {
            const prev = latex.slice(0, i).replace(/\s+$/, '').slice(-1);
            // 一元 +/- 只在"左操作数还没出现"时成立：行首、运算符、开启类括号。
            // 闭括号 }、)、] 不属于开启类 —— 它们后面的 +/- 一定是二元运算符；把它们算进去会让
            // \frac{a}{b}+\frac{c}{d}、x_{1}+x_{2}、f(x)+g(y)、\text{A}+\text{B} 这类式子一个断点都没有。
            const unary = (ch === '+' || ch === '-')
                && (prev === '' || prev === '\\' || '=+-*/<>([{,;:'.includes(prev));
            if (!unary) {
                const priority = ch === '=' ? 0 : (ch === ',' || ch === ';' ? 5 : 2);
                points.push({ index: i, reopen: '', priority });
            }
        }
        i++;
    }

    return points;
}

// 在给定断点处切分。切点落在文本宏内部时（cut.reopen 是当时打开的文本宏名数组，
// 由外到内），前一段按个数补上右花括号、后一段按同样顺序重开这些宏，
// 保证每一段自己都是完整的 LaTeX（嵌套的 \text{…\textbf{…}} 也不会漏括号）。
function splitAtPoints(latex, points) {
    const cuts = points
        .filter((point) => point.index > 0 && point.index < latex.length)
        .sort((a, b) => a.index - b.index);
    if (cuts.length === 0) return [latex];

    const macroList = (reopen) => {
        if (Array.isArray(reopen)) return reopen;
        return reopen ? [reopen] : [];   // 兼容旧的单个宏名字符串
    };

    const parts = [];
    let start = 0;
    for (const cut of cuts) {
        let piece = latex.slice(start, cut.index);
        const macros = macroList(cut.reopen);
        for (let i = 0; i < macros.length; i++) piece += '}';
        parts.push(piece);
        start = cut.index;
    }
    parts.push(latex.slice(start));

    for (let i = 1; i < parts.length; i++) {
        const macros = macroList(cuts[i - 1].reopen);
        if (macros.length > 0) parts[i] = macros.map((macro) => macro + '{').join('') + parts[i];
    }
    return parts;
}

// 取出最外层 \boxed{…} 的三段：前缀、盒内内容、后缀
function splitBoxed(latex) {
    const at = latex.indexOf('\\boxed{');
    if (at < 0) return null;
    const innerStart = at + '\\boxed{'.length;
    let depth = 0;
    for (let i = innerStart - 1; i < latex.length; i++) {
        if (latex[i] === '{') depth++;
        else if (latex[i] === '}') {
            depth--;
            if (depth === 0)
                return { prefix: latex.slice(0, innerStart), inner: latex.slice(innerStart, i), suffix: latex.slice(i) };
        }
    }
    return null;
}



// 找出"最长的、位于顶层的文本宏实参"（\text{…} / \mathrm{…} …），返回切分所需的位置信息。
// 两个条件都是必须的：
//   1. 必须是顶层（花括号深度 0、不在 \left…\right 里）：切开 \frac{\text{…}}{2} 只会得到
//      "\frac{\text{前半" 和 "后半}}{2}" 两段各缺一半括号的废 LaTeX。顶层切开时前后缀各自完整。
//   2. 取最长而不是第一个：\text{结论} + \text{很长的中文…} 这种"短文本打头"的式子，
//      只看第一个 \text{} 会认定"没东西可切"直接放弃（实测 625px 的句子被压成 300x8、4px 高的字）。
function pickTextArg(content) {
    let best = null;
    let depth = 0;
    let leftRight = 0;
    for (let i = 0; i < content.length;) {
        const ch = content[i];
        if (ch === '\\') {
            const { name, end } = readCommand(content, i);
            if (name === 'left') { leftRight++; i = end; continue; }
            if (name === 'right') { leftRight = Math.max(0, leftRight - 1); i = end; continue; }
            if (depth === 0 && leftRight === 0 && content[end] === '{'
                    && TEXT_MACROS.indexOf('\\' + name) >= 0) {
                // 找配对的右花括号；\{ \} \\ 这类两字符转义要跳过，否则 \text{a \{b\} c} 会数错
                let level = 0;
                let close = -1;
                for (let j = end; j < content.length; j++) {
                    const c = content[j];
                    if (c === '\\') { j++; continue; }
                    if (c === '{') level++;
                    else if (c === '}') { level--; if (level === 0) { close = j; break; } }
                }
                if (close > end) {
                    const inner = content.slice(end + 1, close);
                    if (inner.length > 0 && (!best || inner.length > best.text.length)) {
                        best = { macro: '\\' + name, text: inner, start: i, innerEnd: close };
                    }
                    i = close + 1;
                    continue;
                }
            }
            i = end;
            continue;
        }
        if (ch === '{') depth++;
        else if (ch === '}') depth = Math.max(0, depth - 1);
        i++;
    }
    return best;
}

// 按字数硬切一段内容：只切"真正的文本"（最长的顶层文本宏实参），每段各自重新闭合宏。
// 找不到顶层文本宏就原样返回 —— 绝不能把数学式子切碎再包进 \text{}：
//   \alpha\beta… 会被切成 \text{\alph} + \text{a\delta…}（MathJax 报 "only supported in math mode"），
//   x_{1}+x_{2}… 会被切成 \text{x_{1}+x_{2}+x_} + \text{3}... 甚至 \text{}}（括号不配对），
//   而 abcdefgh… 这种"变量连乘"会被当成文本、渲染成直立的 abcdefgh（数学斜体没了）。
// 切完的顺序保持不变：前缀挂到第一段、后缀挂到最后一段，避免出现"一行只有一个箭头"的空行。
function hardSplitText(content, charsPerLine) {
    if (charsPerLine < 2) return [content];

    const arg = pickTextArg(content);
    if (!arg || arg.text.length <= charsPerLine) return [content];

    const before = content.slice(0, arg.start);      // 完整前缀（宏名之前）
    const after = content.slice(arg.innerEnd + 1);   // 配对的 '}' 之后的完整后缀
    const pieces = [];
    for (let i = 0; i < arg.text.length; i += charsPerLine)
        pieces.push(arg.macro + '{' + arg.text.slice(i, i + charsPerLine) + '}');
    pieces[0] = before + pieces[0];
    pieces[pieces.length - 1] = pieces[pieces.length - 1] + after;
    return pieces;
}

// 竖排成左对齐多行。array 的 & 是列分隔符，所以"裸 &"必须转义，否则一段内容会被拆成两列、
// 甚至报 "Extra alignment tab"。但转义不能无脑做：
//   1. \begin{aligned}…\end{aligned} / \begin{array}…\end{array} 里的 & 是人家自己的列分隔符，
//      转义掉就把多列粘成一列 —— 实测 \boxed{\begin{array}{cc}aaaa+bbbb+cccc & 1111\\…\end{array}=…}
//      换行后渲染成 "aaaa + bbbb + cccc&1111"，表格列没了；
//   2. 正则 /(^|[^\\])&/g 会连带吃掉 & 前一个字符，对连续 && 只处理后一个、对 "\\&" 漏转义。
// 所以按字符扫描：跳过 \\、\{ 这类两字符转义与 \命令名，并记录 \begin/\end 的环境深度，
// 只转义环境之外的 &。
function stackLines(pieces) {
    const safe = pieces.map((piece) => {
        let out = '';
        let envDepth = 0;
        for (let i = 0; i < piece.length; i++) {
            const ch = piece[i];
            if (ch === '\\') {
                const next = piece[i + 1] || '';
                if (next !== '' && /[a-zA-Z]/.test(next)) {
                    let name = '';
                    let j = i + 1;
                    while (j < piece.length && /[a-zA-Z]/.test(piece[j])) name += piece[j++];
                    if (name === 'begin') envDepth++;
                    else if (name === 'end') envDepth = Math.max(0, envDepth - 1);
                    out += piece.slice(i, j);
                    i = j - 1;
                } else {
                    out += ch + next;    // \\、\&、\{ …：连同被转义的字符一起原样保留
                    i++;
                }
                continue;
            }
            out += (ch === '&' && envDepth === 0) ? '\\&' : ch;
        }
        return out;
    });
    return '\\begin{array}{l}' + safe.join(' \\\\ ') + '\\end{array}';
}

// 按「字符数 × 自然宽度占比」估计分行容量，贪心装箱。
// cutPriorities[i] 是 parts[i] 与 parts[i+1] 之间那个断点的优先级，用来记下"这一行是在哪一级
// 的断点上换的行"，行数超上限时优先合并弱断点处（见 mergeToBudget）。
function packParts(parts, capacityChars, cutPriorities) {
    const lines = [];
    const separators = [];
    let current = '';
    for (let i = 0; i < parts.length; i++) {
        const part = parts[i];
        if (current !== '' && current.length + part.length > capacityChars) {
            lines.push(current);
            separators.push(cutPriorities && cutPriorities[i - 1] !== undefined ? cutPriorities[i - 1] : 0);
            current = part;
        } else {
            current += part;
        }
    }
    if (current !== '') lines.push(current);
    return { lines: lines, separators: separators };
}

// 行数超上限时，反复合并"最弱"（priority 数值最大）的断点处的相邻两行直到达标：
// 牺牲的是空格、逗号这类弱断点，关系符与箭头这些主干断点保留；合并出的超宽行由单行缩放兜底。
//
// 并列最弱时不能永远挑最左边那个：一行 5 段的式子每段优先级都是 2，老是合并 0/1 会得到
// "一条巨行 + 若干小行"，最后仍然整条不达标（实测 16 行合并成 1 巨型行 + 7 行，掉回整体缩小）。
// 所以并列时挑"合并后宽度最小"的那对（widths 可选，不传就退化成取最左）。
function mergeToBudget(lines, separators, maxLines, widths) {
    let resultLines = lines.slice();
    let resultSeps = separators.slice();
    let resultWidths = widths ? widths.slice() : null;
    while (resultLines.length > maxLines && resultSeps.length > 0) {
        let weakest = 0;
        for (let i = 1; i < resultSeps.length; i++) {
            if (resultSeps[i] > resultSeps[weakest]) { weakest = i; continue; }
            if (resultSeps[i] === resultSeps[weakest] && resultWidths
                    && resultWidths[i] + resultWidths[i + 1] < resultWidths[weakest] + resultWidths[weakest + 1]) {
                weakest = i;
            }
        }
        resultLines[weakest] = resultLines[weakest] + resultLines[weakest + 1];
        resultLines.splice(weakest + 1, 1);
        resultSeps.splice(weakest, 1);
        if (resultWidths) {
            resultWidths[weakest] += resultWidths[weakest + 1];
            resultWidths.splice(weakest + 1, 1);
        }
    }
    return resultLines;
}

/**
 * 把公式按可用宽度断成多行。
 * 返回 { single } 表示不拆（按原尺寸出图），否则返回每一行的 latex 与自然尺寸。
 */
function wrapLatex(latex, display, maxW, maxLines, renderSvg, svgSize) {
    const whole = renderSvg(latex, display);
    const wholeSize = svgSize(whole);
    if (!wholeSize || wholeSize.w <= maxW) return { single: whole, size: wholeSize };

    // 公共工具：把若干段竖排成一个 array 重排后的公式（可在 \boxed 内部）
    const stackCandidate = (boxInfo, lines) => {
        const body = stackLines(lines);
        return boxInfo ? boxInfo.prefix + body + boxInfo.suffix : body;
    };
    const measure = (candidate) => {
        const svg = renderSvg(candidate, display);
        const size = svgSize(svg);
        return { svg: svg, size: size, fits: !!(size && size.w <= maxW) };
    };
    // 把若干段竖排成一块（\boxed 内/外都行），整块仍超宽时只把"超宽的那几行"再切细重排，
    // 最多 rounds 轮。为什么要按整块宽度倒推每行预算：array 自己有列间距、外面套 \boxed 还有框，
    // 整块宽度 = 最宽一行 + 内边距（实测 ~9px）。只保证"每行 ≤ maxW"是不够的 —— 每行刚好
    // 299px 时整块会到 ~308px、过不了 fits 判定，于是整条式子掉回"整体缩小"
    // （实测 1554px 的 \boxed 被压成 300x5）。内边距用"整块宽 - 最宽行宽"实测得到，
    // 不硬编码；budget 以下的窄行原样保留，避免把本来合格的行也切碎（行数白白变多）。
    // 返回 null 表示怎么切都放不下，调用方自己决定兜底。
    const fitStack = (boxInfo, lines, rounds) => {
        let attempt = lines;
        for (let round = 0; round <= rounds; round++) {
            const candidate = stackCandidate(boxInfo, attempt);
            const hit = measure(candidate);
            if (hit.fits) return { latex: candidate, svg: hit.svg, size: hit.size };
            if (round === rounds || !hit.size || !(hit.size.w > 0)) break;

            const rows = attempt.map((line) => ({ line: line, size: svgSize(renderSvg(line, display)) }));
            const widest = rows.reduce((max, row) => Math.max(max, row.size ? row.size.w : 0), 0);
            const pad = Math.max(0, hit.size.w - widest);
            const budget = Math.max(MIN_PNG_DIM, maxW - pad);
            const next = [];
            for (const row of rows) {
                const text = pickTextArg(row.line);
                const pieces = (text && row.size && row.size.w > budget)
                    ? hardSplitText(row.line, Math.max(4, Math.floor(text.text.length * budget / row.size.w)))
                    : [row.line];
                if (pieces.length === 1) next.push(row.line);
                else for (const piece of pieces) next.push(piece);
            }
            if (next.length <= attempt.length) break;   // 一轮下来没有一行能再切，别空转
            attempt = next;
        }
        return null;
    };

    // ── 一、盒子里的内容：只在装不下时断，行数取最少；没有断点就按字数硬切 ──
    const box = splitBoxed(latex);
    if (box && box.inner.length > 0) {
        const innerPoints = scanBreakPoints(box.inner);
        let innerLines = [];
        if (innerPoints.length > 0) {
            const innerParts = splitAtPoints(box.inner, innerPoints);
            const innerChars = innerParts.reduce((sum, part) => sum + part.length, 0);
            const capacity = Math.max(8, Math.floor(innerChars * maxW / wholeSize.w * 0.95));
            // 注意 packParts 返回 {lines, separators}，这里要取 .lines
            innerLines = packParts(innerParts, capacity, innerPoints.map((point) => point.priority)).lines;
            // 估算不准导致某行仍超宽：按实测比例缩小每行字数，而不是对半切
            const evened = [];
            for (const line of innerLines) {
                const lineSize = svgSize(renderSvg(line, display));
                const overflow = lineSize && lineSize.w > maxW ? lineSize.w / maxW : 1;
                const linePoints = scanBreakPoints(line);
                if (overflow > 1.05 && linePoints.length > 0) {
                    const lineParts = splitAtPoints(line, linePoints);
                    const lineChars = lineParts.reduce((sum, part) => sum + part.length, 0);
                    evened.push(...packParts(lineParts, Math.max(6, Math.floor(lineChars / overflow)),
                                             linePoints.map((point) => point.priority)).lines);
                } else if (overflow > 1.05) {
                    evened.push(...hardSplitText(line, Math.max(4, Math.floor(line.length / overflow))));
                } else {
                    evened.push(line);
                }
            }
            innerLines = evened;
        }
        if (innerLines.length <= 1 && wholeSize.w > maxW) {
            // 完全没有断点（整句中文没有标点）：按基准字号能放几个字就切几个字
            innerLines = hardSplitText(box.inner, Math.max(4, Math.floor(maxW / (REF_EM * 1.15))));
        }
        if (innerLines.length > 1) {
            // 竖排进盒子；整块超宽时交给 fitStack 逐轮切细（每行字数是按字符占比估的，偏差可能不止一轮）。
            const hit = fitStack(box, innerLines, 2);
            if (hit) return { lines: [hit] };
        }
    }

    // ── 二、顶层运算符/箭头处断成多行，每行一张图 ──
    const points = scanBreakPoints(latex);
    if (points.length > 0) {
        const cuts = points
            .filter((point) => point.index > 0 && point.index < latex.length)
            .sort((a, b) => a.index - b.index);
        const parts = splitAtPoints(latex, cuts);
        if (parts.length > 1) {
            const cutPriorities = cuts.map((cut) => cut.priority);
            const totalChars = parts.reduce((sum, part) => sum + part.length, 0);
            const capacity = Math.max(8, Math.floor(totalChars * maxW / wholeSize.w * 0.92));
            const packed = packParts(parts, capacity, cutPriorities);
            let lines = packed.lines;
            // separators[k] 是 lines[k] 与 lines[k+1] 之间那个断点的优先级。
            // 细化（refine）会改变行数，所以这份账必须跟着一起维护，否则下面 mergeToBudget 无据可依
            // —— 上一版就是丢了这份账，导致 mergeToBudget 变成死代码、超行数时退化成"把尾部所有行拼成一行"。
            let seps = packed.separators;

            // 估计不准导致某行仍然超宽：把该行再拆一次。优先用行内断点，其次把长文本按字数硬切。
            // 返回 null 表示这一行没得拆（保持原样，交给单行缩放兜底）。
            const splitRowOnce = (line) => {
                const size = svgSize(renderSvg(line, display));
                if (!size || size.w <= maxW) return null;
                // 行内断点全部用上：不允许再 slice(1) 砍掉第一个 —— 该行的第一个断点就在行内，
                // 砍掉之后"只有两个断点的行"永远细化不了（splitAtPoints 自己会丢掉 index 0）。
                const linePoints = scanBreakPoints(line)
                    .filter((point) => point.index > 0 && point.index < line.length)
                    .sort((a, b) => a.index - b.index);
                const inner = splitAtPoints(line, linePoints);
                if (inner.length > 1) {
                    const innerChars = inner.reduce((sum, part) => sum + part.length, 0);
                    const innerCapacity = Math.max(8, Math.floor(innerChars * maxW / size.w * 0.92));
                    const repacked = packParts(inner, innerCapacity, linePoints.map((point) => point.priority));
                    if (repacked.lines.length > 1) return { rows: repacked.lines, seps: repacked.separators };
                }
                // 行内没有断点但仍然超宽：如果是"长中文"这类可切文本就按字数硬切
                // （否则整条式子只能走最后的整体缩小，实测 625px 的中文句子被压成 300x8）
                const text = pickTextArg(line);
                if (text) {
                    const pieces = hardSplitText(line, Math.max(4, Math.floor(text.text.length * maxW / size.w)));
                    if (pieces.length > 1) {
                        // 硬切出来的相邻两段之间按"最强"优先级记，后面的合并不会再动它们
                        return { rows: pieces, seps: pieces.slice(1).map(() => 0) };
                    }
                }
                return null;
            };

            // 每一轮只拆"当前仍超宽的行"，拆完再量一遍，最多两轮：
            // 一轮断点拆分之后行内可能还有别的断点/长文本可切（实测 486px 的行先按 = 切成
            // "= \text{…} = \frac{}{}"，其中第一段仍有 417px，必须再切一次才合格）。
            // 上一版只拆一轮、且不检查拆出来的行宽，于是那些超宽行只能靠"整条式子一起缩小"兜底。
            for (let round = 0; round < 2; round++) {
                const next = [];
                const nextSeps = [];
                let changed = false;
                for (let i = 0; i < lines.length; i++) {
                    const split = splitRowOnce(lines[i]);
                    if (split) {
                        changed = true;
                        for (let k = 0; k < split.rows.length; k++) {
                            if (k > 0) nextSeps.push(split.seps[k - 1] === undefined ? 0 : split.seps[k - 1]);
                            next.push(split.rows[k]);
                        }
                    } else {
                        next.push(lines[i]);
                    }
                    if (i + 1 < lines.length) nextSeps.push(seps[i]);
                }
                lines = next;
                seps = nextSeps;
                if (!changed) break;
            }
            if (lines.length > maxLines) {
                // 按断点优先级从弱到强合并相邻行（关系符/箭头保留，空格/逗号处先并）。
                // 旧写法是 lines.slice(maxLines-1).join('')：把尾部若干行拼成一条超宽行，
                // 结果整条式子都不合格 → 掉到最后的整体缩小（实测 79 项的加法链被压成 300x1 的一条线）。
                // 并列优先级时要按实测宽度挑"合并后最窄"的那对，否则会把所有内容并成一条巨行。
                const widths = lines.map((line) => {
                    const size = svgSize(renderSvg(line, display));
                    return size ? size.w : maxW;
                });
                lines = mergeToBudget(lines, seps, maxLines, widths);
            }
            const rendered = lines.map((line) => {
                const svg = renderSvg(line, display);
                return { latex: line, svg: svg, size: svgSize(svg) };
            });
            const allFit = rendered.every((entry) => entry.size && entry.size.w <= maxW);
            // 合并到上限内之后，个别行仍可能超宽（合并本来就是牺牲宽度换行数）。
            // 这种行交给 rasterize 的单行缩放兜底就够了，不再往下走去整条缩小：
            // 那会把本来合格的行也一起压小，而第三节只救得回"完全断不开的整句文本"。
            if (allFit || lines.length > 1) return { lines: rendered };
        }
    }

    // ── 三、兜底：整句长中文（\text{…} 里没有标点、断不开）按字数硬切成大字多行，而不是整体缩小 ──
    // 注意这里只处理"真的能切"的内容：hardSplitText 自己会挑最长的顶层文本宏，找不到就原样返回。
    // 旧写法是硬造一个 "\text{" + latex + "}" 再切，于是纯数学式子也被当文本切开 —— 命令名被拦腰
    // 截断（\text{\alph}）、下标括号错位（\text{}x_{1 … \text{}}）、变量连乘被渲染成直立字母。
    const chunk = Math.max(4, Math.floor(maxW / (REF_EM * 1.15)));
    const textPieces = hardSplitText(latex, chunk);
    if (textPieces.length > 1) {
        const hit = fitStack(null, textPieces, 2);
        if (hit) return { lines: [hit] };
    }

    return { single: whole, size: wholeSize };
}


async function main() {
    let port = 3000;
    const portArgIndex = process.argv.indexOf('--port');
    if (portArgIndex > -1 && process.argv[portArgIndex + 1]) {
        const parsedPort = parseInt(process.argv[portArgIndex + 1], 10);
        if (!isNaN(parsedPort) && parsedPort > 0) port = parsedPort;
    }

    log('正在初始化 MathJax…');

    const adaptor = liteAdaptor();
    RegisterHTMLHandler(adaptor);

    const tex = new TeX({
        packages: require('mathjax-full/js/input/tex/AllPackages.js').AllPackages
    });
    const svg = new SVG({ fontCache: 'none' });
    const mathjaxDocument = mathjax.document('', { InputJax: tex, OutputJax: svg });

    log('MathJax 初始化完成');

    // 中文字体在启动时就定下来并验证（不是等第一个请求）：日志里能看到最终用的是系统字体还是
    // 指定字体文件（含路径与 family），/health 一上来就能如实报告；失败也只是没有中文，服务照常。
    const cjk = ensureCjkFont();

    const app = express();
    app.use(express.json({ limit: '64kb' }));

    app.get('/health', (_req, res) => {
        // 幂等：选择/探针验证只做一次，这里只是把状态读出来
        ensureCjkFont();
        res.json({
            status: 'ok',
            uptime: process.uptime(),
            renderer: RENDERER_VERSION,
            ...cjkReport()
        });
    });

    app.post('/render', async (req, res) => {
        const clientIp = req.ip || req.connection.remoteAddress || 'unknown';

        if (!rateLimiter.allow(clientIp)) {
            res.set('Retry-After', '1');
            return res.status(429).json({ error: '请求过于频繁，请稍后再试' });
        }

        const {
            latex,
            display = true,
            color = '#FFFFFF',
            screenWidth = SCREEN_WIDTH,
            screenHeight = SCREEN_HEIGHT
        } = req.body;

        if (!latex || typeof latex !== 'string') {
            return res.status(400).json({ error: "缺少必需字段：'latex'" });
        }

        if (latex.length > MAX_LATEX_LENGTH) {
            return res.status(400).json({
                error: `LaTeX 内容过长，最大允许 ${MAX_LATEX_LENGTH} 字符`
            });
        }

        const sw = Math.max(Math.round(screenWidth), MIN_PNG_DIM);
        const sh = Math.max(Math.round(screenHeight), MIN_PNG_DIM);

        await renderMutex.acquire();
        try {
            const maxW = Math.min(sw - 20, MAX_RENDER_WIDTH);

            // MathJax 渲染成 SVG（尺寸度量与出图共用同一次转换）
            const renderSvg = (latexText, isDisplay) => {
                const containerNode = mathjaxDocument.convert(latexText, {
                    display: isDisplay,
                    em: REF_EM,
                    ex: REF_EM / 2,
                    containerWidth: sw
                });
                let svg = adaptor.outerHTML(adaptor.firstChild(containerNode));
                // 减小上标/下标字号，在小屏幕上提升可读性
                svg = svg.replace(/scale\(0\.707\)/g, 'scale(' + SCRIPT_SCALE + ')')
                    .replaceAll('currentColor', color);
                // 只有显式字体文件才改写 <text> 的 font-family（指到该字体的真实 family）。
                // 系统字体模式下保持 MathJax 原样：font-family="serif" 由 fontconfig 解析，
                // 缺字时 resvg 还会逐字回退到装得下的中文字体（真机上实测正常）。
                return fontState.status === 'file' ? applyCjkFamily(svg, fontState.family) : svg;
            };
            const svgSize = (svg) => {
                const size = getSvgExSize(svg);
                if (!size) return null;
                return { w: size.w_ex * REF_EM / 2, h: size.h_ex * REF_EM / 2 };
            };
            const rasterize = (svg, size) => {
                const fitTo = size && size.w > 0
                    ? { mode: 'width', value: Math.max(1, Math.round(Math.min(size.w, maxW))) }
                    : calcFitTo(svg, sw, sh);
                const pngData = new Resvg(svg, { fitTo, font: resvgFontOptions() }).render();
                return {
                    data: pngData.asPng().toString('base64'),
                    width: pngData.width,
                    height: pngData.height
                };
            };

            const wrapped = wrapLatex(latex, display, maxW, MAX_WRAP_LINES, renderSvg, svgSize);
            const naturalWidth = wrapped.size ? Math.round(wrapped.size.w) : null;
            const naturalHeight = wrapped.size ? Math.round(wrapped.size.h) : null;

            // 每个响应都带上字体状态，客户端/日志能直接看出这一屏的中文是不是真能显示
            // （客户端只读 cjkFont/cjkFontFamily，来源只在 /health 里报）
            const cjkFields = {
                cjkFont: fontState.status === 'file' || fontState.status === 'system',
                cjkFontFamily: fontState.family
            };

            if (!wrapped.lines) {
                const png = rasterize(wrapped.single, wrapped.size);
                log(`渲染：${png.width}×${png.height}  latex="${latex.slice(0, 30)}" (${clientIp})`);
                return res.status(200).json({
                    data: png.data,
                    width: png.width,
                    height: png.height,
                    naturalWidth,
                    naturalHeight,
                    renderer: RENDERER_VERSION,
                    ...cjkFields
                });
            }

            const lines = wrapped.lines.map((line) => {
                const png = rasterize(line.svg, line.size);
                return {
                    data: png.data,
                    width: png.width,
                    height: png.height,
                    naturalWidth: line.size ? Math.round(line.size.w) : null,
                    latex: line.latex
                };
            });
            const width = lines.reduce((max, line) => Math.max(max, line.width), 0);
            const height = lines.reduce((sum, line) => sum + line.height, 0) + (lines.length - 1) * LINE_GAP;
            log(`换行渲染：${lines.length} 行 ${width}×${height}  latex="${latex.slice(0, 30)}" (${clientIp})`);
            return res.status(200).json({
                // data 保留首行，旧版客户端至少能显示一行
                data: lines[0].data,
                width,
                height,
                lines,
                gap: LINE_GAP,
                naturalWidth,
                naturalHeight,
                renderer: RENDERER_VERSION,
                ...cjkFields
            });
        } catch (error) {
            console.error(`渲染失败 (${clientIp})：`, error.message);
            res.status(500).json({ error: '渲染失败', details: error.message });
        } finally {
            renderMutex.release();
        }
    });

    server.instance = app.listen(port, () => {
        console.log(`MathJax 服务已启动：http://localhost:${port}`);
        console.log(`  屏幕尺寸: ${SCREEN_WIDTH}×${SCREEN_HEIGHT}`);
        // 高度不设上限：长式子竖排后本就该超过一屏，由客户端滚动查看；只有宽度受 MAX_RENDER_WIDTH 限制。
        // calcFitTo 的高度分支只在拿不到 ex 尺寸的兜底路径里生效。
        console.log(`  尺寸策略: 统一基准字号，只缩不放 [宽 ≤ ${MAX_RENDER_WIDTH}px；高度不设上限，超出的由客户端滚动]`);
        console.log(`  长公式处理: 顶层运算符/箭头处换行（最多 ${MAX_WRAP_LINES} 行），行间距 ${LINE_GAP}px`);
        console.log(`  中文字体: ${cjk.status === 'file'
            ? `指定字体 family="${cjk.family}" dir=${cjk.dir} (${cjk.verdict})`
            : (cjk.status === 'system'
                ? `系统字体 family="${cjk.family}" (${cjk.verdict})`
                : `不可用（${cjk.reason}），中文会显示为空白`)}`);
        console.log(`  并发保护: 已启用 (单队列串行)`);
        console.log(`  频率限制: ${RATE_LIMIT_PER_SECOND} req/s per IP`);
        console.log(`  调试日志: ${DEBUG ? '开启' : '关闭'} (设置 DEBUG=1 开启)`);
    });
    server.port = port;
}

function shutdown(signal) {
    console.log(`\n收到 ${signal}，正在关闭…`);
    if (server.instance) {
        server.instance.close(() => {
            console.log('服务器已关闭');
            process.exit(0);
        });
        setTimeout(() => {
            console.error('强制退出');
            process.exit(1);
        }, 5000).unref();
    } else {
        process.exit(0);
    }
}

process.on('SIGTERM', () => shutdown('SIGTERM'));
process.on('SIGINT', () => shutdown('SIGINT'));

main().catch(err => {
    console.error('服务器启动失败：', err);
    process.exit(1);
});
