const express = require('express');
const { Resvg } = require('@resvg/resvg-js');

const { mathjax } = require('mathjax-full/js/mathjax.js');
const { TeX } = require('mathjax-full/js/input/tex.js');
const { SVG } = require('mathjax-full/js/output/svg.js');
const { liteAdaptor } = require('mathjax-full/js/adaptors/liteAdaptor.js');
const { RegisterHTMLHandler } = require('mathjax-full/js/handlers/html.js');

const app = express();
app.use(express.json());

async function main() {
    let port = 3000;
    const portArgIndex = process.argv.indexOf('--port');
    if (portArgIndex > -1 && process.argv[portArgIndex + 1]) {
        const parsedPort = parseInt(process.argv[portArgIndex + 1], 10);
        if (!isNaN(parsedPort) && parsedPort > 0) port = parsedPort;
    }

    console.log('正在初始化 MathJax…');

    const adaptor = liteAdaptor();
    RegisterHTMLHandler(adaptor);

    const tex = new TeX({
        packages: require('mathjax-full/js/input/tex/AllPackages.js').AllPackages
    });
    const svg = new SVG({ fontCache: 'none' });
    const mathjaxDocument = mathjax.document('', { InputJax: tex, OutputJax: svg });

    console.log('MathJax 初始化完成，服务器已就绪');

    app.post('/render', (req, res) => {
        const {
            latex,
            display = true,
            color = '#FFFFFF',
            maxWidth = 300  // 默认适配 320px 屏幕，保留边距
        } = req.body;

        if (!latex) {
            return res.status(400).json({ error: "缺少必需字段：'latex'" });
        }

        try {
            // 1. LaTeX → SVG
            const containerNode = mathjaxDocument.convert(latex, {
                display,
                em: 12,
                ex: 6,
                containerWidth: 300
            });
            let svgOutput = adaptor.outerHTML(adaptor.firstChild(containerNode));

            // 2. 替换 currentColor 为实际颜色值
            svgOutput = svgOutput.replaceAll('currentColor', color);

            // 3. SVG → PNG（进程内，无外部依赖）
            //    fitTo width 按比例缩放，宽度上限 maxWidth，高度自动保持宽高比
            const clampedWidth = Math.min(Math.max(Math.round(maxWidth), 32), 600);
            const resvg = new Resvg(svgOutput, {
                fitTo: { mode: 'width', value: clampedWidth }
            });
            const pngData = resvg.render();
            const pngBuffer = pngData.asPng();
            const { width, height } = pngData;

            console.log(`渲染成功：${width}×${height}`);
            res.status(200).json({
                data: pngBuffer.toString('base64'),
                width,
                height
            });
        } catch (error) {
            console.error('渲染过程中发生错误：', error.message);
            res.status(500).json({ error: '渲染失败', details: error.message });
        }
    });

    app.listen(port, () => {
        console.log(`MathJax 服务已启动：http://localhost:${port}`);
    });
}

main().catch(err => {
    console.error('服务器启动失败：', err);
});
