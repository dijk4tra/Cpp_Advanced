# SearchEngine V3.1 技术文档

`03_SearchEngine_V3.1` 是搜索引擎项目第三期的持续优化版，包含离线建库、TLV/HTTP
在线查询、BM25 排序、多级缓存、浏览器前端和统一可观测性：

1. `offline_builder`：离线建库程序，生成关键词推荐和网页搜索所需的数据文件。
2. `search_server`：基于 `muduo` 的在线服务，同时提供 TLV 协议端口和浏览器 HTTP 端口。
3. `cache`：L1 分片完整 W-TinyLFU（可回退 LRU）、Redis L2 持久连接池、二级缓存组合、
   singleflight、空结果缓存、TTL 抖动和缓存统计。

当前 V3.1 的核心能力：

1. 关键词推荐最终 JSON 结果增加缓存。
2. 网页搜索最终 JSON 结果增加缓存。
3. 网页库正文不再启动时全量加载到内存，改为根据 `offsets.dat` 从 `pages.dat` 按需读取。
4. 文档展示信息和动态摘要片段增加细粒度缓存。
5. TLV 服务和 HTTP 服务统一依赖 `CachedSearchService`，避免两套入口重复实现缓存逻辑。
6. 网页检索忽略 OOV 并使用 OR 召回，排序由 TF-IDF 升级为 BM25。
7. HTTP/1.1 支持 keep-alive，Redis 使用有上限的持久连接池。
8. spdlog 统一管理离线/在线日志，提供异步写入、按大小滚动、阶段耗时和慢请求日志。
9. 前端静态资源和 JSON API 由同一 HTTP 服务直接返回，包含 favicon 和标准静态资源 404。

## 1. 项目目标

当前 V3.1 项目包含三条完整链路。

离线建库链路：

```text
原始语料 -> offline_builder -> 词典/字符索引/网页库/偏移库/BM25 倒排索引与文档统计
```

在线查询链路：

```text
客户端 -> TLV 或 HTTP -> CachedSearchService -> KeywordRecommender / WebSearcher -> JSON
```

缓存链路：

```text
请求 -> L1 分片 W-TinyLFU -> Redis L2 -> singleflight 回源 -> 写入缓存
```

整体数据流：

```mermaid
flowchart TD
    CFG[conf/config.conf] --> OFF[offline_builder]
    CFG --> ON[search_server]

    ENC[data/corpus/EN] --> OFF
    CNC[data/corpus/CN] --> OFF
    WEB[data/corpus/webpages] --> OFF
    SW[data/stopwords] --> OFF

    OFF --> ED[data/dict/dict_en.dat]
    OFF --> CD[data/dict/dict_cn.dat]
    OFF --> EI[data/index/index_en.dat]
    OFF --> CI[data/index/index_cn.dat]
    OFF --> P[data/index/pages.dat]
    OFF --> O[data/index/offsets.dat]
    OFF --> INV[data/index/invert_index.dat]
    OFF --> STATS[data/index/bm25_doc_stats.dat]

    Client[TLV 客户端] <-->|TLV + JSON| ON
    Browser[浏览器] <-->|HTTP + JSON| ON

    ON --> CACHED[CachedSearchService]
    CACHED --> L1[L1 ShardedWTinyLfuCache]
    L1 --> L2[RedisCache]
    CACHED --> REC[KeywordRecommender]
    CACHED --> SEARCH[WebSearcher]

    ED --> REC
    CD --> REC
    EI --> REC
    CI --> REC
    INV --> SEARCH
    STATS --> SEARCH
    O --> SEARCH
    P -.按需读取.-> SEARCH
    SW --> SEARCH
```

## 2. 目录结构

```text
03_SearchEngine_V3.1/
├── CMakeLists.txt
├── README.md
├── bin/
│   ├── offline_builder
│   └── search_server
├── conf/
│   └── config.conf
├── data/
│   ├── corpus/
│   ├── stopwords/
│   ├── dict/
│   └── index/
├── docs/
│   ├── 项目第三期开发思路.md
│   ├── 项目第三期进一步优化思路.md
│   ├── 项目第三期缓存技术详解.md
│   ├── 项目第三期HTTP压测报告.md
│   ├── 项目第三期HTTP压测报告-V3.1.md
│   ├── 项目第三期缓存改造流程.md
│   ├── 项目第三期开发进度.md
│   └── 项目第三期优化进度.md
├── include/
│   ├── cache/
│   │   ├── Cache.h
│   │   ├── TinyLfuFrequencySketch.h
│   │   ├── ShardedWTinyLfuCache.h
│   │   ├── ShardedLruCache.h
│   │   ├── RedisCache.h
│   │   ├── TwoLevelCache.h
│   │   └── CachedSearchService.h
│   ├── common/
│   │   ├── Config.h
│   │   ├── DirectoryScanner.h
│   │   ├── Logger.h
│   │   └── TextUtils.h
│   ├── offline/
│   │   ├── KeywordProcessor.h
│   │   └── PageProcessor.h
│   └── online/
│       ├── ProtocolCodec.h
│       ├── SearchServer.h
│       ├── WebHttpServer.h
│       ├── KeywordRecommender.h
│       ├── WebSearcher.h
│       ├── PageLibrary.h
│       └── DynamicAbstract.h
├── src/
│   ├── cache/
│   │   ├── TinyLfuFrequencySketch.cc
│   │   ├── ShardedWTinyLfuCache.cc
│   │   ├── ShardedLruCache.cc
│   │   ├── RedisCache.cc
│   │   ├── TwoLevelCache.cc
│   │   └── CachedSearchService.cc
│   ├── common/
│   ├── offline/
│   └── online/
├── tests/
│   ├── README.md
│   ├── tlv_client/
│   │   ├── Makefile
│   │   ├── README.md
│   │   └── tlv_client.cc
│   ├── cache_policy/
│   │   ├── Makefile
│   │   ├── README.md
│   │   ├── cache_policy_test.cc
│   │   ├── cache_policy_benchmark.cc
│   │   └── sample_trace.txt
│   ├── http_load/
│   │   ├── Makefile
│   │   ├── README.md
│   │   ├── http_load_test.py
│   │   └── sample_queries.txt
│   └── web_searcher_recall_test.cc
└── www/
    ├── index.html
    ├── styles.css
    ├── app.js
    ├── favicon.svg
    └── README.md
```

