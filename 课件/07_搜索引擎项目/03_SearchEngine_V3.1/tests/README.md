# 测试目录

不同测试工具按职责独立存放，并使用各自目录中的 Makefile：

| 目录 | 用途 | 是否需要运行中的服务 |
| --- | --- | --- |
| `tlv_client/` | 手工构造并检查 TCP/TLV 请求与响应 | 是，TLV 端口 8888 |
| `cache_policy/` | W-TinyLFU 组件测试、策略对比、多容量和日志回放 | 否 |
| `http_load/` | HTTP 端到端并发压测，输出 P50/P95/P99 和 QPS | 是，HTTP 端口 18888 |
| `web_searcher_recall_test.cc` | 使用真实离线索引验证 OOV 容错、多词 OR 召回和 BM25 正分 | 否 |

完整 CTest 验证：

```bash
cmake -S .. -B /tmp/searchengine_v31_build -DBUILD_TESTING=ON
cmake --build /tmp/searchengine_v31_build -j2
ctest --test-dir /tmp/searchengine_v31_build --output-on-failure
```

快速验证缓存算法：

```bash
cd cache_policy
make test
make benchmark
```

端到端压测前，需要先从项目根目录启动 `./bin/search_server`，再进入 `http_load/` 执行测试。详细参数和测试边界见各子目录 README。
