# 03_SeriesWork 知识点整理

本章代码重点学习 workflow 的串行任务流 `SeriesWork`：把多个异步任务按顺序组织起来，让后一个任务依赖前一个任务的结果。相比前两章，本章不再重复展开 HTTP 报文、MySQL 结果集、`WaitGroup` 等基础知识，只补充本目录代码中新出现或更深入使用的知识点。

对应源码：

- `01_webpage_mysql.cc`：先通过 HTTP 抓取网页，再把网页 URL 和响应体大小写入 MySQL。
- `02_assert.cc`：运行时断言 `assert` 和编译期断言 `static_assert`。
- `practice/01_SeriesWork.cc`：`01_webpage_mysql.cc` 的练习版本，核心逻辑相同。

本章涉及的新增头文件：

```cpp
#include <workflow/Workflow.h>
#include <assert.h>
#include <stdio.h>
```

> [!NOTE]
> `SeriesWork` 解决的是“任务依赖关系”问题：HTTP 任务完成后，才知道网页大小；拿到网页大小后，才创建并执行 MySQL INSERT 任务。用普通异步回调也能写，但 `SeriesWork` 能把这些任务放进同一个串行工作流里，并提供序列级回调和上下文。

## 1. 本章程序的执行目标

`01_webpage_mysql.cc` 的目标是：

1. 用户通过命令行传入 URL。
2. 程序创建 HTTP 客户端任务，请求该 URL。
3. HTTP 成功后读取响应体大小。
4. 动态创建 MySQL INSERT 任务。
5. 把 URL 和网页大小写入 `tbl_webpage`。
6. MySQL 成功后把整个序列标记为成功。
7. `SeriesWork` 结束后执行 series callback，释放上下文并通知主线程退出等待。

执行顺序可以表示为：

```text
main
  |
  | create httpTask
  | create SeriesWork(httpTask)
  | series->start()
  v
httpTask
  |
  | http_callback()
  | create mysqlTask
  | series->push_back(mysqlTask)
  v
mysqlTask
  |
  | mysql_callback()
  | ctx->success = true
  v
series callback
  |
  | delete ctx
  | waitGroup.done()
  v
main resumes
```

对应的动态串行任务链可以画成：

```mermaid
flowchart TD
    A[main 校验 URL 参数] --> B[创建 WFHttpTask]
    B --> C[设置 HTTP 请求头、size limit、receive timeout]
    C --> D[创建 WaitGroup]
    D --> E[Workflow::create_series_work<br/>首任务是 httpTask]
    E --> F[创建 SeriesContext<br/>url, size=0, success=false]
    F --> G[设置序列上下文]
    G --> H[启动序列]
    H --> I[执行 HTTP task]
    I --> J{HTTP callback 是否成功?}
    J -->|否| K[不追加 MySQL task]
    J -->|是| L[读取 body size 并写入 ctx->size]
    L --> M[创建 WFMySQLTask]
    M --> N[拼接 INSERT SQL]
    N --> O[追加 MySQL 任务到序列尾部]
    O --> P[执行 MySQL task]
    P --> Q{MySQL callback 是否 OK?}
    Q -->|是| R[标记序列成功]
    Q -->|否| S[保持 success=false]
    K --> T[series callback]
    R --> T
    S --> T
    T --> U[打印 success/failed]
    U --> V[delete ctx]
    V --> W[通知 WaitGroup 完成]
```

> [!IMPORTANT]
> 这个流程里 MySQL 任务不是在 `main()` 中提前创建的，而是在 HTTP 回调中根据 HTTP 结果动态创建，并追加到当前 `SeriesWork` 后面。

## 2. `SeriesWork` 基础

### 2.1 什么是 `SeriesWork`

`SeriesWork` 是 workflow 中的串行任务容器。它内部保存一组 `SubTask`，按顺序执行：

```text
task1 -> task2 -> task3 -> series callback
```

每个 `WFHttpTask`、`WFMySQLTask` 等 workflow 任务都属于 `SubTask` 的派生体系，因此可以被放入 `SeriesWork`。

头文件：

```cpp
#include <workflow/Workflow.h>
```

核心接口：