模块职责：

| 模块 | 作用 |
| --- | --- |
| `common` | 配置读取、目录扫描、UTF-8 和文本处理工具 |
| `offline` | V3.1 离线建库，生成词典、网页库、BM25 倒排索引和文档统计 |
| `online` | muduo TLV 服务、HTTP 服务、推荐和搜索算法 |
| `cache` | 第三期缓存接口、本地缓存、Redis 缓存、二级缓存和缓存服务层 |
| `tests` | TLV 协议客户端、缓存策略测试/日志回放和 HTTP 端到端压测 |
| `www` | 浏览器搜索页面 |

## 3. 构建与运行

### 3.1 依赖

项目使用 C++17。当前实现依赖：

```text
cmake
g++
tinyxml2
muduo_net
muduo_base
pthread
hiredis
cppjieba
utfcpp
simhash
nlohmann/json
spdlog
```

依赖用途：

| 依赖 | 用途 |
| --- | --- |
| `tinyxml2` | 解析网页 XML 语料和按需读取出的 `<doc>` 片段 |
| `muduo` | 在线 TCP/TLV 服务和 HTTP 服务 |
| `hiredis` | Redis L2 缓存客户端 |
| `cppjieba` | 中文分词 |
| `utfcpp` | UTF-8 字符拆分和码点判断 |
| `simhash` | 离线网页近似去重 |
| `nlohmann/json` | 请求、响应和文档缓存 JSON 序列化 |
| `spdlog` | 异步控制台日志、滚动文件日志和统一格式化 |

### 3.2 编译

必须从项目根目录运行，因为配置文件中的路径是相对路径。

```bash
cd 课件/07_搜索引擎项目/03_SearchEngine_V3.1
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON
cmake --build build -j2
```

构建完成后生成：

```text
bin/offline_builder
bin/search_server
```

CMake 会查找并链接：

```text
tinyxml2
hiredis
muduo_net
muduo_base
pthread
spdlog::spdlog
```

如果缺少 `spdlog`、`hiredis`、`tinyxml2` 或 `muduo`，CMake 配置阶段会直接报错。

### 3.3 运行离线建库

```bash
./bin/offline_builder
```

执行流程：

1. 读取 `conf/config.conf`。
2. 创建 `data/dict`、`data/index`、`bin`。
3. 调用 `KeywordProcessor::process()` 生成中英文词典和字符索引。
4. 调用 `PageProcessor::process()` 生成网页库、偏移库、BM25 倒排索引和文档长度统计。

成功结束时，控制台和 `logs/offline_builder.log` 会输出类似：

```text
offline build finished elapsed_ms=19676 output_dirs=data/dict,data/index
```

### 3.4 运行在线服务

```bash
./bin/search_server
```

启动流程：

1. 读取 `conf/config.conf`。
2. 加载中英文词典和字符索引。
3. 加载中文停用词、网页偏移库、BM25 倒排索引和文档长度统计。
4. 记录 `pages.dat` 路径，网页正文后续按需读取。
5. 按配置创建 L1 本地缓存、Redis L2 缓存和 `TwoLevelCache`。
6. 为 `WebSearcher` 设置文档展示信息缓存和动态摘要缓存。
7. 创建 `CachedSearchService`，封装关键词推荐和网页搜索最终结果缓存。
8. 启动 TLV 服务和 HTTP 服务。

默认监听：

```text
0.0.0.0:8888    TLV 协议服务
0.0.0.0:18888   浏览器 HTTP 服务
```

启动成功时，控制台和 `logs/search_server.log` 会输出类似：

```text
L1 cache enabled policy=wtinylfu capacity=4096 shards=32 ttl=600 empty_ttl=60 ...
Redis L2 enabled host=127.0.0.1 port=6379 db=0 pool_size=16 wait_timeout_ms=20
TLV server listening address=0.0.0.0:8888 threads=4
HTTP server listening address=0.0.0.0:18888 threads=8 keep_alive=true
```

浏览器访问：

```text
http://127.0.0.1:18888
```

## 4. 配置文件

配置文件路径固定为 `conf/config.conf`，格式是简单的 `key=value`。所有相对路径都以
程序启动时的工作目录为基准，因此应从 V3.1 项目根目录运行二进制。

### 4.1 配置文件导航

`config.conf` 已按实际使用模块分为 `[01]`~`[09]` 九段：

| 分区 | 主要 key/内容 | 主要使用者 |
| --- | --- | --- |
| `[01]` 离线输入 | `*_corpus_dir`、`*_stop_words` | `offline_builder` |
| `[02]` 离线产物 | `*_dict`、`*_index`、`pages`、`offsets`、`bm25_doc_stats` | 离线生成，在线加载 |
| `[03]` 检索结果 | `bm25_*`、`*_topk`、`abstract_length` | 推荐/搜索模块 |
| `[04]` TLV 服务 | `server_*`、`io_threads`、`max_message_size` | `SearchServer` |
| `[05]` HTTP 服务 | `http_*`、`www_root` | `WebHttpServer` |
| `[06]` 日志 | `log_*`、`muduo_log_level`、`slow_request_ms` | 离线/在线进程 |
| `[07]` 缓存总控 | `cache_enabled`、`cache_version`、`*_ttl_seconds` | 缓存业务层 |
| `[08]` L1 | `l1_*` | 进程内缓存 |
| `[09]` Redis L2 | `redis_*` | `RedisCache` |

