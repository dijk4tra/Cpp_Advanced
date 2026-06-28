# 搜索引擎 V3.1 PPT 汇报文稿与图表

## 1. 使用说明

本文是 PPT 的“内容底稿”，不是要把所有文字原样放到幻灯片上。建议：

- PPT 页面只保留结论、关键数据和图，每页 3～6 个要点。
- “口头讲稿”用于现场表达，不要整段复制到 PPT。
- 修订版按 15～18 分钟、22 页设计。新增四页分模块数据结构和一页 W-TinyLFU 专题；动态摘要单独成页。
- Mermaid 源文件位于 `docs/ppt_materials/`，可在支持 Mermaid 的编辑器中导出 SVG/PNG。

### 建议时间分配

| 部分 | 页码 | 时间 |
| --- | --- | ---: |
| 背景与架构 | 1～3 | 2 分钟 |
| 功能演示 | 4～5 | 2 分钟 |
| 离线建库、模块与数据结构 | 6～11 | 5 分钟 |
| 算法、缓存与动态摘要 | 12～16 | 4 分钟 |
| Bug、日志与性能数据 | 17～19 | 3 分钟 |
| 工程管理、路线与总结 | 20～22 | 2 分钟 |

---

## 2. 演示前准备

### 2.1 环境检查

```bash
cd 课件/07_搜索引擎项目/03_SearchEngine_V3.1

redis-cli -h 127.0.0.1 -p 6379 ping
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON
cmake --build build -j2
ctest --test-dir build --output-on-failure
```

演示当天建议提前启动并保持一个终端窗口：

```bash
./bin/search_server
```

浏览器打开：

```text
http://127.0.0.1:18888
```

如果希望现场展示请求日志，将 `conf/config.conf` 中的 `log_level=info`
临时改为 `debug`。演示后应改回 `info`，高并发压测不建议开启逐请求 DEBUG。

### 2.2 演示查询准备

| 目标 | 输入 | 应观察到的现象 |
| --- | --- | --- |
| 中文关键词推荐 | `搜索` | 下拉显示候选词、编辑距离、词频 |
| 网页搜索 | `汽车 召回` | 结果按 BM25 排序，摘要中关键词标红 |
| 部分 OOV 容错 | `中国 qwertyuiopstrictandoovtoken` | 忽略 OOV，仍能用“中国”召回 |
| 多词 OR 召回 | `搜索引擎 股票` | 不要求文档同时含有全部词 |
| 英文推荐 | `search` | 自动判断英文并使用英文词典 |

### 2.3 备用 curl 演示

浏览器出现问题时，使用 curl 保证汇报可继续：

```bash
curl -s -X POST http://127.0.0.1:18888/api/suggest \
  -H 'Content-Type: application/json' \
  --data '{"query":"搜索"}'

curl -s -X POST http://127.0.0.1:18888/api/search \
  -H 'Content-Type: application/json' \
  --data '{"query":"汽车 召回"}'
```

---

## 3. PPT 逐页文稿

## 第 1 页：封面

### PPT 放什么

```text
Pandex SearchEngine
离线建库、BM25 检索、关键词推荐与多级缓存
```

### 口头讲稿

“我汇报的项目是 Pandex SearchEngine。它不只是一个可以输入关键词的页面，而是包含
离线建库、在线推荐和搜索、TLV/HTTP 网络服务、两级缓存以及日志与压测的完整小型搜索引擎。”

---

## 第 2 页：项目目标与功能

### PPT 要点

- 离线：从中英文语料和网页 XML 生成词典、索引、网页库。
- 在线：关键词推荐 + 网页检索。
- 网络：TLV 协议客户端 + 浏览器 HTTP API。
- 工程：L1 W-TinyLFU + Redis L2 + singleflight + spdlog。
- 数据规模：3827 篇去重文档，96379 个倒排词项。

### 口头讲稿

“项目有两条核心业务链路。离线阶段负责把原始语料转换为在线可快速查询的数据结构；
在线阶段负责处理推荐和搜索请求。V3.1 的重点不仅是功能正确，还包括相关性、高并发缓存、连接复用和可观测性。”

---

## 第 3 页：总体架构

