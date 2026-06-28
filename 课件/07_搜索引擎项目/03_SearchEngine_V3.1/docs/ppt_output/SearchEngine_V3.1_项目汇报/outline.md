# SearchEngine V3.1 项目汇报 PPT 大纲（待确认）

> 状态：大纲确认阶段。尚未生成样页、最终幻灯片图片或 PPTX。
>
> 目标：面向课程项目答辩，用约 12～15 分钟说明项目功能、架构、算法、工程优化、典型 Bug、性能数据与后续改进。
>
> 规格：18 页、16:9、中文；文字较多的页面优先采用高分辨率生成并控制单页文字量。

## Slide 1：封面——Pandex SearchEngine

- 离线建库、BM25 检索、关键词推荐与多级缓存。
- 项目名称只标注 `Pandex SearchEngine`，不显示版本号、汇报人和日期。
- 视觉意图：用“离线数据 → 在线检索 → 用户结果”的简化主线建立主题。
- 页面角色：封面。

## Slide 2：项目目标与核心功能

- 离线生成中英文词典、字符索引、网页库、偏移库和 BM25 索引。
- 在线提供关键词推荐和网页检索，同时支持 TLV 客户端与浏览器 HTTP API。
- 工程能力包括 W-TinyLFU、Redis L2、singleflight、spdlog 和 HTTP keep-alive。
- 展示数据规模：3827 篇去重文档、96379 个倒排词项。
- 页面角色：项目概览；使用双链路功能地图。

## Slide 3：系统总体架构

- 左侧展示离线建库，中央展示在线业务层，右侧展示 TLV/HTTP 入口。
- `SearchServer` 与 `WebHttpServer` 共享 `CachedSearchService`。
- 推荐模块加载词典/字符索引，搜索模块加载 BM25 索引并按偏移读取网页。
- 缓存层由 L1 W-TinyLFU、Redis L2 和 singleflight 组成。
- 页面角色：总体架构图。
- 内容来源：`docs/ppt_materials/01_system_architecture.mmd`，节点、依赖方向和层次必须保持一致。

## Slide 4：基本功能演示

- 首页展示服务状态、结果数和请求耗时。
- 输入“搜索”触发 200 ms 防抖的关键词推荐。
- 方向键选择候选，Enter 发起网页搜索。
- 搜索“汽车 召回”，展示标题、链接、动态摘要和关键词标红。
- 页面角色：功能演示；使用浏览器界面占位框和四步演示路径。
- 可选严格素材：后续如能稳定运行服务，再补充首页、推荐下拉框和搜索结果截图；当前不把截图列为生成前置条件。

## Slide 5：优秀功能演示

- 部分 OOV 不再导致整条查询返回空结果。
- 多词采用 OR 召回，由 BM25 累加分数排序。
- 摘要按查询词选择相关窗口并安全标红。
- 结果、文档和摘要均有缓存，重复请求可直接复用。
- 页面角色：功能对比；使用两组查询示例和“正确返回”结果卡片。

## Slide 6：离线建库流程

- 词典链路：语料扫描、清洗分词、停用词过滤、词频统计、字符索引。
- 网页链路：XML 提取、SimHash 去重、连续编号、网页/偏移库、BM25 统计。
- 强调网页库、偏移库、倒排索引和文档统计中的 docId 一致性。
- 展示 37 个 XML、4365 个原始 item、3827 篇去重文档、`avgdl=613.600`。
- 页面角色：离线流程图。
- 内容来源：`docs/ppt_materials/02_offline_pipeline.mmd`，流程顺序和产物名称必须保持一致。

## Slide 7：模块类图与依赖方向

- 网络入口只依赖统一业务服务，不复制索引或缓存逻辑。
- `Cache` 提供抽象接口，L1、Redis 和 TwoLevelCache 可组合替换。
- `WebSearcher` 依赖 `PageLibrary` 按 docId 读取网页。
- 强调“网络层—业务层—检索模块—缓存/存储”的单向依赖。
- 页面角色：简化类图。
- 内容来源：`docs/ppt_materials/03_module_class_diagram.mmd`，类名与关键依赖必须保持一致。

## Slide 8：各模块的核心数据结构

- 推荐：`vector<DictEntry>` 与 `unordered_map<string, vector<int>>`。
- 检索：倒排索引 `word -> docId -> tf`、OR 候选 `set<int>`、偏移表哈希映射。
- 缓存：分片、链表、哈希索引、4-bit Count-Min Sketch 与 Bloom Doorkeeper。
- 并发：Redis 连接池使用互斥锁/条件变量，singleflight 使用每 key 共享状态。
- 页面角色：数据结构矩阵；按“模块—结构—作用”分组，避免复制完整长表。

## Slide 9：关键词推荐算法

- 自动判断中文或英文，并按 UTF-8 汉字或英文字母拆分。
- 字符索引先召回候选行号，避免遍历整本词典。
- 对候选计算编辑距离，再按距离、词频、字典序排序。
- 明确当前能力更接近拼写纠错，尚未引入用户行为和热门趋势。
- 页面角色：算法流程与边界说明。

## Slide 10：网页检索——OR 召回与 BM25

- 查询分词后过滤噪声并忽略 OOV，对有效词 posting list 求并集。
- BM25 综合 IDF、词频饱和和文档长度归一化进行排序。
- 解释 `tf`、`df`、`N`、`dl/avgdl`、`k1=1.5`、`b=0.75`。
- 展示一段精简的 BM25 核心代码或公式，不放完整函数。
- 页面角色：核心算法解释；作为两种视觉风格的候选样页。
- 内容来源：`docs/ppt_materials/04_online_search_flow.mmd`、`src/online/WebSearcher.cc`、`conf/config.conf`。

