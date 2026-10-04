# 有道词典笔离线词典（`localdict/*.dat`）格式分析

> 目标：搞清楚词典笔的离线词典能不能扩展/替换，容器格式是什么，自造词典的难度有多大。
> 结论先行：**格式已完全逆向并有可用的读写工具**（`tools/dict-probe/`）；能否"扩展"取决于
> 引擎里硬编码的词典槽位和每个词典各自的 JSON 数据格式，而不是容器本身。

## 1. 设备现状

| 项 | 值 |
|---|---|
| 词典目录 | `/oem/YoudaoDictPen/output/localdict` → 符号链接 → `/uresource/resource/localdict` |
| 挂载 | `/dev/mmcblk1p11  /uresource  ext4 ro`（**只读**，实测 `touch` 报 `Read-only file system`） |
| 容量 | 分区 4.0G，已用 3.0G，**剩 791M**；其中 localdict 约 1.1G、localsound 1.4G、transform_model 338M、Fonts 219M |
| 其它可写分区 | `/userdisk` 剩 3.4G、`/userdata` 剩 173M、rootfs `/` 剩 165M（`/oem/YoudaoDictPen/output` 本身在 rootfs 上，**可写**） |
| 词典文件 | 26 个，其中 V2 词典 21 个 + V1「点读包/笔顺」4 个 + `word-frequency.txt` |
| 清单 | `/userdata/DictPenData/resource/localdict.json`（24 条，NetEase 资源系统；`oxfordV2`/`websterV2` **不在清单里**，属授权词典，PenMods 已通过 `FT::InitFeature` 打开对应 feature 位） |

`/uresource` 只读，但厂商自己会临时改挂载：`YResourceLaunchMananger`（主程序里）在应用资源更新时会执行
`mount -o remount,rw /uresource` → `cp` + `md5sum` 校验 → `mount -o remount,ro /uresource`，配置放在
`/userdisk/resource/localdict/ota_resource/ota_update.json`（或 `.../updateresource/update_resource.json`），
字段：`update_version` / `old_version` / `update_state` / `update_file_size` / `{update,add,remove}_file_list`，
每项 `{file_name, md5, path, destination_path}`。也就是说**官方就把 `/uresource` 当可写目标用**。

## 2. 加载链路

```
YDictQueryEnginePrivate::loadDict(name)
  path = getenv("APP_ROOT_PATH") + "/localdict/" + name     # runDictPen: export APP_ROOT_PATH=/oem/YoudaoDictPen/output
  YQueryDictManager::open(path)
    先试 <path> 里在 ".dat" 前插入 "V2" → "xxxV2.dat"      # 存在则用 CYDOfflineDictParser
    否则回退到原来的 "xxx.dat"                              # 存在则用 QYdDictManager(flag=1)
  缓存进 QHash<QString, shared_ptr<YQueryDictManager>>，按需惰性加载
```

* `APP_ROOT_PATH` 是**每次查询时现读的环境变量**（`QProcessEnvironment::systemEnvironment()`），改名/改指向不需要改二进制。
* ⚠️ **厂商会删文件**：`YQueryDictManager::open(base)` 在找到 `xxxV2.dat` 之后会把同目录的 `xxx.dat` 删掉
  （实测把 13.5MB 的 `webster.dat` 副本删了）。所以不要让 `xxx.dat` 与 `xxxV2.dat` 并存——自造词典就按
  引擎的约定命名成 `xxxV2.dat`，或者交给 PenMods 的自定义词典页（它会自动改名，见 §5）。
* 词典名硬编码在 `YDictQueryEnginePrivate` 构造函数里，共 **33 个槽位**（`char/word/idiom/ancientword/poem_*
  /ce/ce-large/ec/eckid/collins_primary/example/sentence/pinyin/seniordict/ssat/gre/sat/ielts/toefl/webster
  /oxford/longchaock/longchaokc/e2k/k2e/e2k-large/k2e-large/common_strokes/not_common_strokes_1/2
  /interactivelearning{chinese,pinyin,english}/cekid`）。
* 本机有数据 21 个（含 `common_strokes` / `not_common_strokes_*` 这类不带 V2 后缀的 V1 文件），
  **缺 12 个**：`cekid` `collins_primary` `example` `pinyin` `longchaock` `longchaokc` `e2k` `k2e`
  `e2k-large` `k2e-large` `interactivelearning{chinese,pinyin,english}` —— 这些功能代码都在，只缺数据文件。

`dict_id`（头部 u64）实测：