### 推荐图

`docs/ppt_materials/01_system_architecture.mmd`

### PPT 边栏可放

```text
两个入口：SearchServer / WebHttpServer
一个业务层：CachedSearchService
两个业务模块：KeywordRecommender / WebSearcher
一套缓存接口：Cache
```

### 口头讲稿

“左边是离线建库，中间是在线服务，下方是缓存层。TLV 和 HTTP 入口没有各写一套业务，
而是共享 CachedSearchService。这一层再选择走推荐还是网页检索，并统一处理缓存、singleflight 和统计。”

---

## 第 4 页：基本功能演示

### 演示顺序

1. 打开首页，简要介绍状态区、结果数和耗时。
2. 输入“搜索”，暂停 200 ms，展示推荐下拉框。
3. 说明每个候选同时显示编辑距离和语料词频。
4. 使用方向键选择候选，Enter 执行搜索。
5. 再搜索“汽车 召回”，展示标题、链接、动态摘要和耗时。

### PPT 备用截图

- 首页全景。
- “搜索”的推荐下拉框。
- “汽车 召回”的搜索结果和红色 `<em>` 标注。

### 口头讲稿

“这是用户直接看到的基本功能。输入过程中前端会做 200 ms 防抖，请求 `/api/suggest`；
提交后请求 `/api/search`。前端不经过 Python 代理，而是由 C++ HTTP 服务直接处理。”

---

## 第 5 页：优秀功能演示

### PPT 要点

- 部分 OOV 不会让整条查询失败。
- 多词使用 OR 召回，BM25 负责排序。
- 摘要是查询相关窗口，不是固定截断开头。
- 摘要使用 `<em>` 标红，前端只允许这一种标签。
- 结果、文档和摘要都有缓存。

### 演示

输入：

```text
中国 qwertyuiopstrictandoovtoken
```

然后说：“第二个词不在索引中，但‘中国’仍然可以召回结果。”

再输入：

```text
搜索引擎 股票
```

强调：候选集是两个 posting list 的并集，同时命中更多词的文档会累加更高 BM25 分数。

---

## 第 6 页：离线建库流程

### 推荐图

`docs/ppt_materials/02_offline_pipeline.mmd`

### 实际数据

```text
网页 XML：37 个文件
原始 item：4365
有效文档：4135
SimHash 去重后：3827
BM25 词项：96379
avgdl：613.600
中文词典：17040
英文词典：34705
```

### 口头讲稿

“离线建库分为词典链路和网页链路。网页链路严格按照提取、去重、重新编号、生成网页/偏移库、
生成 BM25 统计的顺序执行，保证四个输出文件的 docId 一致。”

### 可放的代码摘录

```cpp
extract_documents(dir);
deduplicate_documents();
build_pages_and_offsets(pages, offsets);
build_inverted_index(invertIndex, docStats);
```

代码位置：`src/offline/PageProcessor.cc::process()`。

---

## 第 7 页：模块类图

### 推荐图

`docs/ppt_materials/03_module_class_diagram.mmd`

### 讲解顺序

1. `SearchServer` 和 `WebHttpServer` 是网络入口。
2. 两者都依赖 `CachedSearchService`，不直接持有巨大索引副本。
3. `Cache` 是抽象接口，L1、Redis 和 TwoLevelCache 可互换。
4. `WebSearcher` 通过 `PageLibrary` 按需读取文档。

### 口头讲稿

“这张图重点不是类的数量，而是依赖方向。网络层不关心具体缓存是 LRU、W-TinyLFU 还是 Redis，
业务层也不关心请求来自 TLV 还是 HTTP。这种分层让后续优化可以局部进行。”

---

## 第 8 页：离线模块的核心数据结构

### KeywordProcessor

- `cppjieba::Jieba tokenizer_`：整个建库对象长期复用，避免重复加载分词词典。
- `set<string> enStopWords_ / cnStopWords_`：停用词去重与查找。
- 词频构建阶段使用 `map<string,int>` 统计，便于稳定有序输出。
- 英文字符索引使用 `map<char,set<int>>`，中文使用 `map<string,set<int>>`，记录 `character -> lineNo...`；lineNo 从 1 开始并对应词典物理行。