```cpp
SeriesWork *Workflow::create_series_work(SubTask *first,
                                         series_callback_t callback);

void SeriesWork::start();
void SeriesWork::push_back(SubTask *task);
void SeriesWork::push_front(SubTask *task);
void SeriesWork::set_context(void *context);
void *SeriesWork::get_context() const;
void SeriesWork::cancel();
bool SeriesWork::is_canceled() const;
bool SeriesWork::is_finished() const;
```

### 2.2 创建串行任务流

源码：

```cpp
SeriesWork* series = Workflow::create_series_work(
    httpTask,
    [&waitGroup](const SeriesWork* series) {
        SeriesContext* ctx =
            static_cast<SeriesContext*>(series->get_context());

        if (ctx->success) {
            cout << "success! " << endl;
        } else {
            cout << "failed! " << endl;
        }

        delete ctx;
        waitGroup.done();
    });
```

含义：

- `httpTask` 是序列中的第一个任务。
- 第二个参数是 series callback，整个序列结束时调用。
- `create_series_work()` 只创建序列，不启动。
- `series->start()` 才真正启动序列。

> [!NOTE]
> `Workflow::start_series_work(first, callback)` 可以创建并立即启动序列；本章使用 `create_series_work()`，因为还要在启动前设置上下文 `series->set_context(ctx)`。

### 2.3 启动序列

源码：

```cpp
series->start();
waitGroup.wait();
```

`series->start()` 会从第一个任务开始调度执行。之后当前线程继续向下走，因此仍然需要 `waitGroup.wait()` 等待整个异步序列结束。

> [!CAUTION]
> 不要对已经属于某个 `SeriesWork` 的任务再直接调用 `task->start()`。workflow 的任务 `start()` 内部有 `assert(!series_of(this))`，意思是任务如果已经在序列里，就不能再作为独立任务启动。

### 2.4 series callback 的时机

series callback 在整个序列结束后执行：

```cpp
[&waitGroup](const SeriesWork* series) {
    // 所有已经进入序列并执行到的任务结束后调用
}
```

本章实际顺序：

```text
HTTP task callback
MySQL task callback
series callback
```

如果 HTTP 失败，没有追加 MySQL 任务，则顺序是：

```text
HTTP task callback
series callback
```

> [!IMPORTANT]
> series callback 不等同于某个任务的 callback。任务 callback 负责处理单个任务结果；series callback 负责处理整个序列收尾，例如释放上下文、通知主线程、汇总成功失败。

`SeriesWork` 的核心时序如下：

```mermaid
sequenceDiagram
    participant M as main
    participant S as SeriesWork
    participant H as HTTP task
    participant HC as http_callback
    participant Q as MySQL task
    participant MC as mysql_callback
    participant SC as series callback
    participant W as WaitGroup

    M->>S: 创建以 HTTP 为首任务的序列
    M->>S: 设置上下文
    M->>S: 启动序列
    M->>W: 等待序列结束
    S->>H: 调度首任务
    H->>HC: HTTP 完成后执行
    alt HTTP 成功且状态码为 2xx
        HC->>S: 追加 MySQL 任务
        S->>Q: HTTP callback 返回后执行 MySQL
        Q->>MC: MySQL 完成后执行
        MC->>S: 读取 context 并标记 success
    else HTTP 失败或非 2xx
        HC-->>S: 不追加后续任务
    end
    S->>SC: 序列结束后执行
    SC->>SC: 释放上下文
    SC->>W: 通知完成
    W-->>M: main 继续
```

## 3. 动态追加任务：`series->push_back()`

### 3.1 在 HTTP 回调中追加 MySQL 任务

源码：

```cpp
SeriesWork* series = series_of(httpTask);

WFMySQLTask* mysqlTask = WFTaskFactory::create_mysql_task(
    "mysql://root:123456@localhost:3306/demo",
    3,
    mysql_callback);

MySQLRequest* mysql_req = mysqlTask->get_req();
mysql_req->set_query(sql);

series->push_back(mysqlTask);
```

含义：

- `series_of(httpTask)` 获取当前 HTTP 任务所属的 `SeriesWork`。
- 根据 HTTP 任务结果创建 MySQL 任务。
- 设置 SQL。
- `push_back(mysqlTask)` 把 MySQL 任务追加到序列尾部。

