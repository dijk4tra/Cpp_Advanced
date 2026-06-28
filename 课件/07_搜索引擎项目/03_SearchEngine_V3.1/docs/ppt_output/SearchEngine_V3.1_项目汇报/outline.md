# SearchEngine V3.1 项目汇报 PPT 大纲（修订版）

> 状态：已按项目实际代码完成内容重构，等待生成修订幻灯片。
>
> 规格：22 页、16:9、中文、高信息密度手绘白板风。内容以 `03_SearchEngine_V3.1` 的实际源码、配置和压测数据为准。

## Slide 1：封面——Pandex SearchEngine

- 保留初版封面的“离线建库—检索核心—在线服务”三段全景形式。
- 离线区只保留真实产物：中英文词典/字符索引、`pages.dat`、`offsets.dat`、BM25 倒排索引与文档统计。
- 中央展示 OR 召回、BM25 排序、TopK 和动态摘要。
- 在线区展示 TLV/HTTP、W-TinyLFU、Redis L2、singleflight、keep-alive 与 spdlog。
- 不显示版本号、汇报人、日期或虚构结果。信息量略低于初版，明显高于当前极简版。
- 代码来源：`src/offline/`、`src/online/`、`src/cache/`、`conf/config.conf`。

## Slide 2：项目目标与核心功能

- 离线构建词典、字符索引、网页库、偏移库和 BM25 索引。
- 在线提供关键词推荐和网页检索，支持 TLV 客户端与 HTTP API。
- 工程能力：W-TinyLFU、Redis L2、singleflight、spdlog、HTTP keep-alive。
- 数据规模：3827 篇去重文档，96379 个倒排词项。

## Slide 3：系统总体架构

- 左侧是离线建库，中央是在线业务，右侧是 TLV/HTTP 入口，底部是缓存层。
- `SearchServer` 与 `WebHttpServer` 共享 `CachedSearchService`。
- `KeywordRecommender` 加载词典/字符索引，`WebSearcher` 加载 BM25 索引并通过 `PageLibrary` 按需读网页。
- 严格来源：`docs/ppt_materials/01_system_architecture.mmd`。

## Slide 4：基本功能演示

- 首页展示服务状态、结果数和请求耗时。
- 输入“搜索”，200 ms 防抖后请求关键词推荐。
- 方向键选择候选，Enter 发起网页搜索。
- 查询“汽车 召回”，展示标题、链接、动态摘要与命中词高亮。

## Slide 5：优秀功能演示

- 查询“中国 qwertyuiopstrictandoovtoken”时忽略 OOV，仍由有效词召回。
- 查询“搜索引擎 股票”时对 posting list 求并集，由 BM25 累加排序。
- 结果、文档和动态摘要均有缓存。

## Slide 6：离线建库流程

- 词典链：语料扫描→清洗/分词→停用词过滤→词频统计→字符索引。
- 网页链：XML 提取→64-bit SimHash 去重→重新连续编号→网页/偏移库→BM25 统计。
- 保持网页库、偏移库、倒排索引和文档统计的 docId 一致。
- 数据：37 个 XML、4365 个原始 item、3827 篇去重文档、`avgdl=613.600`。
- 严格来源：`docs/ppt_materials/02_offline_pipeline.mmd`、`src/offline/PageProcessor.cc`。

## Slide 7：模块类图与依赖方向

- 网络入口只依赖统一业务层，不复制索引和缓存逻辑。
- `Cache` 是抽象接口，L1、Redis 和 `TwoLevelCache` 可组合替换。
- `WebSearcher` 通过 `PageLibrary` 按 docId 读取网页。
- 严格来源：`docs/ppt_materials/03_module_class_diagram.mmd`与各模块头文件。

## Slide 8：离线模块的核心数据结构

- `KeywordProcessor`：长期复用 `cppjieba::Jieba tokenizer_`；中英文停用词分别使用 `set<string>`。
- 词频构建阶段使用有序 `map<string,int>` 统计并稳定输出；英文字符索引为 `map<char,set<int>>`，中文为 `map<string,set<int>>`。
- `PageProcessor`：`vector<Document> documents_`保存提取/去重后文档；`Document` 含 id/link/title/content。
- `map<string, map<int,int>> invertedIndex_` 保存 `word -> docId -> raw tf`；建库中还用 `map<int,map<string,int>>` 累积每文档词频、`map<int,int>` 保存 dl。`Simhasher` 生成 64-bit SimHash，汉明距离不超过 3 视为近似重复。
- 代码来源：`include/offline/KeywordProcessor.h`、`PageProcessor.h` 及对应 `.cc`。

## Slide 9：关键词查询模块的核心数据结构

