# 歌词时间轴回归检查

本地音乐播放页直接使用 `LyricTimeline` 解析 UTF-8 LRC，不依赖官方引擎的译文配对或逐字时间解析。
官方句子模型仍通过临时 JSON 同步，用于原有跟读和复读功能；不支持的扩展名保留官方处理。

支持普通 LRC、`[offset:+/-毫秒]`、多时间标签、乱序行、相同时间戳原文/译文和 Enhanced LRC：

```lrc
[00:01.000]<00:01.000>Hello <00:01.600>world<00:02.400>
[00:01.000]你好，世界
[00:03.000]没有逐字时间的句子按句高亮
```

同时间戳第一次出现的非空行作为原文，后续不同文本作为译文（多条译文保留换行）。
空时间行可结束上一句、表示间奏。逐字标签表示该词/字开始的绝对时间；重复段落会平移词时间。
没有逐字标签或词时间倒序时按句高亮，不推测演唱节奏。已知词起止时间使用缓存文字连续扫色；
末词缺少结束标签时在开始时间点亮，不补造拖音时长。时间标签若拆开 grapheme（如 e + 组合音符）
会回退为按句显示。
另外支持 `.yrc`（毫秒行标签及前置三元组词标签）、`.qrc`（明文后置双元组词标签或 `LyricContent` XML 包装），
以及 `.ttml` 的绝对时间子集：`p` 行、`span` 词、`begin/end/dur`（时钟、s/ms）、`ttm:role="x-translation"` 译文。
TTML 可嵌套普通 span；音译/元数据角色暂不显示。不支持帧时间、相对父节点时间偏移或样式布局。
XML 拒绝 DTD/实体，深度限制 32，行数限制 10000；不解密或解压 KRC/QRC。
文件读取上限为 2 MiB。

文件发现和宿主适配均支持上述四种扩展名，不依赖宿主直接解析新格式。
同名有效歌词优先级为 TTML > YRC > 明文 QRC > LRC；不可解析文件跳过；仍支持原有模糊匹配。
启用隐藏配对歌词时，同名的所有支持格式均隐藏。

在有 Qt5Core 开发包的主机运行：

```sh
g++ -std=c++17 -fPIC -Isrc tools/lyrics-probe/check.cpp \
  src/filemanager/player/LyricTimeline.cpp src/filemanager/player/LyricFormats.cpp $(pkg-config --cflags --libs Qt5Core) \
  -o /tmp/penmods-lyrics-check
/tmp/penmods-lyrics-check
```

格式及在线响应回归检查：

```sh
g++ -std=c++17 -fPIC -Isrc tools/lyrics-probe/formats.cpp \
  src/filemanager/player/LyricTimeline.cpp src/filemanager/player/LyricFormats.cpp \
  $(pkg-config --cflags --libs Qt5Core) -o /tmp/penmods-formats-check
/tmp/penmods-formats-check
# 可选：以真实 API 响应检查保存/重载后行数与词数不变
/tmp/penmods-formats-check /tmp/online-lyrics-sample.json
```

在线歌词入口沿用播放器的下载按钮，仅无可用歌词的本地音频可用。
网易云搜索支持修改关键词，选择候选后先预览原文/译文并标注逐字或逐句，再点击保存。
预览采用 44 px 高的整宽保存按钮，隐藏并禁用候选列表，背景拦截空白触摸；左侧返回先退出预览。
`python3 tools/lyrics-probe/preview.py` 在设备独立离屏窗口加载实际面板和公共控件（以 stub 替代网络模型），
检查保存按钮下沿、按钮下方空隙、歌词区的点击隔离和预览返回层级。
优先 YRC、无有效逐字时间则回退 LRC；译文与最近的原文时间在 500 ms 内匹配。
以 Enhanced LRC 保存，保留词起止时间和词间空隙，原目录已有同名 `.lrc` 时不覆盖。
保存后立即重载独立时间轴与宿主句子模型；切歌/关闭面板取消请求和预览；超时/网络异常可重试。

设备验收：将上述示例保存为音乐同名 `.lrc`；播放并验证逐词切换、译文分行、暂停保持、前后跳转、
切歌后无残留以及原文/译文/双语/隐藏切换。再验证一首普通 LRC 和一首没有歌词的歌曲。
修改同时涉及主库和资源库，部署时须将两者分别推送到临时文件，再重命名并重启应用。

时钟检查（Qt5Core 开发包和 moc）：