由于当前 HTTP 任务正在执行，追加到尾部的 MySQL 任务会在 HTTP 任务 callback 结束后继续执行。

### 3.2 为什么要动态创建任务

MySQL 任务依赖 HTTP 结果：

```cpp
resp->get_parsed_body(&body, &size);
ctx->size = size;
```

SQL 需要使用 `size`：

```cpp
string sql = "INSERT INTO tbl_webpage (url, size) VALUES ('"
    + url + "', "
    + std::to_string(size) + ")";
```

如果在 `main()` 中提前创建 MySQL 任务，此时还不知道网页大小。因此本章选择在 HTTP callback 中创建并追加。

> [!NOTE]
> `SeriesWork` 支持“先放入一个任务，后续任务在前一个任务 callback 中按需追加”。这适合分支逻辑、依赖上游结果的异步流程。

`push_back()` 在本章中体现的是“运行时决定后续任务”：

```mermaid
stateDiagram-v2
    [*] --> SeriesCreated: 首任务是 HTTP
    SeriesCreated --> HttpRunning: 启动序列
    HttpRunning --> HttpCallback: HTTP 完成
    HttpCallback --> SeriesEnds: HTTP 失败或非 2xx
    HttpCallback --> MysqlAppended: HTTP 成功，追加 MySQL
    MysqlAppended --> MysqlRunning: callback 返回后调度
    MysqlRunning --> MysqlCallback: MySQL 完成
    MysqlCallback --> SeriesEnds: 回调结束
    SeriesEnds --> SeriesCallback: 执行序列回调
    SeriesCallback --> [*]
```

### 3.3 `push_back()` 与 `push_front()`

`SeriesWork` 提供：

```cpp
void push_back(SubTask *task);
void push_front(SubTask *task);
```

区别：

| 接口 | 含义 | 适合场景 |
| --- | --- | --- |
| `push_back()` | 把任务追加到序列尾部。 | 常规后续任务。 |
| `push_front()` | 把任务插入到序列头部或更靠前位置。 | 框架内部或需要优先执行的新任务。 |

本章使用 `push_back()`，因为 MySQL 应该在 HTTP 之后执行。

## 4. 获取当前序列：`series_of(task)`

### 4.1 从任务拿到所属序列

源码：

```cpp
SeriesWork* series = series_of(httpTask);
```

以及：

```cpp
SeriesWork* series = series_of(mysqlTask);
```

`series_of()` 定义在 `Workflow.h`：

```cpp
static inline SeriesWork *series_of(const SubTask *task)
{
    return (SeriesWork *)task->get_pointer();
}
```

它根据任务内部保存的指针，找到任务所属的 `SeriesWork`。

### 4.2 使用场景

在任务 callback 中，常需要访问序列：

- 获取序列上下文。
- 根据当前任务结果追加后续任务。
- 取消整个序列。
- 检查序列状态。

本章使用了前两种：

```cpp
SeriesWork* series = series_of(httpTask);
SeriesContext* ctx =
    static_cast<SeriesContext*>(series->get_context());

series->push_back(mysqlTask);
```

> [!IMPORTANT]
> 只有已经放入 `SeriesWork` 并由序列调度的任务，`series_of(task)` 才有意义。独立创建但尚未加入序列的任务，不应该依赖 `series_of()`。

`series_of(task)` 的查找关系可以理解为：

```mermaid
flowchart LR
    A[SubTask: httpTask/mysqlTask] --> B[task 内部 pointer]
    B --> C[SeriesWork]
    C --> D[get_context]
    D --> E[SeriesContext]
    C --> F[push_back / cancel / is_finished]
```

## 5. 序列级上下文：`set_context()` / `get_context()`

### 5.1 为什么需要序列上下文

HTTP 任务、MySQL 任务、series callback 之间需要共享数据：

- URL。
- HTTP 响应体大小。
- 整个序列是否成功。

源码定义：

```cpp
struct SeriesContext {
    string url;
    size_t size;
    bool success;
};
```

字段含义：

| 字段 | 含义 |
| --- | --- |
| `url` | 用户传入的网页 URL。 |
| `size` | HTTP 响应体大小。 |
| `success` | 整个序列是否成功完成。 |

### 5.2 设置上下文

源码：