## Slide 11：多级缓存与 singleflight

- 请求依次检查 L1 和 Redis L2，L2 命中时回填 L1。
- 同 key 冷请求由 singleflight 合并为一次真实回源。
- 空结果使用短 TTL，普通结果使用正常 TTL，并增加随机抖动。
- W-TinyLFU 通过 Window/Main 准入减少扫描污染。
- 页面角色：缓存请求流程；主图使用 L1/L2/singleflight 链路，W-TinyLFU 作为侧边机制图。
- 内容来源：`docs/ppt_materials/05_cache_request_flow.mmd`、`06_wtinylfu_admission.mmd`、`07_singleflight_sequence.mmd`。

## Slide 12：动态摘要与安全标红

- 清理 HTML、实体和连续空白，并按 UTF-8 字符处理。
- 以关键词命中位置构造候选窗口，按覆盖度、紧密度和位置打分。
- 后端仅用 `<em>` 标记命中词，前端先转义其他 HTML 再恢复 `<em>`。
- 强调“相关性体验”和“XSS 边界”同时处理。
- 页面角色：六步处理流水线 + 前后端安全边界。

## Slide 13：三个典型 Bug 与解决方案

- Bug 1：严格 AND 与 OOV 导致召回丢失；修复为忽略 OOV + OR + BM25。
- Bug 2：HTTP/Redis 短连接与逐连接 INFO 日志拖慢端到端性能；修复连接复用和日志分级。
- Bug 3：浏览器自动请求 favicon 被误记为 WARN；补充 SVG favicon，并让静态缺失返回正常 404。
- 每个 Bug 都按照“现象—根因—修复—验证”表达。
- 页面角色：三列案例卡片。
- 内容来源：`docs/ppt_materials/08_bug_and_to_or.mmd` 与项目优化进度记录。

## Slide 14：日志与可观测性

- spdlog 异步输出彩色控制台和按大小滚动文件。
- 离线记录提取、去重、建库、BM25 索引等阶段耗时。
- 在线记录慢请求、HTTP/TLV 上下文和搜索阶段耗时。
- 普通逐请求放 DEBUG，启动和统计放 INFO，可恢复异常采样 WARN。
- 页面角色：日志分层图 + 一条真实格式的阶段耗时日志。

## Slide 15：性能优化结果

- 搜索并发 32：QPS `665.80 → 1015.54`，提升 52.53%。
- 推荐并发 32：QPS `692.06 → 1410.41`，提升 103.80%。
- 扫描流量：QPS 提升 99.00%，P95 从 81.354 ms 降至 50.197 ms。
- 说明 keep-alive、Redis 连接池和日志降噪是主要收益来源；线程数并非越多越好。
- 页面角色：证据页；分组柱状图 + 延迟指标卡 + 测试边界说明。
- 严格数据源：`docs/ppt_materials/benchmark_data.csv` 和 `docs/项目第三期HTTP压测报告-V3.1.md`；所有数值必须原样保留。

## Slide 16：Git 与工程管理

- 展示 V3 基线、简化/完整 W-TinyLFU、日志系统和 V3.1 更新的提交节点。
- 代码、测试、配置、README 和优化进度同步更新。
- 使用 CTest 固化缓存策略和召回语义。
- 结合误用 `git restore .` 的经历，说明小阶段及时提交的重要性。
- 页面角色：Git 时间线 + 工程实践清单。

## Slide 17：现有不足与改进路线

- P0：真实查询评测集、分层 metrics、索引原子发布。
- P1：BM25F 标题加权、SimHash 分桶、topK heap/MaxScore/WAND。
- P2：行为推荐、Redis 熔断和异步化。
- P3：多机分片、增量索引和分布式部署。
- 页面角色：按优先级排列的路线图，同时展示“当前问题—改进方案”。

## Slide 18：项目感悟与总结 / Q&A

- 搜索引擎是数据、算法和系统工程的组合。
- 召回优先于精细排序：未进入候选集的文档无法被 BM25 挽回。
- 性能结论需要对照实验，不能仅凭直觉增加线程。
- 缓存、日志、测试、配置和 Git 历史都是项目交付的一部分。
- 收束关键词：`BM25`、`W-TinyLFU`、`Keep-Alive`。
- 页面角色：总结与 Q&A。

## 素材映射汇总

| 页码 | 素材 | 使用方式 | 保真要求 |
| ---: | --- | --- | --- |
| 3 | `01_system_architecture.mmd` | 总体架构主图 | 保留节点、层次和依赖方向 |
| 6 | `02_offline_pipeline.mmd` | 离线建库主流程 | 保留流程顺序、统计名和产物名 |
| 7 | `03_module_class_diagram.mmd` | 简化类图 | 保留类名、接口实现和关键依赖 |
| 10 | `04_online_search_flow.mmd` | OR + BM25 流程 | 保留 OOV、posting 并集、排序和摘要链路 |
| 11 | `05`～`07` Mermaid | 缓存主图与机制辅图 | 不改变命中、回源、准入和等待语义 |
| 13 | `08_bug_and_to_or.mmd` | Bug 1 前后对照 | 保留 AND/OR 的语义差异 |
| 15 | `benchmark_data.csv` | QPS/延迟图表 | 数值、单位和正负方向不得改变 |

当前没有必须使用的现成位图、照片或机构 Logo。Mermaid 和 CSV 属于严格内容源，后续将先转换为可检查的视觉素材或把其结构化内容直接写入生成提示；不会用生成模型臆造性能数据。