开关统一使用 `1=开启 / 0=关闭`；`_seconds` 和 `_ms` 分别表示秒和毫秒。离线数据重建或
结果格式变化后应更新 `cache_version`，避免命中 Redis 中的旧结果。

### 4.2 缓存配置

```text
cache_enabled=1
cache_version=search_engine_v3_003_bm25

l1_cache_enabled=1
l1_cache_capacity=4096
l1_cache_shards=32
l1_cache_policy=wtinylfu
l1_wtinylfu_window_percent=1
l1_wtinylfu_protected_percent=80
l1_wtinylfu_frequency_sample_multiplier=10
l1_cache_ttl_seconds=600
l1_empty_ttl_seconds=60
cache_ttl_jitter_seconds=30

document_cache_ttl_seconds=600
abstract_cache_ttl_seconds=600
cache_stats_log_interval=100

redis_cache_enabled=1
redis_host=127.0.0.1
redis_port=6379
redis_db=0
redis_connect_timeout_ms=20
redis_command_timeout_ms=20
redis_pool_size=16
redis_pool_wait_timeout_ms=20
redis_l1_backfill_ttl_seconds=600
```

缓存配置说明：

| 配置 | 说明 |
| --- | --- |
| `cache_enabled` | 缓存总开关，`0` 表示完全关闭缓存 |
| `cache_version` | 缓存版本，离线数据重建后应更新，避免命中旧数据 |
| `l1_cache_enabled` | 是否启用进程内 L1 本地缓存 |
| `l1_cache_capacity` | L1 缓存总容量，按 key 数量计 |
| `l1_cache_shards` | L1 分片数量，用于降低多线程锁竞争 |
| `l1_cache_policy` | L1 淘汰策略：`wtinylfu`（默认）或 `lru`（回退/对照） |
| `l1_wtinylfu_window_percent` | Window LRU 占分片容量百分比，内部限制到 `[1, 99]` |
| `l1_wtinylfu_protected_percent` | Main Protected 占 Main 容量上限百分比 |
| `l1_wtinylfu_frequency_sample_multiplier` | 触发 Count-Min Sketch 老化和 Doorkeeper 重置的采样窗口倍数 |
| `l1_cache_ttl_seconds` | 普通搜索/推荐结果 TTL，`<= 0` 表示不过期 |
| `l1_empty_ttl_seconds` | 空结果 TTL，用于缓存不存在查询 |
| `cache_ttl_jitter_seconds` | TTL 随机抖动秒数，减少大量 key 同时过期 |
| `document_cache_ttl_seconds` | 文档展示信息缓存 TTL |
| `abstract_cache_ttl_seconds` | 动态摘要片段缓存 TTL |
| `cache_stats_log_interval` | 每处理多少次请求打印一次缓存统计，`<= 0` 表示关闭 |
| `redis_cache_enabled` | 是否启用 Redis L2 缓存 |
| `redis_host` / `redis_port` / `redis_db` | Redis 连接信息 |
| `redis_connect_timeout_ms` | Redis 建连超时 |
| `redis_command_timeout_ms` | Redis 命令读写超时 |
| `redis_pool_size` | Redis 持久连接池最大连接数 |
| `redis_pool_wait_timeout_ms` | 连接池耗尽时等待空闲连接的最长时间，超时后降级 |
| `redis_l1_backfill_ttl_seconds` | Redis 命中后回填 L1 的 TTL |

### 4.3 日志配置

| 配置 | 说明 |
| --- | --- |
| `log_level` | spdlog 级别，默认 `info`；逐请求和搜索阶段详情使用 `debug` |
| `log_dir` | 日志目录，生成 `offline_builder.log` 和 `search_server.log` |
| `log_max_file_size_mb` | 单个文件达到该大小后滚动 |
| `log_max_files` | 每个 logger 最多保留的滚动文件数 |
| `log_async_queue_size` | 异步队列容量，队列满时阻塞而不是丢失错误日志 |
| `slow_request_ms` | HTTP/TLV 业务处理超过该时长时记录 WARN |
| `muduo_log_level` | muduo 框架日志级别，默认 WARN 避免逐连接 INFO |

离线日志在 INFO 级别记录词典、字符索引、网页提取、去重、网页库和 BM25
索引等阶段的开始、结束、耗时与统计。在线普通逐请求日志和搜索分阶段耗时放在
DEBUG；默认 INFO 只保留启动配置、周期缓存统计；慢请求和可恢复异常使用 WARN，
启动失败使用 CRITICAL。请求日志只记录 query/body 字节数，不主动记录原始查询词。

需要查看每次 HTTP/TLV 请求、连接建立/断开和搜索阶段耗时时，将 `log_level`
临时改为 `debug` 并重启服务。高并发压测建议保持 `info`，避免逐请求日志影响结果。

`Config` 解析规则：

1. 支持空行。
2. 支持以 `#` 开头的整行注释。
3. 支持 `key = value` 两侧空白。
4. 不支持行尾注释。
5. 使用第一个 `=` 分隔 key 和 value。
6. key 或 value 为空会抛异常。
7. 同名 key 重复出现时，后面的配置覆盖前面的配置。
8. `Config::get()` 查询不存在的 key 会抛异常。

在线服务中，端口、线程数、返回数量、摘要长度、前端目录和缓存配置都有默认值；离线数据路径是必需配置。

## 5. 离线建库

### 5.1 输出文件

关键字推荐数据：

```text
data/dict/dict_en.dat
data/index/index_en.dat
data/dict/dict_cn.dat
data/index/index_cn.dat
```

网页搜索数据：

```text
data/index/pages.dat
data/index/offsets.dat
data/index/invert_index.dat
data/index/bm25_doc_stats.dat
```