```cpp
SeriesContext* ctx = new SeriesContext{ argv[1], 0, false };
series->set_context(ctx);
```

要点：

- 使用 `new` 在堆上创建上下文。
- 初始 `url` 来自命令行参数。
- 初始 `size` 为 `0`。
- 初始 `success` 为 `false`，只有 MySQL 成功后才改成 `true`。

> [!NOTE]
> `SeriesContext{ argv[1], 0, false }` 使用聚合初始化，按结构体字段声明顺序依次初始化。

### 5.3 获取上下文

HTTP callback 中：

```cpp
SeriesContext* ctx =
    static_cast<SeriesContext*>(series->get_context());

ctx->size = size;
```

MySQL callback 中：

```cpp
SeriesContext* ctx =
    static_cast<SeriesContext*>(series->get_context());

ctx->success = true;
```

series callback 中：

```cpp
SeriesContext* ctx =
    static_cast<SeriesContext*>(series->get_context());

delete ctx;
```

`SeriesWork` 的 context 类型是 `void*`，因此取出后需要转换回原始类型。

> [!CAUTION]
> `set_context()` 只保存裸指针，不接管内存管理。谁 `new`，谁就要安排合适位置 `delete`。本章在 series callback 中统一释放，是合理的收尾点。

### 5.4 与 `task->user_data` 的区别

代码中注释提到：

```cpp
// httpTask->user_data = argv[1];
// 可以使用 user_data 传递 URL, 也可使用 SeriesContext 上下文传递 URL
```

区别：

| 方式 | 绑定对象 | 适合用途 |
| --- | --- | --- |
| `task->user_data` | 单个任务。 | 只给某一个任务 callback 使用的数据。 |
| `series->set_context()` | 整个序列。 | 多个任务和 series callback 共享的数据。 |

本章 URL、size、success 都要跨 HTTP、MySQL、series callback 使用，所以更适合放在 `SeriesContext`。

> [!IMPORTANT]
> 如果数据属于整个流程，而不是某个单独任务，优先使用 `SeriesWork` context。这样可以减少多个任务之间手工传裸指针的混乱。

本章 `SeriesContext` 的数据流如下：

```mermaid
flowchart TD
    A[main new SeriesContext] --> B[url = argv[1]]
    A --> C[size = 0]
    A --> D[success = false]
    A --> E[series->set_context(ctx)]
    E --> F[http_callback get_context]
    F --> G[读取 HTTP body size]
    G --> H[ctx->size = size]
    H --> I[拼接 SQL 使用 ctx->url 和 ctx->size]
    E --> J[mysql_callback get_context]
    J --> K[MySQL OK 后 ctx->success = true]
    E --> L[series callback get_context]
    L --> M[根据 success 打印结果]
    M --> N[delete ctx]
```

## 6. HTTP 任务中的新增设置

基础 HTTP 任务创建和响应解析前面笔记已经整理过，这里只记录本章新增的设置。

### 6.1 设置请求头：`add_header_pair()`

源码：

```cpp
req->add_header_pair("Accept", "*/*");
req->add_header_pair("User-Agent", "wget/1.14 (linux-gnu)");
req->add_header_pair("Connection", "close");
```

`add_header_pair()` 表示添加 header。与 `set_header_pair()` 的区别：

| 接口 | 语义 |
| --- | --- |
| `add_header_pair()` | 添加一条 header，可能允许同名 header 多次出现。 |
| `set_header_pair()` | 设置 header，通常用于替换或覆盖同名 header。 |

本章模拟 wget 客户端：

- `Accept: */*` 表示接受任意类型响应。
- `User-Agent: wget/1.14 (linux-gnu)` 表示客户端身份。
- `Connection: close` 表示请求结束后关闭连接。

### 6.2 限制响应体大小：`set_size_limit()`

源码：

```cpp
HttpResponse* resp = httpTask->get_resp();
resp->set_size_limit(20 * 1024 * 1024); // 20MB
```

`set_size_limit()` 来自 `ProtocolMessage`，用于限制消息大小。

这里限制 HTTP 响应最多 20MB：

```text
20 * 1024 * 1024 = 20,971,520 bytes
```

用途：

- 防止下载过大的响应体占用大量内存。
- 避免服务端异常返回巨大内容时拖垮客户端。
- 给教学程序设置明确资源边界。

