# HTTP 端到端压测

`http_load_test.py` 使用 Python 标准库并发请求真实 `search_server`，测量包含网络解析、L1/L2、singleflight 和搜索回源在内的端到端指标。

脚本默认使用 HTTP/1.1 keep-alive：每个并发 worker 持有并复用一条连接。使用
`--connection-mode close` 可以切回逐请求短连接，量化 TCP 建连和服务端连接管理开销。

## 前置条件

从项目根目录启动服务：

```bash
./bin/search_server
```

检查脚本语法：

```bash
cd tests/http_load
make
```

## 基本压测

```bash
python3 http_load_test.py \
  --mode search \
  --requests 5000 \
  --concurrency 32 \
  --warmup 200 \
  --connection-mode keep-alive
```

关键词推荐：

```bash
python3 http_load_test.py --mode suggest --requests 5000 --concurrency 32
```

输出为 JSON，包含：

- 实际完成请求数和错误数。
- HTTP 状态码分布。
- 总耗时和 QPS。
- 平均延迟、P50、P95、P99 和最大延迟。

任意请求异常或非 2xx 响应都会使脚本返回非 0。

短连接对照：

```bash
python3 http_load_test.py \
  --mode search \
  --requests 5000 \
  --concurrency 32 \
  --warmup 200 \
  --connection-mode close
```

## 使用真实查询文件

查询文件每行一个 query，空行和以 `#` 开头的行会被忽略：

```bash
python3 http_load_test.py \
  --query-file sample_queries.txt \
  --requests 10000 \
  --concurrency 64 \
  --warmup 500
```

## 扫描流量

`scan` 工作负载与 V3 HTTP 压测报告保持相同形状：20 个热点先预热 200 次，
然后交替插入 4000 个仅访问一次的 query 和 2000 次热点访问，共 6200 次。

```bash
python3 http_load_test.py \
  --mode search \
  --workload scan \
  --scan-namespace v31_scan_round_1 \
  --requests 6200 \
  --concurrency 32 \
  --warmup 0 \
  --connection-mode keep-alive
```

`scan-namespace` 会进入查询和缓存 key。测试冷缓存/冷 Redis 路径时，每轮应使用
不同 namespace，避免上轮结果被复用。

### singleflight 冷 key

`--query` 可让全部并发请求复用同一个 query，方便检查冷 key 是否只回源一次：

```bash
python3 http_load_test.py \
  --mode search \
  --query v31_singleflight_cold_key \
  --requests 300 \
  --concurrency 32 \
  --warmup 0
```

## 对比 LRU 与 W-TinyLFU

建议使用完全相同的查询文件、请求数、并发度和机器环境，分别执行：

```text
l1_cache_policy=lru
l1_cache_policy=wtinylfu
```

每次修改配置后重启服务。为避免上一轮 Redis 数据干扰，可以为两组测试使用不同的 `cache_version`，或先关闭 Redis 只比较 L1；不要在共享 Redis 环境中随意清空整个 DB。

建议分别测试：

1. `warmup=0` 的冷缓存阶段。
2. 足够 warmup 后的稳定阶段。
3. Redis 开启和关闭两种状态。
4. 并发度 `1、8、32、64、128`。
5. 多次重复，报告中位数而不是只取最好结果。

结果 JSON 会包含 `connection_mode`，报告中应同时注明 HTTP 工作线程数、Redis
连接池大小和连接模式，避免把不同网络模型的结果直接比较。

项目当前一次完整实测的方法、原始指标与分析见：

```text
../../docs/项目第三期HTTP压测报告.md
```