- `DictEntry { string word; int frequency; }`，中英文词典分别保存为 `vector<DictEntry>`。
- `dict[0]` 故意保留空记录，使离线索引从 1 开始的 lineNo 可直接作为向量下标。
- `CharIndex = unordered_map<string, vector<int>>`，字符映射到包含它的词典行号。
- 查询时先用去重字符召回候选行，再计算 UTF-8 感知的编辑距离。
- 代码来源：`include/online/KeywordRecommender.h`、`src/online/KeywordRecommender.cc`。

## Slide 10：网页搜索模块的核心数据结构

- `unordered_map<string, PostingMap> invertedIndex_`，其中 `PostingMap = unordered_map<int,int>`，对应 `word -> docId -> raw tf`。
- `set<int>` 合并有效词 posting list，自动去重并保持稳定 docId。
- `unordered_map<int,int> documentLengths_` 保存 docId 到 dl；全局保存 N、avgdl、`k1=1.5`、`b=0.75`。
- `PageLibrary` 只把 `unordered_map<int,PageOffset> offsets_` 加载到内存，正文按 offset/length 从 `pages.dat` 按需读取。
- 文档与动态摘要通过 `Cache* detailCache_` 使用独立 TTL 缓存。
- 代码来源：`include/online/WebSearcher.h`、`PageLibrary.h` 及对应 `.cc`。

## Slide 11：缓存模块的核心数据结构

- `Cache` 定义 `get/put/erase`；`TwoLevelCache` 组合 `Cache* l1_`、`Cache* l2_` 与 L1 回填 TTL。
- `ShardedWTinyLfuCache`：`vector<unique_ptr<Shard>>`；每个 Shard 持有 mutex、Window/Probation/Protected 三条 `list<Entry>` 与 `unordered_map<string,Location>`。
- `TinyLfuFrequencySketch`：4 行 4-bit Count-Min Sketch、Bloom Doorkeeper 和周期老化。
- `RedisCache`：`vector<redisContext*> idleConnections_` + mutex + condition_variable，限制连接总数并支持等待/惰性重建。
- `CachedSearchService`：`unordered_map<string, shared_ptr<InFlight>> inFlight_`，每 key 独立 mutex/cv/done/value/error。
- 代码来源：`include/cache/*.h`、`src/cache/*.cc`。

## Slide 12：关键词推荐算法

- 自动判断中文或英文，按 UTF-8 汉字或字母拆分。
- 字符索引先召回候选行号，避免遍历整本词典。
- 候选按编辑距离升序、词频降序、字典序排序。
- 当前更接近拼写纠错，未引入用户行为和热门趋势。

## Slide 13：网页检索——OR 召回与 BM25

- 分词后过滤噪声并忽略 OOV，对有效词 posting list 求并集。
- BM25 累加 IDF、词频饱和与文档长度归一化；`k1=1.5`、`b=0.75`。
- 分数降序，同分按 docId 升序，然后取 TopK。
- 严格来源：`docs/ppt_materials/04_online_search_flow.mmd`、`src/online/WebSearcher.cc`、`conf/config.conf`。

## Slide 14：多级缓存与 singleflight

- 请求依次检查 L1 与 Redis L2，L2 命中时回填 L1。
- 同 key 冷请求仅有 owner 真实回源，waiter 通过 condition_variable 等待结果。
- owner 在回源前二次检查缓存；回源后写 L1+Redis 并 `notify_all`。
- 空结果使用短 TTL，普通结果使用正常 TTL，均加随机抖动。
- 严格来源：`05_cache_request_flow.mmd`、`07_singleflight_sequence.mmd`、`CachedSearchService.cc`。

## Slide 15：W-TinyLFU 专项讲解

- 实际配置：总容量 4096、32 分片、Window 1%、Main Protected 80%、频率采样倍数 10。当 4096 可被 32 整除时，每分片容量 128：Window=1、Main=127、Protected 上限=101、Probation 可用 26，采样周期=1280 次访问。
- 新 key 总是进入 Window MRU；Window 超限时取 Window LRU 作 candidate。
- Main 未满时 candidate 直接进 Probation MRU；Main 已满时与 Probation LRU victim 比较频率。
- 只有 `freq(candidate) > freq(victim)` 才替换；同频率保留 victim，抑制一次性扫描污染。
- Probation 再次命中晋升 Protected；Protected 超限时将其 LRU 降级到 Probation MRU。
- 频率估计：Doorkeeper 记录首次访问，第二次起增加 4 行 4-bit Count-Min Sketch，取四行最小值；达到采样上限后计数减半并清空 Doorkeeper。
- 严格来源：`06_wtinylfu_admission.mmd`、`ShardedWTinyLfuCache.*`、`TinyLfuFrequencySketch.*`、`conf/config.conf`。

## Slide 16：动态摘要

