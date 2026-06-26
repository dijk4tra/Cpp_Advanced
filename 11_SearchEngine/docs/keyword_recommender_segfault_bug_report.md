# KeywordRecommender 关键字推荐段错误 Bug 报告

## 摘要

启动 `./bin/search_server` 后，在浏览器中测试关键字推荐功能时，服务端出现：

```text
段错误 (核心已转储)
```

问题定位在 `11_SearchEngine/src/online/KeywordRecommender.cc` 的编辑距离计算逻辑中。根因是 `KeywordRecommender::edit_distance` 的内层 `for` 循环条件写错，导致循环变量 `j` 不受 `right.size()` 限制，最终越界访问 `std::vector`，触发段错误。

## 影响范围

- 影响功能：浏览器关键字推荐接口 `/api/suggest`，以及复用 `KeywordRecommender::recommend_json` 的关键字推荐调用链。
- 影响语言：中文、英文都可能触发。
- 触发条件：查询词能从字符索引召回至少一个候选词，并进入 `edit_distance(query, entry.word, realLang)` 计算。
- 严重程度：高。单次合法请求即可导致搜索服务进程崩溃。

## 复现步骤

1. 进入项目目录：

   ```bash
   cd 11_SearchEngine
   ```

2. 启动在线搜索服务：

   ```bash
   ./bin/search_server
   ```

3. 在浏览器打开搜索页面。
4. 切换到“关键字推荐”模式。
5. 输入能命中词典索引的中文或英文关键字。
6. 后台进程崩溃，终端输出 `段错误 (核心已转储)`。

## 实际结果

服务端在处理推荐请求时崩溃，浏览器无法稳定拿到推荐结果。

崩溃调用链可以概括为：

```text
WebHttpServer::handle_api
  -> KeywordRecommender::recommend_json
    -> KeywordRecommender::edit_distance
      -> 越界访问 vector，触发段错误
```

## 期望结果

服务端应返回稳定的 JSON 响应，例如：

```json
{
  "type": "keyword",
  "query": "输入词",
  "lang": "cn",
  "results": [
    {
      "word": "候选词",
      "distance": 1,
      "frequency": 100
    }
  ]
}
```

即使没有候选词，也应返回空数组：

```json
{
  "type": "keyword",
  "query": "输入词",
  "lang": "cn",
  "results": []
}
```

## 根因分析

问题代码位于 `KeywordRecommender::edit_distance` 的动态规划循环：

```cpp
for (std::size_t i = 1; i <= left.size(); ++i) {
    for (std::size_t j = 1 ; i <= right.size(); ++j) {
        if (left[i - 1] == right[j - 1]) {
            dp[i][j] = dp[i - 1][j - 1];
        } else {
            dp[i][j] = std::min({dp[i - 1][j] + 1,
                                 dp[i][j - 1] + 1,
                                 dp[i - 1][j - 1] + 1});
        }
    }
}
```

内层循环的结束条件错误写成了：

```cpp
i <= right.size()
```

这里应该判断内层循环变量 `j`，而不是外层循环变量 `i`。

当前写法会产生两类错误行为：

1. 当 `i <= right.size()` 时，内层循环条件始终为真，`j` 会持续自增，最终访问 `right[j - 1]` 或 `dp[i][j]` 的非法位置。
2. 当 `i > right.size()` 时，内层循环完全不执行，编辑距离矩阵没有被正确填充，结果也不可信。

因此，只要某个候选词长度不小于当前外层位置 `i`，就很容易发生越界访问。浏览器关键字推荐会对召回出的多个候选词逐个计算编辑距离，所以该错误在正常请求路径上稳定暴露。

## 修复方案

将内层循环条件从 `i <= right.size()` 改为 `j <= right.size()`：

```cpp
for (std::size_t i = 1; i <= left.size(); ++i) {
    for (std::size_t j = 1; j <= right.size(); ++j) {
        if (left[i - 1] == right[j - 1]) {
            dp[i][j] = dp[i - 1][j - 1];
        } else {
            dp[i][j] = std::min({dp[i - 1][j] + 1,
                                 dp[i][j - 1] + 1,
                                 dp[i - 1][j - 1] + 1});
        }
    }
}
```

修复后，动态规划矩阵的访问范围与创建范围一致：

- `dp` 行数为 `left.size() + 1`，合法行下标为 `0..left.size()`。
- `dp` 列数为 `right.size() + 1`，合法列下标为 `0..right.size()`。
- 外层循环控制行下标 `i`。
- 内层循环控制列下标 `j`。

## 关联问题

`recommend_json` 初始化响应数组时还存在字段名不一致的问题：

```cpp
response["result"] = nlohmann::json::array();
```

后续追加结果时使用的是：

```cpp
response["results"].push_back(...)
```

这不会导致段错误，但会造成无候选结果时返回字段为 `result`，有候选结果时返回 `result` 和 `results` 混杂。建议同步修复为：

```cpp
response["results"] = nlohmann::json::array();
```

## 验证建议

修复后建议做以下验证：

1. 重新编译项目。
2. 启动 `./bin/search_server`。
3. 浏览器测试中文关键字推荐，确认服务不崩溃且返回 `results` 数组。
4. 浏览器测试英文关键字推荐，确认服务不崩溃且排序结果正常。
5. 测试空输入、纯标点、纯数字等无法召回字符的输入，确认返回空 `results` 数组。
6. 使用 AddressSanitizer 或 gdb 复测，确认 `edit_distance` 不再发生越界访问。

## 结论

本次段错误由 `KeywordRecommender::edit_distance` 中内层循环条件变量写错导致。该错误会让 `j` 越过 `right` 和 `dp` 的合法下标范围，是服务进程崩溃的直接原因。修复循环条件后，关键字推荐的编辑距离计算可以回到预期的二维动态规划访问范围内。
