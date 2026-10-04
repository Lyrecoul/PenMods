# 自造词典：怎么构造

> 面向"我想给词典笔加一本自己的词典"。容器的二进制格式见
> [`DICT_FORMAT_ANALYSIS.md`](DICT_FORMAT_ANALYSIS.md)，本文只讲怎么造、怎么装、怎么排查。
> 一句话：**在电脑上把词条写成 JSONL 或 TSV，用 `tools/dict-probe/dat_probe.py` 打包成 `.dat`，
> 推进 `/userdisk/PenMods/dicts/`，然后在笔上正常查词就能看到。**

## 0. 准备

* 电脑上有一份本仓库（`tools/dict-probe/` 就够了），**只需要 python3**，没有第三方依赖。
* 不需要编译任何东西：设备侧 PenMods 已经支持读这个目录（`src/dict/CustomDict`）。

## 1. 准备词条内容

推荐先按"推荐 schema"写，结果页会排成内置词典那样（粗体词头 + 音标 + 词性释义 + 例句）：

```json
{
  "word": "type",                       // 词；别名 headword/ew/title
  "phonetic": "/taɪp/",                 // 可选；别名 phone/usphone/ukphone/pr
  "lang": "en",                         // 可选：en/zh/ja/ko，缺省按词自动判断
  "tags": ["CET4", "高中"],              // 可选；别名 tag/labels
  "defs": [                             // 释义；别名 def/trans/tran/meaning/definition
    {"pos": "n.", "tran": "类型；种类"},
    {"pos": "v.", "tran": "打字；测定…的类型"}
  ],
  "examples": [                         // 可选；别名 example/sentences
    {"en": "Can you type more slowly?", "zh": "你能再慢点打字吗？"}
  ],
  "note": "备注",                        // 可选；别名 notes/usage/tip
  "source": "来源"                       // 可选；别名 from
}
```

字段不全没关系，缺谁就少一行；**不按这个 schema 写也能用**，其它字段会排成
`key 值` 的键值行（数组展开、最多两层、跳过空值和 `http(s)://`/`data:` 链接）。
想先用现成的例子练手：`tools/dict-probe/example-dict.jsonl`。

**发音按钮**：词头旁和每条英文例句旁会自动出现喇叭按钮（用宿主自己的 `soundCenter`）。
`phonetic` 会一起交给 TTS（汉字多音字更准），语言取 `lang` 字段，没写就按内容判断
（含汉字→zh、假名→ja、谚文→ko、其它→en）。

### 三种输入形式

| 输入 | 写法 | 命令 |
|---|---|---|
| JSONL（最灵活） | 一行一个 `{"word": "...", "record": {…}}` | `dat_probe.py build` |
| TSV（表格/Excel 导出最省事） | 6 列：`词 音标 词性 释义 例句原文 例句译文`，同词多行自动合并 | `dat_probe.py tsv` |
| 自有数据脚本 | 自己把 CSV/数据库/纯文本转成上面任意一种 | 同左 |

TSV 例子（`\t` 分隔，空列留空）：

```
apple	/ˈæpl/	n.	苹果；苹果树	An apple a day keeps the doctor away.	一天一苹果，医生远离我。
apple		n.	苹果（果实）
banana	/bəˈnɑːnə/	n.	香蕉
```

## 2. 打包成容器

```sh
cd <repo>

# JSONL
python3 tools/dict-probe/dat_probe.py build 我的词典V2.dat 我的词条.jsonl --name "我的词典" --id 6099

# TSV（Excel/表格导出，留空列用空字符串）
python3 tools/dict-probe/dat_probe.py tsv   我的词典V2.dat 我的词条.tsv --name "我的词典" --id 6099
```

* `--name` 就是笔上那一段的**标题**（红色那行）；不写就是"PenMods 测试词典"。
* `--id` 是容器里的词典 id，随便给个 6000～6999 的数即可（引擎不校验，也不与内置词典冲突）。
* `--bucket 512` 可调分桶词数（一般不用动）。
* **建议文件名以 `V2.dat` 结尾**：厂商引擎先在 `.dat` 前插 `V2` 再回退原名，而且它一旦用了
  `xxxV2.dat` 就会删掉同目录的 `xxx.dat`。写成别的名字也行——PenMods 会帮你改名（见下）。

## 3. 本地自检（强烈建议）

```sh
python3 tools/dict-probe/dat_probe.py validate 我的词典V2.dat     # 结构/排序/长度一致性
python3 tools/dict-probe/dat_probe.py info     我的词典V2.dat     # 名字、词头数、分桶
python3 tools/dict-probe/dat_probe.py lookup   我的词典V2.dat type # 查一条看看内容
```