输出文件格式：

| 文件 | 格式 | 说明 |
| --- | --- | --- |
| `dict_en.dat` | `word frequency` | 英文小写单词词频 |
| `index_en.dat` | `character lineNo...` | 英文字母到英文词典行号的映射 |
| `dict_cn.dat` | `word frequency` | 纯汉字中文词语词频 |
| `index_cn.dat` | `character lineNo...` | 单个 UTF-8 汉字到中文词典行号的映射 |
| `pages.dat` | 连续 `<doc>` 记录 | 去重后的网页库 |
| `offsets.dat` | `docId offset length` | 每篇网页在 `pages.dat` 中的字节范围 |
| `invert_index.dat` | `word df docId tf ...` | BM25 倒排索引，保存文档频率和原始词频 |
| `bm25_doc_stats.dat` | `BM25_STATS_V1 N avgdl` + `docId dl` | BM25 全库及逐文档长度统计 |

词典字符索引里的 `lineNo` 从 `1` 开始。在线加载词典时保留 `dict[0]` 为空记录，使 `lineNo` 可以直接作为数组下标使用。

### 5.2 关键词推荐离线处理

英文词典流程：

```mermaid
flowchart LR
    A[扫描英文语料] --> B[逐行读取]
    B --> C[非字母替换为空格]
    C --> D[字母转小写]
    D --> E[按空白切词]
    E --> F[过滤英文停用词]
    F --> G[统计词频]
    G --> H[写 dict_en.dat]
    H --> I[生成 index_en.dat]
```

中文词典流程：

```mermaid
flowchart LR
    A[扫描中文语料] --> B[读取整篇文件]
    B --> C[Jieba Mix 分词]
    C --> D[过滤中文停用词]
    D --> E[仅保留纯汉字 token]
    E --> F[统计词频]
    F --> G[写 dict_cn.dat]
    G --> H[生成 index_cn.dat]
```

中文索引使用 `TextUtils::split_utf8_characters()` 按 Unicode 码点拆分词语。索引 key 是一个完整 UTF-8 汉字，不能按单字节 `char` 处理。

### 5.3 网页搜索离线处理

`PageProcessor` 负责生成网页库、偏移库、BM25 倒排索引和文档长度统计。

```mermaid
flowchart TD
    A[扫描网页 XML 语料] --> B[tinyxml2 解析 item]
    B --> C[提取 title/link/content]
    C --> D[SimHash 近似去重]
    D --> E[重新连续编号]
    E --> F[写 pages.dat 和 offsets.dat]
    E --> G[Jieba 分词]
    G --> H[过滤停用词和噪声 token]
    H --> I[统计 tf/df/dl/N/avgdl]
    I --> J[写 invert_index.dat]
    I --> K[写 bm25_doc_stats.dat]
```

网页提取规则：

1. 递归收集 XML 中所有 `<item>`。
2. 正文优先读取 `<content>`。
3. `<content>` 为空时读取 `<description>`。
4. 两者都为空时丢弃该 item。
5. `<title>` 和 `<link>` 缺失时保留为空字符串。

SimHash 去重规则：

```text
topN = max(5, min(200, content.size() / 120))
汉明距离 <= 3 视为重复
```

离线阶段不再计算最终相关性分数，而是保存 BM25 的充分统计量：

```text
tf(term, doc) = 词项在文档中的原始出现次数
df(term)      = 包含词项的文档数量
dl(doc)       = 文档过滤后的有效 token 数
N             = 去重后的文档总数
avgdl         = 全库平均文档长度
```

倒排索引格式：

```text
word df docId tf [docId tf]...
```

## 6. 在线服务架构

### 6.1 当前模块关系

```mermaid
classDiagram
    class SearchServer {
        -TcpServer server_
        -CachedSearchService service_
        +start()
    }

    class WebHttpServer {
        -TcpServer server_
        -CachedSearchService service_
        +start()
    }

    class CachedSearchService {
        -KeywordRecommender recommender_
        -WebSearcher searcher_
        -Cache cache_
        -inFlight_
        +suggest(query, lang, topK)
        +search(query, topK)
    }

    class Cache {
        <<interface>>
        +get(key, value)
        +put(key, value, ttl)
        +erase(key)
    }

    class ShardedWTinyLfuCache
    class ShardedLruCache
    class RedisCache
    class TwoLevelCache
    class KeywordRecommender
    class WebSearcher
    class PageLibrary
    class DynamicAbstract

    SearchServer --> CachedSearchService
    WebHttpServer --> CachedSearchService
    CachedSearchService --> KeywordRecommender
    CachedSearchService --> WebSearcher
    CachedSearchService --> Cache
    TwoLevelCache --> ShardedWTinyLfuCache
    TwoLevelCache --> RedisCache
    WebSearcher --> PageLibrary
    WebSearcher --> DynamicAbstract
    WebSearcher --> Cache
```

当前在线服务只有一份 `KeywordRecommender`、一份 `WebSearcher` 和一份缓存对象。TLV 和 HTTP 两个网络入口共享同一个 `CachedSearchService`，因此两种访问方式可以命中同一批缓存。

### 6.2 muduo 线程模型

`search_server` 在同一个进程中创建两个 `muduo::net::TcpServer`：

| 服务 | 默认端口 | 作用 |
| --- | --- | --- |
| `SearchServer` | `8888` | TLV 协议接口 |
| `WebHttpServer` | `18888` | 浏览器静态文件和 HTTP JSON API |

并发访问策略：

1. 词典库、字符索引和倒排索引启动后只读，可以被多个 muduo 工作线程共享。
2. 网页正文不全量加载，`PageLibrary::find()` 每次创建局部 `ifstream` 按需读取，避免共享文件流位置。
3. L1 缓存按 key 哈希分片，每个分片有独立互斥锁。
4. Redis L2 使用有上限的持久连接池，每条 hiredis 连接同一时刻只由一个线程独占使用。
5. `CachedSearchService` 使用 singleflight 合并相同 key 的并发回源。