> [!IMPORTANT]
> 网络客户端读取响应体时应设置合理大小限制。否则如果目标 URL 返回超大文件，程序可能消耗大量内存。

### 6.3 设置接收超时：`set_receive_timeout()`

源码：

```cpp
httpTask->set_receive_timeout(30 * 1000); // 30s
```

`set_receive_timeout()` 来自 `WFNetworkTask`：

```cpp
void set_receive_timeout(int timeout);
```

单位是毫秒：

```text
30 * 1000 = 30000 ms = 30 s
```

作用：

- 限制接收响应的最长等待时间。
- 目标服务器迟迟不返回数据时，任务会超时失败。
- 超时失败时可通过 `get_state()`、`get_error()` 和 `WFGlobal::get_error_string()` 查看原因。

相关超时接口：

| 接口 | 作用 |
| --- | --- |
| `set_send_timeout()` | 设置发送超时。 |
| `set_receive_timeout()` | 设置接收超时。 |
| `set_keep_alive()` | 设置长连接保活时间。 |
| `set_watch_timeout()` | 设置整体 watch 超时。 |

> [!NOTE]
> workflow 的网络超时单位是毫秒。设置时要注意 `30` 和 `30 * 1000` 的区别，前者只有 30 毫秒。

## 7. 成功/失败标记的设计

### 7.1 初始状态为失败

源码：

```cpp
SeriesContext* ctx = new SeriesContext{ argv[1], 0, false };
```

`success` 初始为 `false`。只有完整链路成功后才改成 `true`：

```cpp
ctx->success = true;
```

这种设计是保守的：

- HTTP 失败，不改 `success`。
- HTTP 状态码不是 2xx，不改 `success`。
- MySQL 任务失败，不改 `success`。
- MySQL 返回错误包，不改 `success`。
- 只有 MySQL OK 包后，才认为整个序列成功。

> [!IMPORTANT]
> 异步流程中建议默认失败，最后一个关键步骤成功后再显式标记成功。这样中间任何提前返回都不会误报成功。

### 7.2 HTTP 成功还要检查状态码

源码：

```cpp
const char* status_code = resp->get_status_code();
if (status_code == nullptr || status_code[0] != '2') {
    cerr << "HTTP请求失败，状态码: "
         << (status_code ? status_code : "unknown")
         << endl;
    return;
}
```

这里没有重复展开 HTTP 基础，只强调本章的判断策略：

- `WFT_STATE_SUCCESS` 只说明 HTTP 任务在网络和协议层完成。
- `status_code[0] == '2'` 表示 2xx 状态码。
- 只有 2xx 才继续写 MySQL。

这种写法能匹配：

```text
200 OK
201 Created
204 No Content
```

但不匹配：

```text
301 Moved Permanently
404 Not Found
500 Internal Server Error
```

> [!CAUTION]
> 使用 `status_code[0] == '2'` 是简洁判断，但状态码本质是三位字符串。更严谨的业务代码可以把它转换成整数，再判断 `200 <= code && code < 300`。

### 7.3 MySQL 成功还要检查包类型

源码：

```cpp
if (resp->get_packet_type() != MYSQL_PACKET_OK) {
    cerr << resp->get_error_code() << " "
         << resp->get_error_msg() << endl;
    return;
}
```

本章执行的是 INSERT，所以期望 MySQL 返回 OK 包。

只有满足：

```text
workflow task success
MySQL packet OK
```

才执行：

```cpp
ctx->success = true;
```

成功/失败标记的决策链如下：

```mermaid
flowchart TD
    A[初始状态为失败] --> B{HTTP 任务状态成功?}
    B -->|否| Z[保持 false]
    B -->|是| C{HTTP status_code 是 2xx?}
    C -->|否| Z
    C -->|是| D[追加 MySQL INSERT]
    D --> E{MySQL task state 成功?}
    E -->|否| Z
    E -->|是| F{MySQL packet 是 OK?}
    F -->|否| Z
    F -->|是| G[标记 success 为 true]
    Z --> H[series callback 输出 failed]
    G --> I[series callback 输出 success]
```

## 8. SQL 拼接与风险

### 8.1 本章 SQL 拼接方式

源码：

