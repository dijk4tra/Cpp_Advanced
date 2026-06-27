# SearchEngine V2 Web UI

`www/` 是 `search_server` 内置 HTTP 服务直接返回的浏览器前端。页面品牌显示为：

```text
SearchEngine
Pandex
```

## 文件说明

```text
index.html    页面结构，包含搜索框、推荐词下拉框、结果区和状态指标
styles.css    minimalism / swiss 风格样式
app.js        输入推荐、网页搜索、键盘选择和结果渲染逻辑
```

## 交互方式

页面只有一个搜索入口：

1. 用户在搜索框输入文本时，前端防抖请求 `POST /api/suggest`。
2. 推荐词显示在搜索框下方，每项展示推荐词、编辑距离和词频。
3. 点击推荐词会填入搜索框并立即执行网页搜索。
4. 方向键可以选择推荐词，`Enter` 搜索选中词，`Esc` 关闭推荐框。
5. 点击“搜索”或在搜索框直接按 `Enter` 时，前端请求 `POST /api/search`。

前端不再提供 TopK 设置，也不会向后端发送 TopK。推荐词数量和网页搜索结果数量由 `conf/config.conf` 中的 `keyword_topk` 和 `web_topk` 控制。

## HTTP 请求

关键词推荐：

```http
POST /api/suggest
Content-Type: application/json

{"query":"搜索"}
```

网页搜索：

```http
POST /api/search
Content-Type: application/json

{"query":"汽车 召回"}
```

## 运行方式

从项目根目录启动在线服务：

```bash
cd 课件/07_搜索引擎项目/02_SearchEngine_V2
./bin/search_server
```

浏览器访问：

```text
http://127.0.0.1:18888
```

端口说明：

```text
8888   原始 TCP/TLV 服务
18888  浏览器 HTTP 服务
```