想再用**厂商自己的读取器**验证一遍（不改设备上的任何东西）：

```sh
QT=$HOME/PenMods/aarch64-linux-qt-5.15.2
aarch64-linux-gnu-g++ -O2 -std=c++11 -fPIC -I$QT/include -I$QT/include/QtCore \
  -o /tmp/appcheck tools/dict-probe/appcheck.cxx -ldl -L$QT/lib -lQt5Core -Wl,--allow-shlib-undefined
adb push /tmp/appcheck 我的词典V2.dat /userdisk/
adb shell 'chmod +x /userdisk/appcheck; LD_LIBRARY_PATH=/oem/YoudaoDictPen/output/libs:/userdisk/Qtlib \
  /userdisk/appcheck /userdisk/我的词典V2.dat type'
```

## 4. 装到笔上

```sh
adb shell mkdir -p /userdisk/PenMods/dicts
adb push 我的词典V2.dat /userdisk/PenMods/dicts/
```

然后在笔上**正常查词**（扫描 / 查词翻译 / 历史重查）——命中的话结果页最上面就会出现以词典名
命名的一段。**不需要重启**：查询前会自动重扫目录。也可以在
**更多设置 → 扫描查询 → 自定义词典** 里查看装了哪几本、单独试查。

> 笔上手动输入小提示：输入键盘只露出第一行字母（q…p），在键盘区域**向上滑**就能露出下面两行。

## 5. 从现有数据批量转换

大多数情况下你手上是 CSV/Excel/纯文本，用一个几十行的脚本转成 TSV 或 JSONL 即可。要点：

* **按"词"排序**：工具内部会按**小写化后的词**排序（引擎就是这么二分查找的），所以源数据不必预先排序。
* **一条记录别超过 4MB**（容器用 varint 存长度，理论 4MB 上限）；正常词条几 KB 而已。
* **词头就是词本身**：自定义词典不参与内置词典的"翻译/变形"逻辑，所以英文请用小写词头、
  中文用汉字词头，才能被 `查词` 命中的同一个键查到（大小写不敏感，`Apple`/`apple` 都能命中）。
* 例句、音标、词性没有就留空，不要塞占位符。

小例子（CSV → JSONL，然后按第 2 步打包）：

```python
import csv, json, sys
with open(sys.argv[1], encoding="utf-8") as src, open(sys.argv[2], "w", encoding="utf-8") as dst:
    for row in csv.DictReader(src):                  # 列名: word,pos,tran
        record = {"word": row["word"], "defs": [{"pos": row["pos"], "tran": row["tran"]}]}
        dst.write(json.dumps({"word": row["word"], "record": record}, ensure_ascii=False) + "\n")
```

## 6. 排查

| 现象 | 原因 / 处理 |
|---|---|
| 结果页没出现那一段 | 词没命中（大小写/空格？用 `lookup` 确认）／文件不在 `/userdisk/PenMods/dicts/`／记录不是 JSON 且内容为空 |
| 段标题不是我的词典名 | 忘了 `--name`，或容器头里的名字没写对（`info` 看一眼） |
| 我的 `xxx.dat` 变成 `xxxV2.dat` 了 | PenMods 按引擎约定改名（见上）；这是正常的，也避免厂商删文件 |
| 目录里同时有 `xxx.dat` 和 `xxxV2.dat` | 会被跳过并在日志里警告——这两个并存时厂商引擎会**删掉**其中一个，所以请只留一个 |
| `validate` 报 index1Size / 排序错误 | 容器被手改过？重新用工具生成 |
| 词典太多、笔变卡 | 每个被查询过的词典会把索引读进内存（几百 KB～几 MB，最多缓存 4 本）；把不用的移出目录即可 |
| 想要"整本替换内置词典" | 那是另一条路：见 `DICT_FORMAT_ANALYSIS.md` §5 的 `/oem/.../localdict` 与官方资源更新通道 |

## 7. 可选的坑与进阶

* **不支持 mdx/mdd/stardict 直接转换**：这些格式需要自己解析后按上面的 schema 输出。
* 想彻底自己写容器（不依赖 Python 工具）？格式在 `DICT_FORMAT_ANALYSIS.md` §3 有完整layout，
  也可以直接 `rebuild` 一本现成词典再把记录替换掉。
* 词典名、词头数都会显示在设置页和结果页段标题上，所以名字起得清楚一点更实用
  （例如 `2026考研红宝书`）。
