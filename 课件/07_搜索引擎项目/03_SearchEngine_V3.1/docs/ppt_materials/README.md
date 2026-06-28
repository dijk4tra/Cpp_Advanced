# PPT Mermaid 图表源文件

该目录保存搜索引擎 V3.1 汇报使用的 Mermaid 源图。建议导出为 SVG 后插入 PPT。

| 文件 | 内容 |
| --- | --- |
| `01_system_architecture.mmd` | 离线、在线和缓存总体架构 |
| `02_offline_pipeline.mmd` | 词典与网页离线建库流程 |
| `03_module_class_diagram.mmd` | 主要类与依赖关系 |
| `04_online_search_flow.mmd` | OR 召回、BM25、按需读取和摘要 |
| `05_cache_request_flow.mmd` | L1/L2/singleflight 缓存链路 |
| `06_wtinylfu_admission.mmd` | W-TinyLFU 分段和准入 |
| `07_singleflight_sequence.mmd` | 同 key 并发冷请求时序 |
| `08_bug_and_to_or.mmd` | 严格 AND 到忽略 OOV + OR 的修复 |
| `benchmark_data.csv` | 可直接导入 PPT/Excel 的性能对比数据 |

完整逐页文稿见 `../项目第三期PPT汇报文稿与图表.md`。