### 6.3 统一日志

`AppLogger` 在离线和在线进程中创建同一套日志结构：一个彩色控制台 sink、一个
按大小滚动的文件 sink、一个后台线程和有界异步队列。muduo WARN 及以上输出也
桥接到 spdlog，避免长期维护两套日志出口。

在线搜索 DEBUG 日志包含 `tokenize_us`、`recall_us`、`rank_us`、`render_us` 和
`total_us`；HTTP/TLV 日志包含协议、客户端、请求大小和总处理耗时。Redis 故障
只记录首次及每累计 100 次 WARN，避免故障期间刷屏。

## 7. 缓存设计与实现

### 7.1 缓存对象

当前项目缓存四类派生结果：

| 缓存内容 | 生成位置 | key 主要字段 |
| --- | --- | --- |
| 关键词推荐最终 JSON | `CachedSearchService::suggest()` | version、`suggest`、lang、topK、query |
| 网页搜索最终 JSON | `CachedSearchService::search()` | version、`search`、topK、abstractLength、query |
| 文档展示信息 | `WebSearcher::get_document()` | version、`doc`、docId |
| 动态摘要片段 | `WebSearcher::get_abstract()` | version、`abstract`、docId、abstractLength、keywords |

缓存只保存可以从离线数据重新计算出来的派生数据，因此 Redis 不可用或缓存丢失不会影响搜索正确性。

### 7.2 L1 分片完整 W-TinyLFU

实现文件：

```text
include/cache/TinyLfuFrequencySketch.h
src/cache/TinyLfuFrequencySketch.cc
include/cache/ShardedWTinyLfuCache.h
src/cache/ShardedWTinyLfuCache.cc

# 原 LRU 实现作为配置回退和命中率对照保留
include/cache/ShardedLruCache.h
src/cache/ShardedLruCache.cc
```

核心结构：

```text
ShardedWTinyLfuCache
  -> vector<Shard>
      -> mutex
      -> Window LRU                          接收全部新数据
      -> Main Probation                      保存通过准入的候选
      -> Main Protected                      保护重复命中的热点
      -> unordered_map<key, Location>        O(1) 定位节点及所属分段
      -> TinyLfuFrequencySketch
          -> 4-row Count-Min Sketch          4-bit 饱和计数器
          -> Bloom Doorkeeper                过滤首次访问
          -> Frequency Aging                 周期减半并重置 Doorkeeper
```

访问流程：

1. 对 key 做 `std::hash<std::string>`。
2. 根据哈希值选择分片。
3. 只锁定该分片。
4. 每次 `get()`（包括未命中）记录访问频率。
5. `put()` 的新数据先进入 Window LRU。
6. Window 淘汰候选在 Main 未满时直接进入 Main Probation。
7. Main 已满时，候选频率严格高于 Probation 尾部 victim 才允许替换。
8. Probation 再次命中后晋升 Protected；Protected 超限时将尾部降级回 Probation。
9. 第一次访问只进入 Doorkeeper，第二次起才增加 Count-Min Sketch，减少长尾污染。
10. 频率采样达到阈值后将 4-bit 计数器减半并清空 Doorkeeper。
11. 过期项在访问和写入时惰性清理。

频率估计器的空间在构造时按分片容量固定分配。Count-Min Sketch 使用 4 行计数，每个计数器占 4 bit、最大值为 15；Doorkeeper 使用两个 Bloom bit。无论出现多少种长尾 query，频率估计器都不会按 key 扩容。

TTL 语义：

```text
ttlSeconds > 0   到期后失效
ttlSeconds <= 0  永不过期，只受容量淘汰影响
```

### 7.3 Redis L2

实现文件：

```text
include/cache/RedisCache.h
src/cache/RedisCache.cc
```

当前 `RedisCache` 使用 hiredis 同步客户端和惰性持久连接池：

1. 首次需要时通过 `redisConnectWithTimeout()` 建立连接。
2. `redisSetTimeout()` 设置命令读写超时。
3. `SELECT` 切换 DB。
4. `GET` 读取缓存。
5. `SETEX` 或 `SET` 写入缓存。
6. `DEL` 删除缓存。

连接池行为：

1. 每条连接借出期间只属于一个工作线程，避免并发使用 hiredis context。
2. 命令完成后健康连接归还池中，不重复 TCP 握手和 `SELECT`。
3. 池达到上限时只等待 `redis_pool_wait_timeout_ms`，超时后按缓存未命中降级。
4. 命令失败或 context 异常时丢弃连接，后续请求惰性重建。
5. 构造 `RedisCache` 时不强制连接 Redis，Redis 不可用不会阻止服务启动。

Redis 异常处理：

```text
get 失败 -> 返回 false，按缓存未命中处理
put 失败 -> 不影响响应，按采样策略记录 WARN
erase 失败 -> 不影响响应，按采样策略记录 WARN
```

### 7.4 二级缓存

实现文件：

```text
include/cache/TwoLevelCache.h
src/cache/TwoLevelCache.cc
```

读取流程：

```mermaid
flowchart TD
    A[get key] --> B{L1 命中?}
    B -- 是 --> C[返回 L1 value]
    B -- 否 --> D{L2 命中?}
    D -- 否 --> E[返回未命中]
    D -- 是 --> F[回填 L1]
    F --> G[返回 L2 value]
```

写入流程：

```text
put(key, value, ttl) -> 同时写 L1 和 L2
erase(key) -> 同时删 L1 和 L2
```

当同时启用 L1 和 Redis 时，`online_main.cc` 创建：

```text
TwoLevelCache(l1Cache.get(), redisCache.get(), redis_l1_backfill_ttl_seconds)
```