```cpp
string url = ctx->url;
string sql = "INSERT INTO tbl_webpage (url, size) VALUES ('"
    + url + "', "
    + std::to_string(size) + ")";
```

涉及 C++ 知识点：

- `std::string` 拼接。
- `std::to_string(size)` 把数字转换成字符串。
- `size_t` 表示字节大小。

生成示例：

```sql
INSERT INTO tbl_webpage (url, size)
VALUES ('http://example.com', 12560)
```

### 8.2 SQL 注入和转义问题

用户输入的 URL 直接拼进 SQL：

```cpp
string url = ctx->url;
```

如果 URL 中包含单引号：

```text
http://example.com/a'b
```

生成的 SQL 会变成：

```sql
VALUES ('http://example.com/a'b', 12560)
```

这会破坏 SQL 字符串结构。更严重时可能产生 SQL 注入风险。

> [!CAUTION]
> 直接拼接用户输入到 SQL 只适合教学演示。真实项目应使用参数化查询、预处理语句，或至少对字符串做数据库规则下的转义。

### 8.3 表结构推断

代码写入：

```sql
tbl_webpage (url, size)
```

可以推断练习表需要至少包含：

```sql
CREATE TABLE IF NOT EXISTS tbl_webpage (
    id BIGINT UNSIGNED PRIMARY KEY AUTO_INCREMENT,
    url TEXT NOT NULL,
    size BIGINT UNSIGNED NOT NULL
);
```

> [!NOTE]
> 这是根据示例 SQL 推断出的学习用表结构，真实表结构可以根据业务增加创建时间、状态码、内容类型、hash 等字段。

## 9. `WaitGroup` 与 lambda 捕获

### 9.1 为什么捕获引用

源码：

```cpp
WFFacilities::WaitGroup waitGroup(1);

SeriesWork* series = Workflow::create_series_work(
    httpTask,
    [&waitGroup](const SeriesWork* series) {
        // ...
        waitGroup.done();
    });
```

这里使用引用捕获：

```cpp
[&waitGroup]
```

原因是代码注释中提到：`WaitGroup` 为了防止误用删除了拷贝构造函数。值捕获需要拷贝对象，因此不适合。

### 9.2 lambda 捕获方式对比

| 捕获方式 | 示例 | 含义 |
| --- | --- | --- |
| 空捕获 | `[]` | 不捕获外部变量。 |
| 值捕获 | `[waitGroup]` | 拷贝外部变量。 |
| 引用捕获 | `[&waitGroup]` | 引用外部变量。 |
| 全部引用捕获 | `[&]` | 用到的外部变量都按引用捕获。 |
| 全部值捕获 | `[=]` | 用到的外部变量都按值捕获。 |

> [!IMPORTANT]
> 异步回调中引用捕获外部变量时，要确保被引用对象在回调执行时仍然存活。本章 `waitGroup` 是 `main()` 中的局部变量，`main()` 会阻塞在 `waitGroup.wait()`，直到 series callback 调用 `done()`，所以生命周期是匹配的。

`waitGroup` 引用捕获的生命周期关系如下：

```mermaid
sequenceDiagram
    participant M as main 栈帧
    participant W as waitGroup
    participant SC as series callback

    M->>W: 构造局部 WaitGroup
    M->>SC: lambda 按引用捕获 waitGroup
    M->>M: waitGroup.wait() 保持 main 不返回
    SC->>W: 序列结束后调用 done()
    W-->>M: wait 返回
    M->>W: main 结束，waitGroup 析构
```

## 10. 内存管理与生命周期

### 10.1 `SeriesContext` 的生命周期

创建：

```cpp
SeriesContext* ctx = new SeriesContext{ argv[1], 0, false };
series->set_context(ctx);
```

释放：

```cpp
delete ctx;
waitGroup.done();
```

为什么在 series callback 中释放：

- HTTP callback 之后还要给 MySQL callback 使用。
- MySQL callback 之后还要给 series callback 判断成功失败。
- series callback 是整个序列的最后收尾位置。

> [!IMPORTANT]
> 序列上下文最好在 series callback 中统一释放。不要在某个中间任务 callback 中释放，否则后续任务或 series callback 可能访问悬空指针。

### 10.2 workflow 任务对象的生命周期