```sh
moc src/filemanager/player/LyricClock.h -o /tmp/moc-lyric-clock.cpp
g++ -std=c++17 -fPIC -Isrc tools/lyrics-probe/clock.cpp \
  src/filemanager/player/LyricClock.cpp /tmp/moc-lyric-clock.cpp \
  $(pkg-config --cflags --libs Qt5Core) -o /tmp/penmods-clock-check
/tmp/penmods-clock-check
```

正式组件的设备离屏检查（不接管设备界面）：

```sh
python3 tools/lyrics-probe/render.py
```

检查使用实际 `LyricLineItem` 和 `parseLyrics`，在 0/500/1500/2500/4000 ms 截图，
验证高亮像素持续增长而排版高度不变。日志和图片保存在 `build/lyrics-render-check/`。
软件离屏通过不能代替用户在可见 Wayland 页面上验证。
渲染检查另保存 `bilingual.png`，验证日语原文与两行中文译文的间距/高度；当前样例原文高 22 px、间隔 3 px、译文两行高 40 px。

`LyricClock` 只驱动显示，不改变音频解码位置。页面隐藏/暂停后停止显示定时器，重新显示时校准；
宿主位置长期未推进时最多外推 1500 ms（乘以倍速）。播放期间每 32 个有效位置变化输出一次
`[LyricClock] samples=... medianMs=... p95Ms=...`；停止刷新时输出剩余样本，供后续校准宿主采样频率。
歌词视口已将标题固定在列表外，并在标题下方裁切歌词，底部预留进度条触摸区。
纵向拖动采用 Flickable 原生滚动，单击仍暂停；移动超过阈值后不触发单击。
惯性滚动期间的点按只停止滚动。浏览时暂停自动定位，滚动结束约 3 秒后恢复到最新当前句。
歌词点句跳转和距离缩放尚待后续实现。无歌词时仅隐藏列表，标题保持可见；暂停遮罩与空状态提示限制在标题下方。
上下边缘各用 8 px 背景色渐变柔化过渡，不引入整屏离屏纹理。
标题为 26 px、顶部留 4 px、底部为 12 px，视口约 128 px；单曲原文缓存只裁去外围透明行，不缩小文字。
原译文间距 3 px，译文每行 20 px。已知词时间的长句可按当前演唱视觉行自动跟随，未知词时间不猜测节奏。
原文跨多行时仍保持原字号；可手动浏览查看后续原文和译文。

切句颜色回归检查：

```sh
python3 tools/lyrics-probe/render.py --handoff
```

从生产 `YAudioPlayerLrcContent.qml` 提取实际 `LyricLineItem` 绑定，在设备独立离屏窗口检查
句尾全蓝、切句淡出期间和淡出后的蓝色保持，以及回退前的未播放白色恢复。
已唱句子以蓝色降低亮度，不能因 `active` 变为 false 而在滚动前闪回白色。
截图和日志位于 `build/lyrics-handoff-check/`。

固定标题与手势回归检查：

```sh
python3 tools/lyrics-probe/render.py --gestures
```

该检查只向独立的离屏 QQuickWindow 发送鼠标事件，不使用 `/dev/uinput`，不操作真实设备界面。
它加载生产 `YAudioPlayerLyricView.qml` 和 `LyricLineItem`，验证普通点击、纵向拖动、横向划动、
惯性滚动中点击、浏览期间进度变化不抢回、延迟恢复后的实际可见位置、超长歌词当前视觉行跟随，并比较滚动前后的标题像素。
日志和截图位于 `build/lyrics-gesture-check/`。此外覆盖无歌词时的标题可见性/像素不变，
并通过纯白内容验证上下边缘趋近背景色、渐变中间存在半透明像素且中心仍保持原亮度。
`empty.png` 和 `fade.png` 分别记录这两组验证。

静音演示歌词使用本目录 `demo.lrc`。每句都有明确的末尾结束标签：

```lrc
[00:00.000]<00:00.000>Hello <00:04.000>world & music<00:10.000>
```

缺少最后的 `<00:10.000>` 会使 `world & music` 没有可插值的结束时间，按当前未知时长规则在 4 秒时整段点亮。
本演示将这段明确设为 4–10 秒，不代表能从普通 LRC 推断出歌声节奏。更新设备文件后，需要先暂停并从文件列表重新打开歌曲，才能替换内存中的旧时间轴。

演示数据回归检查：

```sh
g++ -std=c++17 -fPIC -Isrc tools/lyrics-probe/fixture.cpp \
  src/filemanager/player/LyricTimeline.cpp src/filemanager/player/LyricFormats.cpp $(pkg-config --cflags --libs Qt5Core) \
  -o /tmp/penmods-fixture-check
/tmp/penmods-fixture-check tools/lyrics-probe/demo.lrc
```