### PageProcessor

- `vector<Document> documents_`：保存提取并最终去重的文档；`Document` 含 id/link/title/content。
- `simhash::Simhasher hasher_`：生成 64-bit SimHash，当前逐一比较并以汉明距离 ≤ 3 判定近似重复。
- `map<string, map<int,int>> invertedIndex_`：`word -> docId -> raw tf`；建库阶段还使用 `map<int,map<string,int>> docTermCount` 和 `map<int,int> docTotalWords` 累积每文档词频与 dl。

### 口头讲稿

“离线建库的容器偏向确定性和稳定输出。KeywordProcessor 复用分词器并分开管理中英文停用词；
PageProcessor 在内存中保存去重后文档，再由同一组 docId 生成网页库、偏移库和倒排索引。”

---

## 第 9 页：关键词查询模块的核心数据结构

### PPT 要点

- `DictEntry { string word; int frequency; }`。
- `vector<DictEntry> cnDict_ / enDict_`；`dict[0]` 故意留空，使从 1 开始的 lineNo 可直接作为下标。
- `CharIndex = unordered_map<string, vector<int>>`，保存“字符→包含它的词典行号”。
- 查询字符先去重后召回候选行，再按 UTF-8 字符计算编辑距离。

### 口头讲稿

“这里的关键约定是行号。离线索引从 1 开始，在线词典向量就保留空的 0 号位，从而避免每次转换。
CharIndex 先将整本词典缩小为候选行集，这一步决定推荐查询的基本效率。”

---

## 第 10 页：网页搜索模块的核心数据结构

### PPT 要点

- `unordered_map<string, PostingMap> invertedIndex_`，`PostingMap = unordered_map<int,int>`，即 `word -> docId -> raw tf`。
- `set<int>` 作为 OR 候选集，合并 posting 时自动去重并稳定 docId。
- `unordered_map<int,int> documentLengths_` 保存 dl；全局保存 N、avgdl、k1 和 b。
- `PageLibrary::offsets_ = unordered_map<int,PageOffset>`，启动时只加载偏移，正文按 offset/length 从 `pages.dat` 按需读取。
- `Cache* detailCache_` 对文档与动态摘要分别使用独立 TTL。

### 口头讲稿

“网页搜索的数据结构分成召回、排序和展示三部分。倒排索引和 set 生成候选集，文档长度表参与 BM25，
PageLibrary 则避免启动时把所有正文加载进内存。”

---

## 第 11 页：缓存模块的核心数据结构

### PPT 要点

- `Cache` 抽象接口；`TwoLevelCache` 组合 `Cache* l1_ / l2_` 与 L1 回填 TTL。
- `ShardedWTinyLfuCache`：`vector<unique_ptr<Shard>>`；Shard 内含 mutex、Window/Probation/Protected 三条 `list<Entry>` 与 `unordered_map<string,Location>`。
- `TinyLfuFrequencySketch`：4 行 4-bit Count-Min Sketch + Bloom Doorkeeper + Frequency Aging。
- `RedisCache`：`vector<redisContext*> idleConnections_` + mutex + condition_variable + `totalConnections_`。
- `CachedSearchService`：`unordered_map<string, shared_ptr<InFlight>>`；每个 InFlight 含 mutex/cv/done/value/error。

### 口头讲稿

“缓存模块不是一个容器，而是几种并发状态的组合。L1 用分片锁和三段链表管理本地热数据；Redis 连接池保证同步连接独占借出；
singleflight 使用每 key 共享状态合并重复回源。”

---

## 第 12 页：关键词推荐算法

### PPT 流程

```text
查询词
 -> 判断中文/英文
 -> 拆分 UTF-8 汉字或英文字母
 -> 字符索引召回候选词行号
 -> 计算编辑距离
 -> distance 升序、frequency 降序、word 字典序
 -> TopK JSON
```

### 算法理解

- 编辑距离衡量从 query 到候选词所需的最少插入、删除、替换次数。
- 中文不能按 `char` 字节计算，必须先拆成 UTF-8 码点。
- 字符索引先缩小候选集，避免对整本词典计算距离。
- 词频是编辑距离相同时的第二排序信号。