示例中任务对象通过工厂创建：

```cpp
WFHttpTask* httpTask = WFTaskFactory::create_http_task(...);
WFMySQLTask* mysqlTask = WFTaskFactory::create_mysql_task(...);
```

代码没有手动 `delete httpTask` 或 `delete mysqlTask`。workflow 的任务对象通常由框架在任务完成后管理释放。

> [!CAUTION]
> 不要自行 `delete` 已经交给 workflow 调度的任务对象。任务生命周期由 workflow 框架管理；用户主要负责自己挂载的上下文数据，例如 `SeriesContext`。

`SeriesContext` 和 workflow 任务对象的生命周期职责不同：

```mermaid
flowchart TD
    A[main 创建 httpTask] --> B[交给 Workflow/SeriesWork 管理]
    C[http_callback 创建 mysqlTask] --> B
    D[main new SeriesContext] --> E[用户负责释放]
    E --> F[series->set_context(ctx)]
    F --> G[HTTP/MySQL/series callback 共享]
    G --> H[series callback delete ctx]
    B --> I[任务完成后由 workflow 管理释放]
```

## 11. 取消序列：`cancel()`

本章代码没有调用 `cancel()`，但 `SeriesWork` 提供该接口：

```cpp
series->cancel();
```

语义：

- 通常在属于该 series 的某个任务 callback 中调用。
- 后续尚未执行的任务会被取消和销毁。
- 当前 series 的 callback 仍会被调用。

可能用法：

```cpp
if (业务条件失败) {
    series_of(task)->cancel();
    return;
}
```

在本章代码里，HTTP 失败时没有追加 MySQL 任务，因此不需要取消；序列自然结束后进入 series callback。

> [!NOTE]
> 如果一个 series 已经提前放入多个后续任务，而中途发现不应继续执行，可以考虑 `cancel()`。如果后续任务还没有创建，直接不 `push_back()` 即可。

如果使用 `cancel()`，控制流通常是：

```mermaid
flowchart TD
    A[某个任务 callback] --> B{是否需要继续序列?}
    B -->|是| C[正常返回，后续任务继续]
    B -->|否| D[series_of(task)->cancel()]
    D --> E[后续未执行任务被取消/销毁]
    E --> F[series callback 仍会执行]
    F --> G[统一释放 context / done]
```

## 12. `02_assert.cc`：断言

### 12.1 运行时断言 `assert`

源码：

```cpp
int a = 5;
assert(a == 4 && "a不等于5");
```

`assert` 来自：

```cpp
#include <assert.h>
```

作用：

- 在运行时检查表达式是否为真。
- 如果表达式为真，程序继续运行。
- 如果表达式为假，程序终止，并打印断言失败位置。

这里：

```cpp
a == 4
```

为假，因此程序会在这里终止。

`&& "a不等于5"` 的作用是把一段可读文本放进断言表达式中。因为字符串字面量会转换成非空指针，单独看为真；当左侧条件失败时，整体表达式仍为假，但错误输出里能看到这段提示文本。

> [!NOTE]
> 示例注释写的是“如何给定出错信息”，`assert(condition && "message")` 是 C/C++ 中常见的简易写法。

### 12.2 `NDEBUG` 对 `assert` 的影响

如果编译时定义 `NDEBUG`：

```bash
g++ -DNDEBUG 02_assert.cc
```

`assert(...)` 会被禁用，不再执行运行时检查。

> [!CAUTION]
> 不要把有副作用的逻辑写进 `assert`，例如 `assert(++i > 0)`。发布版本定义 `NDEBUG` 后，这段表达式可能完全不执行。

### 12.3 编译期断言 `static_assert`

源码：

```cpp
static_assert(sizeof(int) == 4, "int的大小不为4");
```

`static_assert` 在编译期检查表达式：

- 条件为真，编译通过。
- 条件为假，编译失败，并显示错误消息。

适合检查：

- 类型大小。
- 模板参数约束。
- 编译期常量条件。
- 平台假设。

> [!IMPORTANT]
> `assert` 是运行时检查，可能被 `NDEBUG` 关闭；`static_assert` 是编译期检查，不依赖程序运行，也不会被 `NDEBUG` 关闭。