| id | 文件 | 名称 | id | 文件 | 名称 |
|---|---|---|---|---|---|
| 6000 | ecV2 | 英汉离线词库 | 6015 | wordV2 | 汉语词典 |
| 6001 | ceV2 | 汉英离线词库 | 6016 | ancientwordV2 | 古代汉语词典 |
| 6004 | ssatV2 | SSAT学习词典 | 6017 | ce-largeV2 | 吴光华汉英大辞典 |
| 6005 | satV2 | SAT学习词典 | 6018 | poem_authorV2 | 古诗文作者词典 |
| 6006 | greV2 | GRE学习词典 | 6019 | poem_sentenceV2 | 古诗文例句词典 |
| 6007 | toeflV2 | 托福学习词典 | 6020 | poem_dataV2 | 古诗文赏析词典 |
| 6008 | ieltsV2 | 雅思学习词典 | 6039 | idiomV2 | 熟语词典 |
| 6009 | seniordictV2 | 高中词典 | 6040 | eckidV2 | 新少儿英汉离线词库 |
| 6010 | sentenceV2 | 双语例句词典 | 1 | common_strokes / not_common_strokes_* / B-B00* / bookreading（V1） | 点读包/笔顺 |
| 6011 | websterV2 | 韦氏词典 | | | |
| 6012 | oxfordV2 | 牛津高阶词典 | | | |
| 6014 | charV2 | 汉字词典 | | | |

id 只是记录信息：引擎不校验（自造文件写任意 id 也被厂商读取器接受）。

## 3. 容器格式（V2，`version == 0x1004`）

```
header
  u64  version                 0x1004 = V2 词典；0x1044 = V1（点读包/笔顺，名字不带 V2）
  u64  dict_id                 见上表
  u8   name_len ; name[…]      UTF-8 显示名（"SAT学习词典"）
  u64  0x4000000000000060      固定值
  u64  word_count              总词条数（引擎不校验，工具会校验）
  u32  51200                   解压缓冲区初始提示值
  u32  config_len ; config[…]  ASCII "index1Size=<字节数>[&configInfo=<key>]"
  (V1 额外) u32 magic_len ; "3575AA5DDA9D1DBF"
u32  bucket_count              桶数（小端）
bucket_count × {
    word[…], 0x09,
    u32 LE regionA_off         该桶 index2 blob 的文件偏移
    u32 LE bucket_words        该桶词条数
    u32 LE regionA_len         压缩长度
    u32 LE regionB_off         该桶记录 blob 的文件偏移
    u32 LE regionB_len         压缩长度
}
bucket_count × zlib(XOR(index2))   index2: (word, 0x09, u32 BE 该词记录在 regionB 内的偏移)×
bucket_count × zlib(XOR(records))  records: (varint 长度, JSON)×
```

细节（每一条都踩过）：

* **XOR**：对压缩后的字节 `i` 异或 `(7*i) % 34967`（`QYdDictManager::decode`）。磁盘上 = `XOR(zlib(payload))`。
* **varint 长度**：`b0<=0x3F` → 1 字节；`<=0x7F` → `((b0-0x40)<<8)|b1`；`<=0xBF` → 24 位；否则 32 位。
  所以单条记录上限 4MB-1；现有词典里最长的单条记录是 `websterV2` 的 `call`（17528 字节，走 3 字节形式）——
  只按 2 字节前缀写的字典在遇到这种记录时会写坏，工具已支持完整 varint。
* **排序键**：index1 的桶首词、以及每个桶的 index2 词表，都是按**词的小写形式**排序的（`tolower`）。
  `eckidV2` 里 `a` 排在 `A` 前，按字节序则不成立；用字节序重建的 `satV2` 用厂商读取器查不到词
  （index1 的二分查找错位），改用小写键后与原文件完全一致。这是自造词典最容易踩的坑。
* **区域布局**：所有桶的 regionA 从 index1 之后连续排列，regionB 紧接 regionA 之后连续排列，
  `config` 里的 `index1Size = 4 + Σ(len(word)+1+20)`（即含 4 字节 bucket_count 的 index1 段总长）。
* **`&configInfo=`**：`charV2`/`eckidV2`/`toeflV2` 等词典的 config 带 `configInfo=$configInfo$_<时间戳>`，
  并且桶 0 的第 0 个「词」就是该 key，记录用另一种 framing（`0x127b` 开头）。重建时按原样搬运即可。