### 可主动说明的边界

当前它更像“拼写纠错”，还不是基于用户行为的热门前缀补全。

---

## 第 13 页：网页检索——OR 召回 + BM25

### 推荐图

`docs/ppt_materials/04_online_search_flow.mmd`

### BM25 公式

```text
IDF(q) = ln(1 + (N - df(q) + 0.5) / (df(q) + 0.5))

score(q,d) = IDF(q) * tf(q,d) * (k1 + 1)
             / (tf(q,d) + k1 * (1 - b + b * dl(d) / avgdl))
```

### 必须讲清的参数

- `tf`：词在当前文档中出现次数。
- `df`：包含该词的文档数。
- `N`：文档总数。
- `dl / avgdl`：当前文档长度相对于全库平均长度。
- `k1=1.5`：控制词频饱和，避免同一词重复出现无限增分。
- `b=0.75`：控制长文档归一化强度。

### 实际代码摘录

```cpp
for (const auto& term : queryTerms) {
    score += bm25_term_score(term, docId);
}

const double idf = std::log(1.0 + (n - df + 0.5) / (df + 0.5));
const double norm = 1.0 - bm25B_ + bm25B_ * dl / averageDocumentLength_;
return idf * tf * (bm25K1_ + 1.0) / (tf + bm25K1_ * norm);
```

### 口头讲稿

“OR 召回解决‘能不能进候选集’，BM25 解决‘候选文档怎样排序’。召回要尽量不漏，排序再通过
词的稀有度、词频饱和和文档长度归一化区分相关性。”

---

## 第 14 页：多级缓存与 singleflight

### 推荐图

- 请求缓存流程：`docs/ppt_materials/05_cache_request_flow.mmd`
- W-TinyLFU 准入：`docs/ppt_materials/06_wtinylfu_admission.mmd`
- singleflight 时序：`docs/ppt_materials/07_singleflight_sequence.mmd`

本页主图放两级缓存与 singleflight 请求链。W-TinyLFU 准入与频率估计放到第 15 页专门讲解。

### 数据结构亮点

```text
L1 Shard
  mutex
  Window LRU
  Main Probation
  Main Protected
  unordered_map<key, Location>
  TinyLfuFrequencySketch
```

### 实际 singleflight 逻辑摘录

```cpp
std::lock_guard<std::mutex> lock(inFlightMutex_);
auto it = inFlight_.find(key);
if (it == inFlight_.end()) {
    state = std::make_shared<InFlight>();
    inFlight_[key] = state;
    owner = true;
} else {
    state = it->second;
}
```

### 口头讲稿

“W-TinyLFU 的重点是不让只访问一次的扫描 key 把真正热点挤出缓存。singleflight 则解决缓存击穿：
当多个请求同时访问一个冷 key，只有 owner 执行搜索，其他线程等待并复用结果。”

---

## 第 15 页：W-TinyLFU 专项讲解

### 实际配置

```text
l1_cache_capacity = 4096
l1_cache_shards = 32
l1_wtinylfu_window_percent = 1
l1_wtinylfu_protected_percent = 80
l1_wtinylfu_frequency_sample_multiplier = 10
```

因为 4096 可被 32 整除，当前每分片容量为 128。按构造函数中的整数容量计算：Window 至少保留 1 个位置，因此 Window=1，Main=127，Protected 上限=`127*80/100=101`，Probation 可用 26，频率采样周期=`128*10=1280` 次访问。

### 三分段与准入

- 所有新 key 先进入 Window MRU，Window 超容量时取其 LRU 作 candidate。
- Main 分成 Probation 和 Protected；Probation 再次命中后晋升 Protected。
- Protected 超容量时，将 Protected LRU 降级到 Probation MRU，而不是直接淘汰。
- Main 已满时，candidate 与 Probation LRU victim 比较近似频率。只有 `freq(candidate) > freq(victim)` 才替换；同频率保留 victim。

### 频率估计

