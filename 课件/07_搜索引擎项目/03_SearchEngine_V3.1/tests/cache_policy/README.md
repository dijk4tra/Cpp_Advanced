# 缓存算法测试与基准

本目录只测试进程内 L1 淘汰策略，不依赖 muduo、Redis、网页库或运行中的搜索服务。

## 文件

| 文件 | 用途 |
| --- | --- |
| `cache_policy_test.cc` | Count-Min Sketch、Doorkeeper、Aging、TTL、删除和并发正确性 |
| `cache_policy_benchmark.cc` | LRU/W-TinyLFU 多场景、多容量、延迟和吞吐对比 |
| `sample_trace.txt` | 查询日志回放格式示例，每行一个 query/key |
| `Makefile` | 独立构建测试与基准 |

## 构建和正确性测试

```bash
make
make test
```

也可以使用项目 CMake/CTest：

```bash
cmake -S ../.. -B /tmp/searchengine_v3_cache_build
cmake --build /tmp/searchengine_v3_cache_build \
  --target cache_policy_test cache_policy_benchmark
ctest --test-dir /tmp/searchengine_v3_cache_build \
  --output-on-failure -R 'cache_policy_(test|benchmark)'
```

## 内置综合基准

```bash
make benchmark
```

默认对容量 `32、100、512、2048` 与分片数 `1、8、32` 的组合各重放 2 次，场景包括：

| 场景 | 验证目标 |
| --- | --- |
| `scan_resistance` | 稳定热点能否抵抗一次性扫描污染 |
| `zipf_1.1` | 接近常见热点长尾分布时的命中率 |
| `uniform` | 缺少明显热点时策略的行为与额外开销 |
| `hot_shift` | 热点集合发生迁移后能否适应新热点 |
| `burst_long_tail` | 热点请求中混入持续一次性长尾 query |
| `cyclic_working_set` | 工作集循环且可能大于缓存容量时的行为 |

输出字段：

| 字段 | 含义 |
| --- | --- |
| `hit%` | L1 命中率 |
| `p50us/p95us/p99us` | 单次 `get + miss 时 put` 的进程内耗时 |
| `QPS` | 带逐请求计时开销的本地操作吞吐 |
| `admit/reject` | W-TinyLFU candidate 准入/拒绝次数 |
| `ages` | Frequency Aging 次数 |

这组延迟和 QPS只衡量缓存数据结构，不包含网络、Redis 和搜索回源。端到端指标应使用 `../http_load/`。

## 参数

```text
--trace FILE          回放日志，每行一个 query/key
--capacities LIST     容量列表，例如 128,512,2048
--shards LIST         分片数列表，例如 1,8,32
--repeats N           使用全新缓存重复回放次数
--requests N          每个内置随机场景的请求数
--seed N              固定随机种子
--csv                 输出 CSV
```

示例：

```bash
./cache_policy_benchmark \
  --capacities 128,512,2048,8192 \
  --shards 1,8,32 \
  --requests 100000 \
  --repeats 5 \
  --csv
```

## 查询日志回放

先将脱敏日志整理成每行一个规范化 query：

```text
汽车 召回
人工智能
汽车 召回
搜索引擎
```

然后执行：

```bash
./cache_policy_benchmark \
  --trace sample_trace.txt \
  --capacities 1024,4096,16384 \
  --shards 1,8,32 \
  --repeats 3
```

必须保留原始时间顺序，否则会破坏热点迁移、突发和局部性特征。日志回放仍只模拟 key 访问，不测量真实搜索计算成本。