* **V1（0x1044）**：同样有 header/index1/两个区域，但记录 framing 不同（前缀高半字节为 8，且头后有 1 字节），
  内容是点读包/笔顺数据（`content_type` / `book_id` / base64 图片）。本工具只正确解析 V2。

## 4. 引擎实现里另外两点值得记住

* V1 文件走 `QYdDictManager(bool=1)`，V2 文件走 `CYDOfflineDictParser`（`open(path, err)` **返回 0 表示成功**），
  两者的 header/index1/区域解析一致，只是记录的 framing 与查询接口不同。
* `YQueryDictManager::lookUp` 会把结果里第一个 `\t` 之前的内容丢掉再交给 QML —— 所以记录可以是
  `{"wordHead":…}` 这种纯 JSON，也可以是 `word\tJSON`。

## 5. 扩展词典的可行路线

1. **官方资源更新通道**（最省事）：把 `.dat` 放到 `/userdisk/resource/localdict/ota_resource/`，
   配一份 `ota_update.json`，由 `YResourceLaunchMananger` 自行 `remount,rw /uresource` → 拷贝 → 校验 → `remount,ro`。
   副作用最小，且用的是官方机制。
2. **改指向**（可用空间最大）：`/oem/YoudaoDictPen/output/localdict` 是符号链接，父目录可写，可改指
   `/userdisk/...`（或 `mount --bind`，厂商对 `/.config` 就是这么做的），拿到 3.4G 空间；
   代价是要和官方资源流程共存。
3. **替换内容**：往 21 个已有槽位写新数据（更新某本词典、或把开源词库转换进去）。
4. **填充 12 个空槽位**：`e2k/k2e/e2k-large/k2e-large`（韩/日↔英）、`collins_primary`、
   `interactivelearning*`、`example`、`pinyin`、`longchaock/longchaokc` —— 这些查询入口和 UI 都在，
   只缺数据文件；把符合格式与 schema 的数据放进去即可点亮（官方 dmgr 包同样是这个格式）。
5. **真正新增一本引擎不认识的词典**：引擎没有对应查询函数，需要自己写 QML 页面/插件去查
   （`libDictManager.so` 导出了通用接口 `QYdDictManager::open/lookUp/hasWord`、`CYDOfflineDictParser::*`，
   PenMods 插件可以直接复用）。
6. **Mod 里已经做好的"真·扩展词典"**：把 `.dat` 放进 `/userdisk/PenMods/dicts/`，**正常查词/扫描**就会在
   结果页最上面多出一段以该词典命名的内容；设置里的入口（**更多设置 → 扫描查询 → 自定义词典**）只是用来
   查看装了哪些词典和单独试查。实现见 `src/dict/CustomDict.cpp` + `dicts/YDictTypeDtPenMods.qml`：

   * 启动/打开页面时扫描目录；V2 容器若没按引擎约定命名会自动改名为 `xxxV2.dat`（并避免出现
     `xxx.dat` 与 `xxxV2.dat` 并存，否则厂商会删文件，见 §2）。
   * 查询走厂商读取器，但只用 `QString` 接口（libPenMods 是 libc++，不能碰厂商的 `std::string`）。
   * 结果页按 `YEnumWrapper::DictType` 选渲染器，宿主没有"自定义词典"这个类型，于是取一个宿主不会派发的
     值（900）并在 PenMods 的 QML 里给 `YDictTypeBase` 加一个自己的渲染器。
   * 注入点：hook `YResultManager::addResult`（所有查询路径的必经之路），在宿主本次查询的**第一条**结果
     之前插入我们那段，再调宿主自己的 `setTop()` 把它固定在第一位——顺序放在最前是因为结果页第一段之后
     往往紧跟着很长的一整段内置词典，放到后面用户要滑很多屏才看得到。
   * 因此自造词典不必装进 `/oem`/`/uresource`，也不依赖任何引擎槽位，扫描/手输/历史重查都会带出来。
   * **推荐 schema**（`dicts/YDictPenModsRender.js` 按这个排版；别名也认，缺字段就少一行）：

     ```json
     {
       "word": "type",                       // 词；别名 headword/ew/title
       "phonetic": "/taɪp/",                 // 可选；别名 phone/usphone/ukphone/pr
       "lang": "en",                         // 可选：en/zh/ja/ko，缺省按内容判断（供发音用）
       "tags": ["CET4", "高中"],              // 可选；别名 tag/labels
       "defs": [                             // 释义；别名 def/trans/tran/meaning/definition
         {"pos": "n.", "tran": "类型；种类"},
         {"pos": "v.", "tran": "打字"}
       ],
       "examples": [                         // 可选；别名 example/sentences
         {"en": "Can you type more slowly?", "zh": "你能再慢点打字吗？"}
       ],
       "note": "备注",                        // 可选；别名 notes/usage/tip
       "source": "来源"                       // 可选；别名 from
     }
     ```

     没命中这些字段的 JSON 走兜底排版：`key.subkey  值` 一行一条（数组展开成 `key[1]`、嵌套到两层、
     跳过空值和 `http(s)://` / `data:` 这类链接值、最多 40 行）。记录不是 JSON 时原样显示。
     词头与英文例句旁会生成发音按钮（`soundCenter.play(词, lang, phonetic)`），排版与按钮见
     `components/YCustomDictContentView.qml`；示例见 `tools/dict-probe/example-dict.jsonl`。

