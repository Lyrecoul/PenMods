# dict-probe

词典笔离线词典容器（`localdict/*.dat`）的工具集合，从 `libDictManager.so` 逆向得到，并用厂商自己的
读取器验证过。容器格式本身见 [`../../doc/DICT_FORMAT_ANALYSIS.md`](../../doc/DICT_FORMAT_ANALYSIS.md)；
**想自己造一本词典？看 [`../../doc/CUSTOM_DICT_GUIDE.md`](../../doc/CUSTOM_DICT_GUIDE.md)。**

| 文件 | 作用 |
|---|---|
| `dat_probe.py` | 纯 Python 的容器读取 / 校验 / **写入原型**（`info`、`list`、`lookup`、`validate`、`build`、`rebuild`） |
| `datcheck.cxx` | 加载 `libDictManager.so` 导出的 `QYdDictManager`，按路径查询 `.dat` —— 走的是厂商代码，没有重新实现 |
| `appcheck.cxx` | 驱动*应用层*的 `YQueryDictManager`（V2／回退解析 + `CYDOfflineDictParser`），和 `YDictQueryEngine` 的做法完全一致 |

## 布局

```
header                     u64 version(0x1004 V2 / 0x1044 V1), u64 dict_id, u8 name_len + UTF-8 name,
                           0x4000000000000060, u64 word_count, u32 51200, u32 config_len + "index1Size=.."
                           (+ u32 magic_len + "3575AA5DDA9D1DBF" for V1 files)
u32 bucket_count
bucket_count x { word, 0x09, u32 LE regionA_off, u32 LE bucket_words, u32 LE regionA_len,
                             u32 LE regionB_off, u32 LE regionB_len }
bucket_count x zlib(XOR(index2))     index2 = (word, 0x09, u32 BE offset into regionB)*
bucket_count x zlib(XOR(records))    records = (varint length, JSON)*
```

* 数据块第 *i* 个字节的 XOR 密钥是 `(7*i) % 34967`（作用于 zlib 流，即先压缩再异或）。
* 记录的长度前缀是 64 进制 varint（`<=0x3F` 1 字节，`<=0x7F` 2 字节，`<=0xBF` 3 字节，其余 4 字节）。
* 词条必须按 **ASCII 小写化**后的形式排序——厂商的二分查找用的就是这个键。
* 配置里含 `&configInfo=` 的词典，其第 0 桶第 0 条记录是一段特殊封装的标记
  （例如 charV2 的 `$configInfo$_1673067260354`）；`rebuild` 会原样搬运它。
* 输出文件名要写成 `<something>V2.dat`：引擎会在 ".dat" 前插入 "V2"，并且一旦启用了 V2 文件就会
  **删掉同名的裸名 `.dat`**。PenMods 的自建词典支持（`src/dict/CustomDict`）接受任意文件名，会自动
  帮你改名成 `V2` 约定。
* 想在笔上用生成的词典，把它拷到 `/userdisk/PenMods/dicts/` 即可——之后每次正常查词，它都会以自己
  一个分区（标题就是容器的名字）出现在结果页最上方，也可以在 更多设置 → 扫描查询 → 自定义词典 里浏览。
* 记录由 `YDictPenModsRender.js` 渲染。按推荐 schema 写就能得到和内置词典一样的外观
  （`word`/`phonetic`/`tags`/`defs[{pos,tran}]`/`examples[{en,zh}]`/`note`/`source`，见
  `example-dict.jsonl` 与 `doc/DICT_FORMAT_ANALYSIS.md` §5）；其它 JSON 仍会按 `键  值` 的形式渲染。

## 使用

```sh
# 查看 / 导出
python3 dat_probe.py info     /tmp/satV2.dat
python3 dat_probe.py lookup   /tmp/charV2.dat 汉
python3 dat_probe.py list     /tmp/satV2.dat 20
python3 dat_probe.py validate /tmp/*.dat

# 造词典：词 -> 任意 JSON 记录（详见推荐 schema）
python3 dat_probe.py build /tmp/custom.dat entries.jsonl [--name 词典名] [--id 6099]

# 从表格/TSV 造：word, phonetic, pos, tran, example_en, example_zh
python3 dat_probe.py tsv /tmp/custom.dat entries.tsv --name "我的词典"

# 往返转换现有词典（导出全部记录，写出一个等价的容器）
python3 dat_probe.py rebuild /tmp/satV2.dat /tmp/satV2-rebuilt.dat

# 用厂商读取器校验（不需要改动设备）
QT=$HOME/PenMods/aarch64-linux-qt-5.15.2
aarch64-linux-gnu-g++ -O2 -std=c++11 -fPIC -I$QT/include -I$QT/include/QtCore \
  -o appcheck appcheck.cxx -ldl -L$QT/lib -lQt5Core -Wl,--allow-shlib-undefined
aarch64-linux-gnu-g++ -O2 -std=c++11 -o datcheck datcheck.cxx -ldl
adb push appcheck datcheck /tmp/ && adb shell 'chmod +x /tmp/appcheck /tmp/datcheck'

# 把容器放到引擎查找的位置（$APP_ROOT_PATH/localdict/<name>V2.dat）
adb shell mkdir -p /userdisk/dictprobe/localdict
adb push /tmp/custom.dat /userdisk/dictprobe/localdict/customV2.dat
adb shell 'LD_LIBRARY_PATH=/oem/YoudaoDictPen/output/libs:/userdisk/Qtlib \
  /tmp/appcheck /userdisk/dictprobe/localdict/custom.dat apple'
```

`appcheck`/`datcheck` 只会读传给它们的那个文件——不会碰 `/oem` 或只读的 `/uresource`，所以候选词典
可以在正式安装之前先校验。

## 已验证

对 `satV2`（4464 词）、`eckidV2`（38558）、`charV2`（20947）、`poem_authorV2`（3186）和 `websterV2`
（89686，包含需要 3 字节长度形式的记录）执行 `rebuild`，生成的容器交给厂商自己的读取器查询，结果与
原文件完全一致（包括大小写混合的词头、中日韩词头，以及未知词的拒绝），而从零生成的容器也能被应用层的
`YQueryDictManager`/`CYDOfflineDictParser` 路径正常接受。

在 YDP02X 上做过端到端验证：把 `localdict` 临时指向一个 `/userdisk` 目录，里面放的是指向原文件的符号
链接，外加一本自造的 `websterV2.dat`（只改了它的 `type` 记录）。重启应用后，查词翻译 → `type` 会在
“韦氏大学英语词典”分区里显示改过的文本（`1. a: PENMODS-E2E-OK｜…`）；恢复符号链接并重启后，原文回来，
只读的 `/uresource` 文件 md5 也仍是原来的值。详见 `doc/DICT_FORMAT_ANALYSIS.md` §6。