- Doorkeeper 用两个 Bloom bit 记录本采样周期的首次访问，首次不写入 Sketch。
- 从第二次访问起，在 4 行 Count-Min Sketch 各增加一个 4-bit 饱和计数器，最大值 15。
- `estimate()` 取四行最小值，再根据 Doorkeeper 是否存在加 1，估算范围 0～16。
- 访问数达到 `shardCapacity * 10` 后老化：所有 4-bit 计数减半，清空 Doorkeeper。

### 口头讲稿

“W-TinyLFU 同时利用时间局部性和频率信息。Window 让新热点有短暂观察机会，Main 保留稳定热点，TinyLFU 则把一次性扫描拦在 Main 外面。
同频率不替换是一个刻意的保守策略：没有足够证据时，优先保留已经证明价值的 victim。”

---

## 第 16 页：动态摘要

### PPT 要点

1. 清理 HTML 标签、实体和连续空白，按 UTF-8 字符分割。
2. 非法摘要长度回退为 150 字符；所有关键词都未命中时，返回正文开头作为兜底。
3. 预计算每个关键词的全文命中位置，以每个命中位置为锚点构造候选窗口。
4. 窗口默认从 `pos-50` 开始，再按摘要长度与正文范围修正 begin/end。
5. 窗口分数为 `totalHits * positionWeight + coverageBonus + closeBonus`。
6. 位置权重：正文前 20% 为 **1.30**，后 20% 为 **1.15**，中间 60% 为 **1.00**。
7. 覆盖率最高增加 10 分；至少命中 2 个关键词且首尾距离 ≤ 40 字符时增加 5 分紧密度奖励。
8. 返回最高分窗口，最后对查询词做 `<em>` 高亮；本页不展开前端安全处理。

### 实际代码摘录

```cpp
double score = totalHits * position_weight(pos, chars.size())
             + coverageBonus
             + closeBonus;

// position_weight(pos, total)
// ratio <= 0.20 -> 1.30
// ratio >= 0.80 -> 1.15
// otherwise     -> 1.00
```

### 口头讲稿

“动态摘要的关键是‘针对本次查询选窗口’，而不是从每篇文章头部固定截取。代码会对每个命中位置构造窗口，综合命中、覆盖、紧密度和位置打分。
其中开头的位置权重 1.30，结尾 1.15，中间 1.00，体现了网页开头和结尾通常承载摘要或总结信息的先验。”

---

## 第 17 页：典型 Bug 与解决方案

### Bug 1：严格 AND + OOV 导致召回丢失

| 项目 | 内容 |
| --- | --- |
| 现象 | `中国 + OOV` 直接返回空；多词只保留同时命中全部词的文档 |
| 根因 | 任一词未登录就返回空 query vector，posting list 反复求交集 |
| 修复 | 忽略 OOV，对有效词 posting list 求并集，后续使用 BM25 排序 |
| 回归 | `web_searcher_recall_test`：单词 + OOV 结果一致，多词结果等于单词结果并集 |

推荐图：`docs/ppt_materials/08_bug_and_to_or.mmd`

### Bug 2：中文编辑距离按字节计算，推荐结果排序异常

| 项目 | 内容 |
| --- | --- |
| 现象 | 中文查询的编辑距离明显偏大；两个只相差一个汉字的词，可能被判断为相差多个字符，推荐排序不符合直觉 |
| 根因 | UTF-8 是变长编码，一个常用汉字通常占 3 个字节；如果直接用 `std::string::size()` 和 `operator[]` 做 DP，算法计算的是“字节距离”而不是“字符距离” |
| 修复 | 中文先通过 `split_utf8_characters()` 拆成完整码点字符串，编辑距离 DP 改为在 `vector<string>` 上比较；英文仍按单字母处理 |
| 验证 | 使用“搜索/搜素”“中国/中囯”等单字符插入、删除、替换用例，修复后距离均按字符数变化；英文推荐结果保持不变 |

可展示的实际修复代码：

```cpp
std::vector<std::string> split_word(const std::string& word,
                                    const std::string& lang)
{
    if (lang == "en") {
        std::vector<std::string> result;
        for (char ch : word) {
            result.emplace_back(1, ch);
        }
        return result;
    }
    return TextUtils::split_utf8_characters(word);
}
```

代码位置：`src/online/KeywordRecommender.cc::split_word()`。

