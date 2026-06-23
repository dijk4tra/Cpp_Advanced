# SearchEngine V1 第一期离线建库

这个目录实现搜索引擎项目第一期：离线生成关键字推荐和网页搜索所需的数据文件。

## 目录结构

```text
01_SearchEngine_V1/
├── bin/                  # 编译后的可执行程序输出目录
├── build/                # CMake 构建目录
├── conf/                 # 配置文件
│   └── config.conf
├── data/
│   ├── corpus/           # 语料库
│   │   ├── CN
│   │   ├── EN
│   │   └── webpages
│   ├── stopwords/        # 停用词
│   ├── dict/             # 词典库输出目录
│   └── index/            # 索引库、网页库、倒排索引输出目录
├── docs/                 # 文档预留目录
├── include/
│   ├── common/           # 通用模块头文件
│   ├── offline/          # 一期离线模块头文件
│   └── online/           # 二期在线模块预留头文件
├── src/
│   ├── common/           # 通用模块实现
│   ├── offline/          # 一期离线模块实现
│   └── online/           # 二期在线模块预留实现
└── tests/                # 测试预留目录
```

这个结构比把所有文件平铺在 `src/` 和 `include/` 下更适合后续扩展。第一期只实现 `common` 和 `offline`，第二期可以直接在 `online` 中继续写服务器相关代码。

## 配置文件

路径集中放在 [conf/config.conf](/home/lws/my_project/Cpp_Advanced/课件/07_搜索引擎项目/01_SearchEngine_V1/conf/config.conf) 中，格式是简单的 `key=value`。

关键配置项：

```text
en_corpus_dir=data/corpus/EN
cn_corpus_dir=data/corpus/CN
webpage_corpus_dir=data/corpus/webpages

en_stop_words=data/stopwords/en_stopwords.txt
cn_stop_words=data/stopwords/cn_stopwords.txt

en_dict=data/dict/dict_en.dat
cn_dict=data/dict/dict_cn.dat
en_dict_index=data/index/index_en.dat
cn_dict_index=data/index/index_cn.dat

pages=data/index/pages.dat
offsets=data/index/offsets.dat
invert_index=data/index/invert_index.dat
```

如果你以后移动语料或想改输出文件名，优先改配置文件，不需要改 C++ 源码。

## 功能

1. 关键字推荐离线建库
   - 读取英文语料和英文停用词，生成 `data/dict/dict_en.dat`
   - 根据英文词典生成 `data/index/index_en.dat`
   - 读取中文语料和中文停用词，只保留纯汉字 token，生成 `data/dict/dict_cn.dat`
   - 根据中文词典生成 `data/index/index_cn.dat`，索引 key 只包含完整 UTF-8 汉字

2. 网页搜索离线建库
   - 读取 `data/corpus/webpages` 中的 XML 文件
   - 使用 `tinyxml2` 提取 `<item>` 文档
   - 使用 `simhash` 去重
   - 生成 `data/index/pages.dat`
   - 生成 `data/index/offsets.dat`
   - 生成 `data/index/invert_index.dat`

## 依赖

需要提前安装：

```bash
sudo apt install cmake g++ libtinyxml2-dev
```

还需要按照课程 PDF 说明安装这些头文件库：

```text
cppjieba
utfcpp
simhash
```

默认代码会从 `/usr/local/include` 查找：

```text
/usr/local/include/cppjieba/Jieba.hpp
/usr/local/include/utfcpp/utf8.h
/usr/local/include/simhash/Simhasher.hpp
```

## 构建运行

在 `01_SearchEngine_V1` 目录中执行：

```bash
# 第一步：根据 CMakeLists.txt 生成构建系统
# -S .      指定源码目录是当前目录
# -B build  指定构建目录是 build
cmake -S . -B build
# 第二步：调用构建系统真正编译代码
# --build build 表示在 build 目录下执行构建
cmake --build build
./bin/offline_builder
```

运行成功后会生成：

```text
data/dict/dict_en.dat
data/dict/dict_cn.dat
data/index/index_en.dat
data/index/index_cn.dat
data/index/pages.dat
data/index/offsets.dat
data/index/invert_index.dat
```

其中 `data/dict/dict_cn.dat` 是中文关键字推荐词典，只应包含全部由汉字组成的词语；英文、数字、标点、圈号数字、罗马数字、特殊符号和中英混合 token 会被过滤。英文候选词由 `data/dict/dict_en.dat` 单独负责，网页搜索倒排索引仍按网页内容分词结果构建。

## 代码阅读顺序

建议按这个顺序学习：

1. `src/offline/offline_main.cc`：整体流程入口。
2. `include/common/Config.h` 和 `src/common/Config.cc`：配置文件读取。
3. `DirectoryScanner.h/.cc`：目录扫描。
4. `TextUtils.h/.cc`：停用词、英文清洗、UTF-8 切字、XML 转义。
5. `KeywordProcessor.h/.cc`：中英文词典库和索引库生成。
6. `PageProcessor.h/.cc`：网页提取、去重、网页库、偏移库、倒排索引。

## 输出文件格式

词典库：

```text
word frequency
```

中文词典 `data/dict/dict_cn.dat` 的 `word` 应全部为汉字。如果文件前面出现英文或特殊符号，通常说明中文分词后的 token 过滤规则过宽，需要检查 `TextUtils::is_chinese_word()` 和 `KeywordProcessor::create_cn_dict()`。

索引库：

```text
character lineNo1 lineNo2 lineNo3 ...
```

中文索引 `data/index/index_cn.dat` 的 `character` 应是单个完整 UTF-8 汉字。构建时要用 utfcpp 按 Unicode 字符切分，不能用 `word[i]` 按字节切分。

网页偏移库：

```text
docId offset length
```

倒排索引库：

```text
keyword docId weight docId weight ...
```

## 说明

1. `data/corpus/CN`、`data/corpus/EN`、`data/corpus/webpages` 是符号链接，避免重复复制大语料。
2. `bin/` 和 `build/` 目录保留是为了贴合常见 C++ 工程结构，但生成的二进制和中间文件不建议提交。
3. 代码注释比较详细，目标是便于你按模块学习第一期实现。
