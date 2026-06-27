# 测试目录

不同测试工具按职责独立存放，并使用各自目录中的 Makefile：

| 目录 | 用途 | 是否需要运行中的服务 |
| --- | --- | --- |
| `tlv_client/` | 手工构造并检查 TCP/TLV 请求与响应 | 是，TLV 端口 8888 |
| `cache_policy/` | W-TinyLFU 组件测试、策略对比、多容量和日志回放 | 否 |
| `http_load/` | HTTP 端到端并发压测，输出 P50/P95/P99 和 QPS | 是，HTTP 端口 18888 |

快速验证缓存算法：

```bash
cd cache_policy
make test
make benchmark
```

端到端压测前，需要先从项目根目录启动 `./bin/search_server`，再进入 `http_load/` 执行测试。详细参数和测试边界见各子目录 README。