上层业务只依赖 `Cache*`，不需要感知当前使用一级缓存还是二级缓存。

### 7.5 CachedSearchService

实现文件：

```text
include/cache/CachedSearchService.h
src/cache/CachedSearchService.cc
```

职责：

1. 封装 `suggest(query, lang, topK)`。
2. 封装 `search(query, topK)`。
3. 归一化 query 和 lang。
4. 构造业务结果缓存 key。
5. 查询缓存、回源计算、写入缓存。
6. 使用 singleflight 合并相同 key 的并发回源。
7. 对空结果使用短 TTL。
8. 给 TTL 增加随机抖动。
9. 定期打印缓存统计。

通用流程：

```mermaid
flowchart TD
    A[请求进入] --> B[归一化参数]
    B --> C[构造 cache key]
    C --> D{缓存命中?}
    D -- 是 --> E[返回缓存 JSON]
    D -- 否 --> F{是否已有相同 key 在回源?}
    F -- 是 --> G[等待 owner 线程结果]
    F -- 否 --> H[当前线程成为 owner]
    H --> I[再次检查缓存]
    I --> J{命中?}
    J -- 是 --> E
    J -- 否 --> K[调用原业务模块计算]
    K --> L[按结果类型选择 TTL]
    L --> M[增加 TTL 抖动]
    M --> N[写入缓存]
    N --> O[唤醒等待线程]
    O --> P[返回 JSON]
    G --> P
```

统计日志示例：

```text
cache stats total=100 hit_rate=73.00% suggest_hit=30 suggest_miss=10 search_hit=43 search_miss=17 backend_compute=27 cache_put=27 empty_put=2 singleflight_wait=5
```

## 8. 在线关键词推荐算法

`KeywordRecommender` 的实现思路是“字符索引召回 + 编辑距离排序”。

查询流程：

```mermaid
flowchart TD
    A[query] --> B{lang}
    B -->|cn| C[按 UTF-8 汉字拆分]
    B -->|en| D[英文归一化并按字符拆分]
    C --> E[查 cnIndex_]
    D --> F[查 enIndex_]
    E --> G[合并候选词行号]
    F --> G
    G --> H[从词典取 word/frequency]
    H --> I[计算编辑距离]
    I --> J[排序]
    J --> K[返回 JSON]
```

候选词召回：

1. 中文查询使用 `TextUtils::split_utf8_characters()` 拆成完整汉字。
2. 英文查询使用 `TextUtils::normalize_english_line()` 归一化为小写字母。
3. 对每个字符查询字符索引。
4. 合并所有行号并去重。

排序规则：

```text
1. distance 小的优先
2. distance 相同，frequency 大的优先
3. distance 和 frequency 都相同，word 字典序小的优先
```

## 9. 在线网页搜索算法

`WebSearcher` 使用 BM25 排序，并保留忽略 OOV 的 OR 召回。

查询流程：

```mermaid
flowchart TD
    A[query] --> B[Jieba 分词]
    B --> C[过滤停用词和噪声 token]
    C --> D[忽略 OOV]
    D --> G[从倒排索引取 posting list]
    G --> H[忽略 OOV 并合并有效词文档集合]
    H --> I{并集为空?}
    I -- 是 --> R[返回空 results]
    I -- 否 --> J[使用 tf/df/dl/avgdl/N 计算 BM25]
    J --> K[按 score 排序]
    K --> L[获取文档展示信息]
    L --> M[生成或读取动态摘要]
    M --> N[返回 JSON]
```

BM25 公式：

```text
IDF(q) = ln(1 + (N - df(q) + 0.5) / (df(q) + 0.5))

score(q,d) = IDF(q) * tf(q,d) * (k1 + 1)
             / (tf(q,d) + k1 * (1 - b + b * dl(d) / avgdl))
```

当前默认 `k1=1.5`、`b=0.75`，可通过配置修改。平滑后的 IDF 始终为正。

文档召回规则：

1. 查询词不在倒排索引时作为 OOV 跳过，不影响其他有效词。
2. 对所有有效查询词的 posting list 求文档 id 并集，执行 OR 召回。
3. 只有全部查询词均为 OOV 或并集为空时返回空数组。
4. 每个命中词分别贡献 BM25 分数，多词得分累加。

排序规则：

```text
1. score 大的优先
2. score 相同，docId 小的优先
```

## 10. 网页库按需读取

第三期已经把 `PageLibrary` 从“启动时全量加载网页库”改为“启动时只加载偏移库，查询时按需读取网页库”。

当前数据成员：

```cpp
std::string pagesFile_;
std::unordered_map<int, PageOffset> offsets_;
```

启动阶段：

```text
PageLibrary::load(pages, offsets)
  -> 验证 pages.dat 可读
  -> 加载 offsets.dat 到 offsets_
  -> 保存 pages.dat 路径到 pagesFile_
```

查询阶段：

```mermaid
flowchart LR
    A[docId] --> B[查 offsets_]
    B --> C[打开 pages.dat]
    C --> D[seekg offset]
    D --> E[读取 length 字节]
    E --> F[tinyxml2 解析单篇 doc]
    F --> G[返回 Document]
```

这样做的意义：

1. 启动时不再把所有网页正文加载到内存。
2. 词典库、字符索引和倒排索引仍保留全量内存加载，因为它们是高频查询结构。
3. 热点文档由 `WebSearcher` 的文档缓存保存，避免频繁随机读文件。
4. 每次 `find()` 使用局部 `ifstream`，多个线程不会共享文件读取位置。

## 11. 动态摘要

`DynamicAbstract` 根据网页正文和查询关键词生成摘要。它不是固定截取文章开头，而是优先选择关键词附近的正文窗口。

处理流程：