推荐图：`docs/ppt_materials/09_bug_utf8_edit_distance.mmd`。

### Bug 3：把 TCP 回调当成完整消息，TLV 出现半包和粘包错误

| 项目 | 内容 |
| --- | --- |
| 现象 | 请求偶发 JSON 解析失败；一次发送多条 TLV 时只处理第一条；报文被拆成多次到达时可能提前读取不完整 body |
| 根因 | TCP 只提供连续字节流，不保留应用层消息边界；一次 `on_message` 既可能只有半条报文，也可能粘连多条报文。另外 TLV 的 4 字节长度从 `header+1` 开始，直接强转还存在未对齐访问和字节序问题 |
| 修复 | 使用 muduo `Buffer` 累积数据；先 `peek()` 检查 5 字节头，再用 `memcpy + ntohl` 安全读取网络序长度；body 不完整时不消费缓冲区；上层用 `while` 循环拆出全部完整帧，并限制最大报文长度 |
| 验证 | 将同一报文按“2 字节头/剩余头/body”分段送入时，只在完整后解码；把两条 TLV 拼接输入时可连续解出两条；超长 length 和非法 type 被拒绝 |

可展示的实际修复代码：

```cpp
if (buffer->readableBytes() < 5) {
    return false;                       // 半个头部：保留数据继续等待
}

uint32_t networkLength = 0;
std::memcpy(&networkLength, buffer->peek() + 1, sizeof(networkLength));
const uint32_t length = ntohl(networkLength);

if (length > maxMessageSize) {
    throw std::runtime_error("message is too large");
}
if (buffer->readableBytes() < 5 + length) {
    return false;                       // body 未收齐：不能移动读指针
}
```

上层处理粘包：

```cpp
while (ProtocolCodec::try_decode(buffer, request, maxMessageSize_)) {
    uint8_t responseType = request.type;
    std::string response = handle_request(
        request.type, request.value, responseType);
    conn->send(ProtocolCodec::encode(responseType, response));
}
```

代码位置：`src/online/ProtocolCodec.cc::try_decode()`、
`src/online/SearchServer.cc::on_message()`。

推荐图：`docs/ppt_materials/10_bug_tlv_stream_framing.mmd`。

### 这一页的口头表达

“我不只列了 Bug 名称，而是保留了现象、根因、修复和验证四个环节。第一个是检索召回语义，
第二个是 UTF-8 字符边界，第三个是 TCP 字节流的消息边界。它们分别对应搜索算法、文本处理和网络协议三个层次。”

---

## 第 18 页：工程亮点——日志与可观测性

### PPT 要点

- spdlog 异步 logger：彩色控制台 + 按大小滚动文件。
- 离线：词典、提取、去重、网页库、BM25 索引阶段耗时。
- 在线：HTTP/TLV 耗时、慢请求、搜索内部 tokenize/recall/rank/render 耗时。
- Redis 故障：首次及每 100 次采样 WARN，避免宕机时刷屏。
- INFO 不记普通逐请求，DEBUG 才记，降低压测干扰。

### 日志示例

```text
2026-06-28 17:35:20.805 [info] [thread 12940]
HTTP server listening address=0.0.0.0:18888 threads=8 keep_alive=true

web search stages query_bytes=... terms=... candidates=...
tokenize_us=... recall_us=... rank_us=... render_us=... total_us=...
```

### 口头讲稿

“日志的价值不是越多越好。我把启动配置和周期统计放在 INFO，普通请求放在 DEBUG，
只让慢请求和可恢复异常进 WARN。这样既可以排障，也不会让日志本身成为瓶颈。”

---

## 第 19 页：性能优化结果

### 推荐表格

| 场景 | V3 | V3.1 | 改善 |
| --- | ---: | ---: | ---: |
| 搜索 QPS，并发 32 | 665.80 | 1015.54 | +52.53% |
| 推荐 QPS，并发 32 | 692.06 | 1410.41 | +103.80% |
| 扫描流量 QPS | 762.06 | 1516.50 | +99.00% |
| 扫描 P95 | 81.354 ms | 50.197 ms | -38.30% |

### 必须补充的严谨性

