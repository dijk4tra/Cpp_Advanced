# TLV 服务测试说明

本目录用于测试 `search_server` 的原始 TCP/TLV 服务。TLV 服务默认监听：

```text
127.0.0.1:8888
```

TLV 协议格式：

```text
1 byte type + 4 bytes length + length bytes JSON value
```

字段说明：

| 字段 | 说明 |
| --- | --- |
| `type` | `1` 表示关键字推荐，`2` 表示网页搜索，`100` 表示错误响应 |
| `length` | JSON value 的字节数，4 字节网络序 |
| `value` | UTF-8 JSON 字符串 |

## 1. 启动服务

先从项目根目录启动在线服务：

```bash
cd 课件/07_搜索引擎项目/03_SearchEngine_V3
./bin/search_server
```

保持这个终端运行，再开另一个终端执行下面的测试命令。

## 2. 编译 TLV 测试客户端

方式一：使用 `make`。

```bash
cd 课件/07_搜索引擎项目/03_SearchEngine_V3/tests/tlv_client
make
```

方式二：直接使用 `g++`。

```bash
cd 课件/07_搜索引擎项目/03_SearchEngine_V3/tests/tlv_client
g++ -std=c++17 -Wall -Wextra -O2 tlv_client.cc -o tlv_client
```

生成的可执行程序为：

```text
tests/tlv_client/tlv_client
```

## 3. 测试关键字推荐

带 `topk`，服务端按请求中的 `topk` 返回：

```bash
./tlv_client --type keyword --query "搜索" --lang cn --topk 5
```

不带 `topk`，服务端使用 `conf/config.conf` 中的 `keyword_topk`：

```bash
./tlv_client --type keyword --query "搜索" --lang cn
```

## 4. 测试网页搜索

带 `topk`，服务端按请求中的 `topk` 返回：

```bash
./tlv_client --type web --query "汽车 召回" --topk 3
```

不带 `topk`，服务端使用 `conf/config.conf` 中的 `web_topk`：

```bash
./tlv_client --type web --query "汽车 召回"
```

## 5. 直接发送自定义 JSON

如果需要清晰验证 TLV 服务收到某个原始 JSON 后的行为，可以使用 `--raw-json`。此时客户端不会帮你组装 JSON，只会把参数原样作为 TLV value 发送。

```bash
./tlv_client --type 1 --raw-json '{"query":"搜索","lang":"cn","topk":5}'
```

网页搜索示例：

```bash
./tlv_client --type 2 --raw-json '{"query":"汽车 召回","topk":3}'
```

也可以用这个方式测试异常请求：

```bash
./tlv_client --type 2 --raw-json '{"query":""}'
```

## 6. 输出怎么看

客户端会打印四类信息：

1. `json value`：本次发给服务端的 JSON 内容。
2. `raw request bytes`：真正写入 TCP 连接的 TLV 原始字节，按十六进制和 ASCII 侧栏展示。
3. `raw response bytes`：服务端返回的 TLV 原始字节。
4. `decoded json value`：把响应 TLV 的 value 部分取出来后解析得到的 JSON。

示例结构：

```text
========== TLV Request ==========
server: 127.0.0.1:8888
type: 1 (keyword)
length: 40 bytes
json value:
{
  "lang": "cn",
  "query": "搜索",
  "topk": 5
}

raw request bytes:
000000  01 00 00 00 28 7b 22 6c  61 6e 67 22 ...

========== TLV Response ==========
type: 1 (keyword)
length: 123 bytes
raw response bytes:
000000  01 00 00 00 7b 7b 22 6c  61 6e 67 22 ...

decoded json value:
{
  "lang": "cn",
  "query": "搜索",
  "results": [
    {
      "distance": 1,
      "frequency": 26,
      "word": "探索"
    }
  ],
  "type": "keyword"
}
```

注意：真实输出中的 `length` 和十六进制内容会随 JSON 字段顺序、UTF-8 中文字节数、搜索结果数量变化。

## 7. 参数列表

```text
--host HOST       服务端地址，默认 127.0.0.1
--port PORT       TLV 端口，默认 8888
--type TYPE       keyword、web、1 或 2
--query TEXT      查询文本
--lang LANG       关键字推荐语言提示：cn 或 en
--topk N          TLV 请求中的 topk，可选
--raw-json JSON   直接发送指定 JSON 字符串
--help            查看帮助
```

## 8. 关于命令行直接测试

TLV 外层是二进制协议，不适合直接用 `nc` 手写，因为前 5 个字节包含二进制 type 和网络序 length。可以用 `xxd` 查看字节，但构造和解析都不直观。

因此推荐使用本目录的 `tlv_client`：它本质上也是 Linux 命令行程序，但会把发送和接收的原始 TLV 字节完整打印出来，更适合调试协议。