```mermaid
flowchart TD
    A[content] --> B[HTML 实体还原]
    B --> C[删除 HTML 标签]
    C --> D[压缩连续空白]
    D --> E[按 UTF-8 字符拆分]
    E --> F[查找关键词出现位置]
    F --> G[生成候选窗口]
    G --> H[窗口打分]
    H --> I[选择最高分窗口]
    I --> J[用 em 高亮关键词]
```

摘要长度来自配置项 `abstract_length`，当前配置为 `200`。

位置权重：

```text
文章开头 0%  - 20%   1.30
文章中间 20% - 80%   1.00
文章结尾 80% - 100%  1.15
```

摘要高亮格式：

```html
<em>关键词</em>
```

动态摘要缓存 key 包含 `docId`、摘要长度和查询关键词序列，因此不同查询不会错误复用摘要。

## 12. TLV 协议

TLV 外层消息格式：

```text
1 byte type + 4 bytes length + length bytes value
```

字段说明：

| 字段 | 类型 | 说明 |
| --- | --- | --- |
| `type` | `uint8_t` | 业务类型 |
| `length` | `uint32_t` | value 字节长度，网络字节序 |
| `value` | JSON 字符串 | 请求或响应内容 |

消息类型：

```text
1    关键词推荐
2    网页搜索
100  错误响应
```

`ProtocolCodec::try_decode()` 解析规则：

1. 可读字节少于 5 时返回 `false`。
2. 读取 `type` 和网络序 `length`。
3. `length > max_message_size` 时抛异常。
4. 可读字节少于 `5 + length` 时返回 `false`。
5. 完整消息到达后才从 `muduo::net::Buffer` 中取走数据。

关键词推荐请求：

```json
{
  "query": "搜索",
  "lang": "cn",
  "topk": 5
}
```

网页搜索请求：

```json
{
  "query": "汽车 召回",
  "topk": 3
}
```

`topk` 在 TLV 请求中可选；未传时使用配置文件中的 `keyword_topk` 或 `web_topk`。

## 13. HTTP 服务和浏览器前端

`WebHttpServer` 监听 `http_port`，默认 `18888`。它在同一个 `search_server` 进程中完成两件事：

1. 返回静态文件：`/`、`/index.html`、`/styles.css`、`/app.js`、`/favicon.svg`。
2. 提供 HTTP API：`POST /api/suggest`、`POST /api/search`。

HTTP API 不通过 Python 代理，也不转发到 TLV 端口，而是直接调用当前进程中的 `CachedSearchService`。

HTTP/1.1 默认启用 keep-alive，同一连接可以连续处理多条请求；客户端发送
`Connection: close` 时响应后关闭连接。连接建立/断开日志使用 DEBUG 级别，默认
INFO 环境不再为每个连接写日志。HTTP 工作线程默认 8，并通过
`http_max_request_size` 限制单条请求大小。线程数并非越大越好，V3.1 压测表明应在
目标机器上按真实负载比较 2/4/8 线程。

静态文件不存在时返回 HTTP 404，不会将浏览器对可选资源的探测误记为 WARN；
包含 `..` 的路径仍作为非法请求拒绝。

浏览器访问：

```text
http://127.0.0.1:18888
```

HTTP API 示例：

```bash
curl -s -X POST http://127.0.0.1:18888/api/suggest \
  -H 'Content-Type: application/json' \
  --data '{"query":"搜索"}'

curl -s -X POST http://127.0.0.1:18888/api/search \
  -H 'Content-Type: application/json' \
  --data '{"query":"汽车 召回"}'
```

HTTP 接口的返回数量由服务端配置控制，浏览器请求不覆盖 `topK`。

## 14. 响应 JSON

关键词推荐响应：

```json
{
  "type": "keyword",
  "query": "搜索",
  "lang": "cn",
  "results": [
    {
      "word": "探索",
      "distance": 1,
      "frequency": 26
    }
  ]
}
```

网页搜索响应：

```json
{
  "type": "web",
  "query": "汽车 召回",
  "results": [
    {
      "id": 55,
      "title": "两车企同日召回进口牧马人及奥迪Q7",
      "link": "http://auto.people.com.cn/n1/2021/0125/c1005-32010571.html",
      "abstract": "...<em>汽车</em>...<em>召回</em>...",
      "score": 0.6984652717
    }
  ]
}
```

错误响应：

```json
{
  "error": "invalid request",
  "message": "query is empty"
}
```

空结果响应仍保持固定结构：

```json
{
  "type": "web",
  "query": "不存在的查询",
  "results": []
}
```

## 15. 测试方式

### 15.1 编译验证

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON
cmake --build build -j2
ctest --test-dir build --output-on-failure
```

生产目标为 `offline_builder` 和 `search_server`。默认还会构建并执行 3 个 CTest：

```text
cache_policy_test
cache_policy_benchmark
web_searcher_recall_test
```

`web_searcher_recall_test` 使用当前真实离线索引验证部分 OOV 容错、多词 OR 召回和
BM25 正分。如只需构建生产二进制，可传入 `-DBUILD_TESTING=OFF`。

### 15.2 启动 Redis

当前配置中 `redis_cache_enabled=1`。如果本机 Redis 已运行，`search_server` 会使用 Redis 作为 L2 缓存。

如果 Redis 未运行，服务仍会继续工作；Redis 相关操作会按缓存未命中或写入失败处理。为了减少日志和连接失败开销，也可以临时关闭：

```text
redis_cache_enabled=0
```

### 15.3 启动服务

```bash
./bin/search_server
```

启动后默认可访问：

```text
TLV:  127.0.0.1:8888
HTTP: http://127.0.0.1:18888
```

### 15.4 浏览器测试

打开：

```text
http://127.0.0.1:18888
```

页面提供统一搜索框。输入文本时显示推荐词；提交搜索时展示网页结果。

### 15.5 使用 TLV 测试客户端

`tests/tlv_client/` 目录提供 C++ TLV 测试客户端。

编译：

```bash
cd tests/tlv_client
make
```

关键词推荐测试：

```bash
./tlv_client --type keyword --query "搜索" --lang cn --topk 5
```

网页搜索测试：

```bash
./tlv_client --type web --query "汽车 召回" --topk 3
```

直接发送自定义 JSON：

```bash
./tlv_client --type 1 --raw-json '{"query":"搜索","lang":"cn","topk":5}'
```

更详细的参数说明见 `tests/tlv_client/README.md`。缓存算法综合测试位于
`tests/cache_policy/`，HTTP P50/P95/P99 与 QPS 压测位于 `tests/http_load/`；
测试总览见 `tests/README.md`。

### 15.6 HTTP 端到端压测

先启动 `search_server`，再从项目根目录执行：

```bash
python3 tests/http_load/http_load_test.py \
  --mode search \
  --requests 3000 \
  --concurrency 32 \
  --warmup 200 \
  --connection-mode keep-alive