### 自造词典的难度评估

| 环 | 难度 | 说明 |
|---|---|---|
| 容器读写 | ★☆☆ | 已完全掌握，`dat_probe.py` 约 200 行即可读、写、重建 |
| 数据来源 | ★★☆ | 需要开源词库（ECDICT / CC-CEDICT / 汉典…）+ 转换成 JSON 记录；纯文本/结构化数据都不难 |
| 每本词典的 JSON schema | ★★★ | **真正的门槛**：同一个引擎对不同类型的词典期望不同字段（英文 `wordHead/content/synos/sentences`，中文 `details/meanings/pinyin`，少儿 `word/phonics/brief_meaning`）。加 12 个空槽位时要先搞清该槽位查询函数的消费方（QML）读哪些字段 —— 有现成同族词典可类比（如 `eckidV2` ↔ `collins_primary`），没有样本的（韩/日）需要逆向 |
| 引擎识别与安装 | ★☆☆ | 名字固定、目录可配置、id 不校验；装到可写目录 + 改链接/走官方通道即可 |
| 端到端 UI 验证 | ★★☆ | 需要在设备上真的装进去并重启（涉及 `/uresource` 可写化或 bind mount），见下节 |

## 6. 验证方法与结果

工具见 `tools/dict-probe/`（`dat_probe.py` 读写/校验，`datcheck`/`appcheck` 用**厂商自己的**读取器验证）。

* `dat_probe.py validate` 对本地 18 个文件（含 6 个完整原文件、2 个带 >16KB 记录的词典）逐一通过
  （`index1Size`、区域连续性、varint 长度与 JSON 结尾、词数与总排序键）。
* **重建对拍**：用工具把 `satV2`(4464 词)、`eckidV2`(38558)、`charV2`(20947)、`poem_authorV2`(3186)
  完整重建，再用厂商读取器查询，行为与原文件**逐词一致**（含大小写混排词头、CJK 词头、未命中返回空）。
* **从零生成**：`build()` 造的小词典也能被 app 级 `YQueryDictManager` + `CYDOfflineDictParser`
  正常 `open`、命中词条、未知词返回空。
* **端到端（真机 UI）**：在设备上把 `/oem/YoudaoDictPen/output/localdict` 临时改指到 `/userdisk/dictprobe/localdict`
  （里面是 25 个指向 `/uresource` 原文件的符号链接 + 1 个自造的 `websterV2.dat`，只改了 `type` 这一条的
  第一个释义），重启 app，在 查词翻译 里输入 `type`：结果页的**「韦氏大学英语词典」**区块显示
  `1. a: PENMODS-E2E-OK｜本条目数据由 PenMods 自造词典容器提供（tools/dict-probe）`，
  而非原厂商文本；恢复符号链接并重启后该区块回到原文。
  这一步证明了「自造容器 → 装到设备 → app 从磁盘加载 → 查询 → QML 渲染」整条链路，而且**没有写入 `/uresource`
  一个字节**（该文件 md5 前后一致）。

## 7. 尚未验证 / 后续

* 只做过「替换已有词典」的端到端验证；**往 12 个空槽位装数据**（`e2kV2.dat` 之类）还没在真机上跑过，
  需要先确认对应功能的 UI 入口与 JSON schema。
* 12 个空槽位各自的 JSON schema 尚未逐个确认（`collins_primary`、`interactivelearning*` 可先类比
  `eckidV2`；韩/日词典无样本）。
* V1（点读包/笔顺）的记录 framing 未完全解析——本工具只保证 V2 的正确性。
* 真机验证时用的临时手法（改 `localdict` 符号链接 + 重启 app）在正式方案里应改为更稳的
  bind mount，或者走官方资源更新通道（见 §5 路线 1）。