- 本页只讲动态摘要，不展开前端安全标红。
- 清理 HTML/实体/连续空白，按 UTF-8 完整字符处理；非法摘要长度回退为 150 字符。
- 预计算每个关键词的字符序列和全文命中位置；以每个命中位置为锚点，窗口默认从 `pos-50` 开始并限制在正文范围内。
- 窗口分数：`totalHits * positionWeight + coverageBonus + closeBonus`。
- 覆盖关键词每个给 10 分基础分，重复命中给有上限的少量增益；覆盖率最高 +10；至少覆盖 2 个词且首尾命中距离不超过 40 字符时 +5。
- 位置权重：正文前 20% 为 **1.30**，后 20% 为 **1.15**，中间 60% 为 **1.00**。
- 若所有关键词都未命中，返回正文开头一段作为兜底；否则返回最高分窗口并对查询词做 `<em>` 高亮。
- 代码来源：`include/online/DynamicAbstract.h`、`src/online/DynamicAbstract.cc`。

## Slide 17：三个典型 Bug 与解决方案

- 严格 AND + OOV 导致召回丢失→忽略 OOV + OR + BM25。
- 中文编辑距离误按 UTF-8 字节计算→按完整字符执行 DP。
- TCP 回调被误当完整消息→Buffer 累积、半包等待、循环解码 TLV。
- 严格来源：`08`–`10` Bug Mermaid 与对应实现/测试。

## Slide 18：日志与可观测性

- spdlog 异步输出彩色控制台和按大小滚动文件。
- 离线记录提取、去重、建库与 BM25 索引阶段耗时。
- 在线记录慢请求、HTTP/TLV 上下文和 tokenize/recall/rank/render 耗时。
- 逐请求放 DEBUG，启动/统计放 INFO，可恢复异常放 WARN。

## Slide 19：性能优化结果

- 搜索并发 32：QPS `665.80 → 1015.54`，+52.53%。
- 推荐并发 32：QPS `692.06 → 1410.41`，+103.80%。
- 扫描流量：QPS `762.06 → 1516.50`，+99.00%；P95 `81.354 ms → 50.197 ms`，-38.30%。
- 主要收益来自 keep-alive、Redis 连接池和日志降噪；线程数并非越多越好。
- 严格数据源：`benchmark_data.csv`、`docs/项目第三期HTTP压测报告-V3.1.md`。

## Slide 20：Git 与工程管理

- V3 基线→简化 W-TinyLFU→完整 W-TinyLFU→日志系统→V3.1 更新。
- 代码、测试、配置、README 和优化进度同步更新。
- CTest 固化缓存策略和召回语义。
- 结合误用 `git restore .` 的经历，强调小阶段及时提交。

## Slide 21：现有不足与改进路线

- P0：真实查询评测集、分层 metrics、索引原子发布。
- P1：BM25F 标题加权、SimHash 分桶、topK heap/MaxScore/WAND。
- P2：行为推荐、Redis 熔断与异步化。
- P3：多机分片、增量索引和分布式部署。

## Slide 22：项目感悟与总结 / Q&A

- 搜索引擎是数据、算法和系统工程的组合。
- 召回决定候选集边界，排序决定进入后的次序。
- 性能结论需要对照实验，不能仅凭直觉增加线程。
- 缓存、日志、测试、配置和 Git 历史都是交付的一部分。
- 收束关键词：`BM25`、`W-TinyLFU`、`Keep-Alive`。

## 严格内容源映射

| 页码 | 内容源 | 保真要求 |
| ---: | --- | --- |
| 1 | 项目实际模块与初版封面布局参考 | 不虚构文件类型、搜索结果或模块 |
| 3 | `01_system_architecture.mmd` | 节点、层次与依赖方向一致 |
| 6 | `02_offline_pipeline.mmd` | 流程顺序、产物名、统计名一致 |
| 7 | `03_module_class_diagram.mmd` | 类名、实现关系和关键依赖一致 |
| 8–11 | 对应模块 `.h/.cc` | 容器类型、成员语义与操作特征以代码为准 |
| 13 | `04_online_search_flow.mmd`、`WebSearcher.cc` | OOV、OR、BM25、稳定排序不改变 |
| 14 | `05_cache_request_flow.mmd`、`07_singleflight_sequence.mmd` | 命中、回填、二次检查、等待与回源语义不改变 |
| 15 | `06_wtinylfu_admission.mmd`、W-TinyLFU 源码与配置 | 三分段、准入比较、Doorkeeper、Sketch 和 Aging 必须准确 |
| 16 | `DynamicAbstract.h/.cc` | 窗口分数与位置权重 1.30/1.15/1.00 必须准确 |
| 17 | `08`–`10` Bug Mermaid 与实现/测试 | 三类 Bug 的现象、根因、修复与验证不改变 |
| 19 | `benchmark_data.csv`、HTTP 压测报告 | 数值、单位和正负方向不得改变 |
