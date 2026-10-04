# dict-probe

Tools for the pen's offline dictionary container (`localdict/*.dat`), recovered from
`libDictManager.so` and verified against the vendor's own reader.
The format itself is documented in [`../../doc/DICT_FORMAT_ANALYSIS.md`](../../doc/DICT_FORMAT_ANALYSIS.md);
**want to build your own dictionary? follow [`../../doc/CUSTOM_DICT_GUIDE.md`](../../doc/CUSTOM_DICT_GUIDE.md).**

| File | What it does |
|---|---|
| `dat_probe.py` | pure-Python reader / validator / **writer prototype** for the container (`info`, `list`, `lookup`, `validate`, `build`, `rebuild`) |
| `datcheck.cxx` | loads `libDictManager.so`'s exported `QYdDictManager` and queries a `.dat` by path — vendor code, no reimplementation |
| `appcheck.cxx` | drives the *app-level* `YQueryDictManager` (V2/fallback resolution + `CYDOfflineDictParser`) exactly like `YDictQueryEngine` does |

## Layout

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

* `XOR` key for byte *i* of a blob is `(7*i) % 34967` (applied to the zlib stream, i.e. compress then xor).
* A record's length prefix is a base-64 varint (`<=0x3F` 1 byte, `<=0x7F` 2 bytes, `<=0xBF` 3 bytes, else 4).
* Words must be ordered by their **ASCII-lowercased** form — the vendor's binary search uses that key.
* Bucket 0 record 0 of dictionaries whose config contains `&configInfo=` is a differently framed
  marker record (e.g. charV2 `$configInfo$_1673067260354`); `rebuild` carries it over verbatim.
* Name the output `<something>V2.dat`: the engine inserts "V2" before ".dat" and, once it uses the V2
  file, **deletes a sibling bare-named `.dat`**. PenMods' custom-dictionary support (`src/dict/CustomDict`)
  accepts any name and renames the file to the `V2` convention for you.
* To use a generated dictionary on the pen, copy it to `/userdisk/PenMods/dicts/` — it then shows up as its
  own section (titled with the container's name) at the top of every normal query result, and can also be
  browsed in 更多设置 → 扫描查询 → 自定义词典.
* Records are rendered by `YDictPenModsRender.js`. Follow the recommended schema for native-looking entries
  (`word`/`phonetic`/`tags`/`defs[{pos,tran}]`/`examples[{en,zh}]`/`note`/`source`, see
  `example-dict.jsonl` and `doc/DICT_FORMAT_ANALYSIS.md` §5); any other JSON still renders as styled
  `key  value` lines.

## Usage

```sh
# inspect / dump
python3 dat_probe.py info     /tmp/satV2.dat
python3 dat_probe.py lookup   /tmp/charV2.dat 汉
python3 dat_probe.py list     /tmp/satV2.dat 20
python3 dat_probe.py validate /tmp/*.dat

# author: word -> arbitrary JSON record (详见推荐 schema)
python3 dat_probe.py build /tmp/custom.dat entries.jsonl [--name 词典名] [--id 6099]

# author from a table/TSV: word, phonetic, pos, tran, example_en, example_zh
python3 dat_probe.py tsv /tmp/custom.dat entries.tsv --name "我的词典"

# round-trip an existing dictionary (extract every record, write an equivalent container)
python3 dat_probe.py rebuild /tmp/satV2.dat /tmp/satV2-rebuilt.dat

# validate with the vendor's reader (no device modification needed)
QT=$HOME/PenMods/aarch64-linux-qt-5.15.2
aarch64-linux-gnu-g++ -O2 -std=c++11 -fPIC -I$QT/include -I$QT/include/QtCore \
  -o appcheck appcheck.cxx -ldl -L$QT/lib -lQt5Core -Wl,--allow-shlib-undefined
aarch64-linux-gnu-g++ -O2 -std=c++11 -o datcheck datcheck.cxx -ldl
adb push appcheck datcheck /tmp/ && adb shell 'chmod +x /tmp/appcheck /tmp/datcheck'

# stage the container where the engine looks for it ($APP_ROOT_PATH/localdict/<name>V2.dat)
adb shell mkdir -p /userdisk/dictprobe/localdict
adb push /tmp/custom.dat /userdisk/dictprobe/localdict/customV2.dat
adb shell 'LD_LIBRARY_PATH=/oem/YoudaoDictPen/output/libs:/userdisk/Qtlib \
  /tmp/appcheck /userdisk/dictprobe/localdict/custom.dat apple'
```

`appcheck`/`datcheck` only read the file that is given to them — they never touch
`/oem` or the read-only `/uresource`, so a candidate dictionary can be validated
before anything is installed.

## Verified

`rebuild` of `satV2` (4464 words), `eckidV2` (38558), `charV2` (20947), `poem_authorV2` (3186) and
`websterV2` (89686, including records that need the 3-byte length form) produces containers that the
vendor's own reader answers identically to the originals (including mixed-case headwords, CJK headwords
and unknown-word rejection), and a container generated from scratch is accepted by the app-level
`YQueryDictManager`/`CYDOfflineDictParser` path.

End-to-end check performed on a YDP02X: `localdict` was temporarily repointed at a `/userdisk` directory
containing symlinks to the originals plus one self-made `websterV2.dat` whose `type` record was edited.
After an app restart, 查词翻译 → `type` showed the edited text inside the “韦氏大学英语词典” block
(`1. a: PENMODS-E2E-OK｜…`); after restoring the symlink and restarting, the original text came back and
the read-only `/uresource` file still had its original md5. See `doc/DICT_FORMAT_ANALYSIS.md` §6.