`assert` 与 `static_assert` 的检查阶段不同：

```mermaid
flowchart LR
    A[源代码] --> B{static_assert}
    B -->|条件为假| C[编译失败]
    B -->|条件为真| D[生成可执行程序]
    D --> E{运行时 assert}
    E -->|条件为真| F[程序继续]
    E -->|条件为假且未定义 NDEBUG| G[程序终止并打印位置]
    E -->|定义 NDEBUG| H[assert 被禁用，不执行检查]
```

## 13. 本章 C++ 语法点

### 13.1 结构体作为上下文

源码：

```cpp
struct SeriesContext {
    string url;
    size_t size;
    bool success;
};
```

这是把相关数据组合成一个上下文对象，比散落多个全局变量更清晰。

优点：

- 数据归属明确。
- 便于传给 `SeriesWork::set_context()`。
- 便于统一初始化和释放。

### 13.2 `size_t`

源码：

```cpp
size_t size;
```

`size_t` 是无符号整数类型，常用于表示对象大小、字节数、容器长度。

HTTP 响应体大小使用 `size_t` 是合理的：

```cpp
resp->get_parsed_body(&body, &size);
```

### 13.3 `std::to_string`

源码：

```cpp
std::to_string(size)
```

作用是把数字转换成 `std::string`，用于 SQL 拼接。

### 13.4 命令行参数校验

源码：

```cpp
if (argc != 2) {
    cerr << "Usage: " << argv[0] << " <URL>" << endl;
    return 1;
}
```

含义：

- 程序需要 1 个用户参数，即 URL。
- `argc != 2` 表示参数数量不符合要求。
- `argv[0]` 是程序名。
- 返回 `1` 表示异常退出。

## 14. 调试与运行参考

### 14.1 建表参考

本章需要 `tbl_webpage` 表。可参考：

```sql
CREATE DATABASE IF NOT EXISTS demo;

USE demo;

CREATE TABLE IF NOT EXISTS tbl_webpage (
    id BIGINT UNSIGNED PRIMARY KEY AUTO_INCREMENT,
    url TEXT NOT NULL,
    size BIGINT UNSIGNED NOT NULL
);
```

### 14.2 运行示例

编译方式取决于本机 workflow/MySQL 库安装方式，可能类似：

```bash
g++ 01_webpage_mysql.cc -o webpage_mysql -lworkflow -lpthread
```

运行：

```bash
./webpage_mysql http://www.baidu.com
```

预期观察：

```text
[SQL] INSERT INTO tbl_webpage (url, size) VALUES (...)
MySQL任务完成!
插入记录: id = ...
success!
```

如果 HTTP 失败，可能输出：

```text
HTTP请求失败，状态码: 404
failed!
```

如果 MySQL 失败，可能输出：

```text
MySQL任务失败: ...
failed!
```

### 14.3 检查数据库

```sql
SELECT * FROM tbl_webpage ORDER BY id DESC LIMIT 5;
```

可以确认 URL 和 size 是否写入。

## 15. 易错点总结

- 已经放入 `SeriesWork` 的任务不要再单独调用 `task->start()`。
- `create_series_work()` 只创建序列，必须调用 `series->start()` 才会执行。
- `series_of(task)` 只适用于已经属于某个序列的任务。
- 需要跨多个任务共享的数据应放在 `SeriesWork` context 中。
- `set_context()` 保存的是裸 `void*`，不会自动释放内存。
- 上下文应在 series callback 中统一释放，避免中间释放导致悬空指针。
- 默认 `success=false`，最后关键任务成功后再改成 `true`。
- HTTP 任务成功后还要检查 HTTP 状态码。
- MySQL 任务成功后还要检查 MySQL 包类型。
- 用户输入直接拼接 SQL 有注入和转义风险。
- 异步 lambda 引用捕获时必须确认被引用对象生命周期足够长。
- `assert` 可能被 `NDEBUG` 关闭，不要放业务副作用。
- `static_assert` 是编译期检查，适合验证平台和类型假设。

> [!IMPORTANT]
> 本章核心是理解 `SeriesWork` 的数据流和控制流：任务按序执行，前一个任务的 callback 可以决定是否追加后一个任务，多个任务通过 series context 共享状态，最后由 series callback 做统一收尾。