- V3 是单轮数据，V3.1 是 3 轮中位数。
- 压测端与服务端在同一台虚拟机，Python GIL 和调度开销也进入结果。
- 收益主要来自 keep-alive、关闭逐连接 INFO 和 Redis 连接池。
- 工作线程数不是越多越好，本机热点负载中 2 线程并不慢于 4/8。

### 口头讲稿

“我没有只展示最好的一次，V3.1 每个条件都运行三轮并取中位数。同时我也通过 keep-alive/close
和 2/4/8 线程对照修正了原来‘两线程就是主要瓶颈’的过度归因。”

---

## 第 20 页：Git 与工程管理

### PPT 要点

- 按阶段保留可回溯节点：V3 基线、简化 W-TinyLFU、完整 W-TinyLFU、日志系统、V3.1 更新。
- 代码、测试、配置、README 和优化进度同步更新。
- 使用独立 CMake 构建目录，避免构建产物污染源码。
- 使用 CTest 固化缓存策略和召回语义。
- 使用 `git diff` / `git status` 复核变更，重要阶段及时提交。

### 可展示的实际历史

```text
384ff5e SearchEngine_V3/0626_23:23
94c46ed Simplified_W-TinyLFU/0627_14:00
dbc46dd W-TinyLFU/0627_20:13
ab5dc6b added_log_system/0627_23:56
9f5b928 update/0628_17:35
```

### 可以坦诚说的经验

“我也遇到过误用 `git restore .` 导致未提交修改丢失的情况。这让我意识到 Git 不只是最后交作业时提交一次，
而应该将可验证的小阶段及时 commit。”

---

## 第 21 页：现有不足与改进方案

### PPT 表格

| 现有不足 | 影响 | 改进方案 |
| --- | --- | --- |
| SimHash 去重两两比较，O(n²) | 语料扩大后离线时间快速上升 | 先做内容 hash 精确去重，再用 SimHash 分桶只比较邻近指纹 |
| BM25 只索引正文 | 标题中的强信号没有利用 | 升级 BM25F，对 title/content 分字段加权 |
| OR 召回候选集可能过大 | 高频短词会增加打分开销 | `minimum_should_match`、topK heap、MaxScore/WAND |
| 推荐主要依赖编辑距离 | 无法很好表达热门趋势和前缀意图 | 分离前缀补全与拼写纠错，加入点击/查询日志行为信号 |
| L1 容量按 key 数而非字节 | 大 JSON 可以占用过多内存 | 增加字节容量上限和 value size 统计 |
| 同步 Redis/文件 I/O 占用 muduo 工作线程 | 慢 I/O 可拉高尾延迟 | 专用业务线程池、异步 I/O 或异步 Redis |
| Redis 无熔断与分层 metrics | 持续故障会反复建连，L1/L2 命中不可分 | 熔断/半开恢复 + Prometheus 指标 |
| 索引发布不是原子切换 | 重建期间直接覆盖有风险 | 版本目录 + 临时文件 + rename 原子切换 |

### 优先级表达

```text
P0：真实查询评测集 + 分层 metrics + 索引原子发布
P1：标题 BM25F + 去重分桶 + topK heap
P2：行为推荐 + Redis 熔断/异步化
P3：多机分片、增量索引和分布式部署
```

---

## 第 22 页：项目感悟与总结

### PPT 要点

- 搜索引擎是“数据、算法、系统”的组合，不是只有一个排序公式。
- 召回问题往往比排序公式更优先：文档没有进候选集，再好的 BM25 也无法挽回。
- 性能优化必须通过对照实验验证，不能只靠直觉增加线程。
- 缓存不只是 map，还需要准入、淘汰、过期、击穿防护和故障降级。
- 日志、测试、配置和 Git 历史是项目的一部分，不是代码写完后的附件。

### 收尾讲稿

“这个项目让我对搜索引擎的理解从‘分词加一个排序公式’变成了一条完整链路：
离线数据是否正确、召回是否丢文档、排序是否合理、缓存是否抗击穿、网络和日志是否成为瓶颈，
都会影响最后的用户体验。”

---

## 4. 备用页：答辩可能追问

