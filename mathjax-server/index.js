const express = require('express');

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
const SVG_EX_RE = /(?:width|height)="([0-9.]+)ex"/g;

const server = { instance: null, port: 0 };

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
const rateLimiter = new RateLimiter(1000, 10);

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
 * 策略：以公式在 REF_EM 下的自然尺寸为基准，然后施加约束：
 *   MIN_HEIGHT — 将过小的公式放大到可读尺寸
 *   MAX_WIDTH  — 宽度不超出屏幕
 *   MAX_HEIGHT — 高度不超出屏幕
 *
 * 结果在可读性与屏幕适配之间取得平衡，
 * 不会将简单公式放大到填满整个屏幕。
 */
function calcFitTo(svgStr, screenW, screenH) {
    const size = getSvgExSize(svgStr);
    if (!size) return { mode: 'width', value: screenW };

    const MAX_W = Math.min(screenW - 20, 300);
    const MAX_H = Math.round(screenH * 0.65);
    const MIN_H = 28;

    // 概念字号 REF_EM 下的自然像素尺寸
    // MathJax 默认 ex = em/2，因此 1ex = REF_EM/2 px
    const natW = size.w_ex * REF_EM / 2;
    const natH = size.h_ex * REF_EM / 2;

    let scale = 1;

    // 1. 宽度不超过屏幕
    scale = Math.min(scale, MAX_W / natW);

    // 2. 确保最小可读高度，但前提是不违反宽度约束
    const minHScale = MIN_H / natH;
    if (minHScale > scale && natW * minHScale <= MAX_W) {
        scale = minHScale;
    }

    // 3. 高度不超过最大限制
    scale = Math.min(scale, MAX_H / natH);

    const finalW = Math.round(natW * scale);
    return { mode: 'width', value: Math.max(finalW, MIN_PNG_DIM) };
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

    const app = express();
    app.use(express.json({ limit: '64kb' }));

    app.get('/health', (_req, res) => {
        res.json({ status: 'ok', uptime: process.uptime() });
    });

    app.post('/render', async (req, res) => {
        const clientIp = req.ip || req.connection.remoteAddress || 'unknown';

        if (!rateLimiter.allow(clientIp)) {
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
            // 单次 MathJax 渲染（SVG 输出不依赖于 em 和 containerWidth 参数）
            const containerNode = mathjaxDocument.convert(latex, {
                display,
                em: REF_EM,
                ex: REF_EM / 2,
                containerWidth: sw
            });
            let svgOutput = adaptor.outerHTML(adaptor.firstChild(containerNode));
            // 减小上标/下标字号，在小屏幕上提升可读性
            svgOutput = svgOutput.replace(/scale\(0\.707\)/g, 'scale(' + SCRIPT_SCALE + ')');
            svgOutput = svgOutput.replaceAll('currentColor', color);

            const fitTo = calcFitTo(svgOutput, sw, sh);

            const resvg = new Resvg(svgOutput, { fitTo });
            const pngData = resvg.render();
            const { width, height } = pngData;

            const pngBuffer = pngData.asPng();
            log(`渲染：${width}×${height}  latex="${latex.slice(0, 30)}" (${clientIp})`);
            res.status(200).json({
                data: pngBuffer.toString('base64'),
                width,
                height
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
        console.log(`  尺寸策略: 自然尺寸 + 约束 [28px ≤ 高 ≤ 65%, 宽 ≤ 300px]`);
        console.log(`  并发保护: 已启用 (单队列串行)`);
        console.log(`  频率限制: 10 req/s per IP`);
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
