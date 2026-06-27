# HTTP 端到端压测

`http_load_test.py` 使用 Python 标准库并发请求真实 `search_server`，测量包含网络解析、L1/L2、singleflight 和搜索回源在内的端到端指标。

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
  --warmup 200
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

## 使用真实查询文件

查询文件每行一个 query，空行和以 `#` 开头的行会被忽略：

```bash
python3 http_load_test.py \
  --query-file sample_queries.txt \
  --requests 10000 \
  --concurrency 64 \
  --warmup 500
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

本脚本的每次请求使用短 HTTP 连接，与当前服务 `Connection: close` 行为一致。它适合测量当前项目实际端到端表现，但不能模拟浏览器连接复用。

项目当前一次完整实测的方法、原始指标与分析见：

```text
../../docs/项目第三期HTTP压测报告.md
```