### Q1：为什么不把倒排索引全部放进 Redis/数据库？

答：当前索引规模可以在启动时加载到内存，词项查询是高频路径。如果每次召回都走数据库或 Redis，
会把本地哈希查找变成网络往返。当前 Redis 更适合缓存可重建的 JSON 结果和文档/摘要。

### Q2：为什么 OR 召回不会让不相关文档排到前面？

答：OR 只是放宽候选集。文档命中的词越多、词越稀有、词频和文档长度越合理，BM25 累加分越高。
当候选规模扩大到影响性能时，再加 `minimum_should_match` 或 WAND，而不是先用严格 AND 牺牲召回。

### Q3：W-TinyLFU 比 LRU 好在哪里？

答：LRU 只看最近访问，一次大扫描会把大量只访问一次的 key 放入缓存并挤掉热点。
W-TinyLFU 用 Window 接纳新数据，再用估算频率决定是否进入 Main，因此抗扫描污染更强。

### Q4：singleflight 和互斥锁有什么不同？

答：一把全局互斥锁会让所有 key 串行。singleflight 只合并“同一 key”的回源，不同 key 仍可并发计算；
等待线程使用每 key 状态中的 condition variable，不会忙等。

### Q5：为什么 Redis 不可用时服务还能工作？

答：Redis 只保存可从离线数据重建的派生数据，不是唯一数据源。`get` 失败被视为 miss，
`put/erase` 失败不影响业务响应，因此可以降级到 L1 和原始搜索流程。

### Q6：为什么不能只用 QPS 评价检索优化？

答：QPS 只衡量吞吐。搜索系统还要看空结果率、Recall@K、NDCG@K、P95/P99 延迟、缓存命中率和回源次数。
一个 QPS 很高但经常返回空结果的搜索引擎没有业务价值。

---

## 5. 图表索引

| 文件 | 用途 | 建议 PPT 页 |
| --- | --- | ---: |
| `01_system_architecture.mmd` | 总体架构 | 3 |
| `02_offline_pipeline.mmd` | 离线建库流程 | 6 |
| `03_module_class_diagram.mmd` | 主要类与依赖 | 7 |
| `04_online_search_flow.mmd` | OR + BM25 + 按需读取 + 摘要 | 10 |
| `05_cache_request_flow.mmd` | L1/L2/singleflight 请求链 | 11 |
| `06_wtinylfu_admission.mmd` | W-TinyLFU 准入与淘汰 | 备用 |
| `07_singleflight_sequence.mmd` | 同 key 冷请求时序 | 备用 |
| `08_bug_and_to_or.mmd` | 严格 AND 修复对比 | 13 |
| `09_bug_utf8_edit_distance.mmd` | UTF-8 字节距离到字符距离 | 13 |
| `10_bug_tlv_stream_framing.mmd` | TLV 半包、粘包与安全解码 | 13 |
| `benchmark_data.csv` | V3/V3.1 QPS 和延迟作图数据 | 15 |

### Mermaid 导出建议

可使用 Mermaid Live Editor、VS Code Mermaid 插件或 Mermaid CLI 导出 SVG。建议优先使用 SVG，
放入 PPT 后放大不会变糊。导出前可根据 PPT 主题统一节点颜色，但不要删除图中的关键数据流。

---

## 6. PPT 排版与截图建议

- 主色使用 1 种深色 + 1 种高亮色，与前端的黑/黄视觉保持一致。
- 架构图和流程图每页只强调一条主线，不要把 8 张 Mermaid 图都放进主汇报。
- 代码截图保留函数名和 8～15 行核心逻辑，删除 include 和大段注释。
- Bug 页使用“现象—根因—修复—验证”四列表，比只放一张代码截图更有说服力。
- 性能页从 `benchmark_data.csv` 生成两张图：QPS 用分组柱状图，P50/P95/P99 用延迟柱状图。
- 截图前清空浏览器无关标签页，缩放到 100%，确保查询词、耗时、结果数和标红摘要同时可见。
- 主 PPT 最后放一页“谢谢 / Q&A”，上面只保留三个关键词：`BM25`、`W-TinyLFU`、`Keep-Alive`。