```

扫描污染工作负载：

```bash
python3 tests/http_load/http_load_test.py \
  --mode search \
  --workload scan \
  --scan-namespace v31_scan_round_1 \
  --requests 6200 \
  --concurrency 32 \
  --warmup 0
```

`--connection-mode close` 可对照短连接，`--query <new-key>` 可检查 singleflight 冷 key。
每轮冷缓存测试应使用新 namespace/query，避免命中 Redis 旧结果。完整的 V3/V3.1
对比见 `docs/项目第三期HTTP压测报告-V3.1.md`。

## 16. 当前实现边界

当前第三期已经完成第一阶段到第十阶段缓存改造，仍保留以下边界：

1. L1 已实现完整 W-TinyLFU 组件；当前 Count-Min Sketch 使用较易理解的固定 4 行设计，尚未采用 Caffeine 更复杂的 hill climbing 自适应窗口调节。
2. Redis L2 已使用持久连接池，尚未实现 pipeline、熔断器和分层命中指标。
3. Redis 只作为加速层，不保证强一致性，也不作为唯一数据源。
4. 网页召回已改为忽略 OOV 的 OR 召回；尚未实现可配置的 `minimum_should_match`、topK heap 或 WAND。
5. 网页搜索已使用 BM25；当前只索引正文，尚未实现标题字段加权的 BM25F。
6. 动态摘要做轻量 HTML 清理，不实现完整 HTML 解析器。
7. HTTP 服务已支持 keep-alive 和请求大小限制，但仍是课程项目的最小实现，不作为通用 Web 框架。
8. HTTP 请求体仅支持 `Content-Length`，不支持 chunked request、TLS、完整 URL 解码或通用静态文件服务能力。
9. 可观测性以本地日志为主，尚未接入 Prometheus metrics、request ID 或分布式 tracing。

## 17. 关键源码入口

| 功能 | 文件 |
| --- | --- |
| 离线入口 | `src/offline/offline_main.cc` |
| 在线入口 | `src/online/online_main.cc` |
| 配置读取 | `src/common/Config.cc` |
| 统一日志 | `src/common/Logger.cc` |
| 关键词离线建库 | `src/offline/KeywordProcessor.cc` |
| 网页离线建库 | `src/offline/PageProcessor.cc` |
| 缓存接口 | `include/cache/Cache.h` |
| TinyLFU 频率估计器 | `src/cache/TinyLfuFrequencySketch.cc` |
| L1 分片 W-TinyLFU | `src/cache/ShardedWTinyLfuCache.cc` |
| L1 分片 LRU | `src/cache/ShardedLruCache.cc` |
| Redis L2 | `src/cache/RedisCache.cc` |
| 二级缓存组合 | `src/cache/TwoLevelCache.cc` |
| 缓存业务服务 | `src/cache/CachedSearchService.cc` |
| TLV 协议 | `src/online/ProtocolCodec.cc` |
| TLV 服务 | `src/online/SearchServer.cc` |
| HTTP 服务 | `src/online/WebHttpServer.cc` |
| 在线关键词推荐 | `src/online/KeywordRecommender.cc` |
| 在线网页搜索 | `src/online/WebSearcher.cc` |
| 网页库按需读取 | `src/online/PageLibrary.cc` |
| 动态摘要 | `src/online/DynamicAbstract.cc` |

## 18. 总结

V3.1 当前实现已经形成“离线建库 + 在线查询 + 二级缓存 + 可观测性”的结构：

```text
offline_builder 负责生成基础数据
search_server 负责加载基础数据并响应 TLV/HTTP 查询
CachedSearchService 负责最终结果缓存、singleflight 和统计
WebSearcher 负责网页搜索、按需读取网页库和细粒度缓存
```

第三期已落地的核心能力：

1. L1 分片完整 W-TinyLFU 本地缓存，并保留分片 LRU 回退策略。
2. hiredis Redis L2 缓存和有上限的持久连接池。
3. L1 + L2 二级缓存组合。
4. 关键词推荐和网页搜索最终结果缓存。
5. 文档展示信息和动态摘要片段缓存。
6. singleflight 合并相同 key 的并发回源。
7. 空结果短 TTL 缓存。
8. TTL 随机抖动。
9. 缓存命中率统计日志。
10. 网页库正文按需从文件读取。
11. Window LRU + Main SLRU + Count-Min Sketch + Doorkeeper + Frequency Aging。
12. 忽略 OOV 的 OR 召回和 BM25 在线排序。
13. HTTP/1.1 keep-alive、可调工作线程和静态资源 404。
14. spdlog 异步滚动日志、阶段耗时和慢请求记录。

缓存与连接优化阶段均已完成。后续工程优化可聚焦 Redis 熔断/pipeline、真实查询流量重复压测和基于运行指标的窗口比例调优。
