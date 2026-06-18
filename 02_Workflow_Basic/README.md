# 02_Workflow_Basic 知识点整理

本章代码学习 workflow 的基础用法：原生 HTTP 服务端、HTTP 客户端异步任务、等待异步任务完成、MySQL 异步任务、MySQL DML/DQL 结果解析，以及把 HTTP 响应保存为文件的简单 wget 工具。

对应源码：

- `01_hello_workflow.cc`：使用 `WFHttpServer` 搭建最小 HTTP 服务端。
- `02_fetch_baidu.cc`：使用 `WFTaskFactory::create_http_task()` 创建 HTTP 客户端任务，抓取网页并解析响应。
- `03_mysql_insert.cc`：使用 `WFTaskFactory::create_mysql_task()` 执行 INSERT，解析 DML 结果。
- `04_mysql_select.cc`：执行 SELECT，使用 `MySQLResultCursor` 遍历结果集。
- `practice/01_wget.cc`：命令行 wget 工具，把响应写入文件。
- `practice/02_mysql_insert.cc`：INSERT 练习。
- `practice/03_mysql_select.cc`：SELECT 练习。

本章涉及的主要头文件：

```cpp
#include <workflow/WFHttpServer.h>
#include <workflow/WFTaskFactory.h>
#include <workflow/HttpMessage.h>
#include <workflow/HttpUtil.h>
#include <workflow/WFFacilities.h>
#include <workflow/WFGlobal.h>
#include <workflow/WFTask.h>
#include <workflow/MySQLMessage.h>
#include <workflow/MySQLResult.h>
#include <workflow/mysql_types.h>
```

> [!NOTE]
> 上一章 `wfrest` 是基于 workflow 的更高层 REST 封装；本章直接使用 workflow 原生接口。原生接口更贴近任务模型，代码里会直接接触 `WFHttpTask`、`WFMySQLTask`、`HttpRequest`、`HttpResponse`、`MySQLRequest`、`MySQLResponse` 等类型。

## 1. workflow 的核心思想

### 1.1 异步任务模型

workflow 的核心是“任务”：

- 创建任务。
- 设置任务参数。
- 调用 `start()` 提交任务。
- 任务完成后由框架调用回调函数。
- 在回调中检查状态、读取结果、处理业务。

典型结构：

```cpp
WFHttpTask *task = WFTaskFactory::create_http_task(
    url,
    redirect_max,
    retry_max,
    callback);

task->get_req()->set_method("GET");
task->start();
```

`task->start()` 不会阻塞等待结果，它只是把任务交给 workflow 框架调度。

> [!IMPORTANT]
> workflow 客户端任务是异步执行的。`start()` 返回时请求通常还没完成，主线程如果立刻结束，进程会退出，回调函数可能没有机会执行。

基础异步任务模型可以画成下面的状态流程：

```mermaid
stateDiagram-v2
    [*] --> Created: create_xxx_task
    Created --> Configured: 设置请求或 SQL
    Configured --> Submitted: task->start()
    Submitted --> Running: workflow 异步调度
    Running --> Callback: 任务完成
    Callback --> SuccessPath: get_state 成功
    Callback --> ErrorPath: get_state 失败
    SuccessPath --> Done: 解析响应或结果
    ErrorPath --> Done: 打印错误并收尾
    Done --> [*]
```

### 1.2 请求对象与响应对象

workflow 网络任务一般有两个对象：

- `get_req()`：获取请求对象，用来设置请求参数。
- `get_resp()`：获取响应对象，用来读取执行结果。

HTTP 任务：

```cpp
HttpRequest *req = task->get_req();
HttpResponse *resp = task->get_resp();
```

MySQL 任务：

```cpp
MySQLRequest *req = task->get_req();
MySQLResponse *resp = task->get_resp();
```

请求对象在 `start()` 前设置；响应对象通常在回调函数里读取。

### 1.3 回调函数

HTTP 回调：

```cpp
void http_callback(WFHttpTask *task)
{
    // 任务完成后执行
}
```

MySQL 回调：

```cpp
void mysql_callback(WFMySQLTask *task)
{
    // 任务完成后执行
}
```

回调函数中必须先判断任务状态：

```cpp
int state = task->get_state();
if (state != WFT_STATE_SUCCESS) {
    cerr << WFGlobal::get_error_string(state, task->get_error()) << endl;
    return;
}
```

> [!CAUTION]
> `WFT_STATE_SUCCESS` 只说明网络任务或协议任务完成成功，不一定说明业务成功。例如 HTTP `404` 也可能是任务成功，MySQL SQL 语法错误也可能收到响应后表现为 MySQL error packet。

## 2. 等待异步任务：`WFFacilities::WaitGroup`

### 2.1 为什么需要 WaitGroup

客户端任务异步执行，`main()` 线程需要等待任务完成。

示例：

```cpp
WFFacilities::WaitGroup waitGroup(1);

task->start();
waitGroup.wait();
```

回调中：

```cpp
waitGroup.done();
```

含义：

- `WaitGroup waitGroup(1)`：当前有 1 个异步任务需要等待。
- `waitGroup.wait()`：阻塞当前线程，直到计数归零。
- `waitGroup.done()`：任务完成后计数减 1。

### 2.2 成功和失败路径都要 `done()`

示例：

```cpp
if (state != WFT_STATE_SUCCESS) {
    cerr << WFGlobal::get_error_string(state, task->get_error()) << endl;
    waitGroup.done();
    return;
}

// 正常处理结果
waitGroup.done();
```

> [!IMPORTANT]
> 使用 `WaitGroup` 时，回调函数里的每条返回路径都必须调用 `done()`。如果失败路径提前 `return` 但没有 `done()`，主线程会永久阻塞在 `wait()`。

`WaitGroup` 在客户端示例中的作用是把异步回调和主线程退出条件连接起来：

```mermaid
sequenceDiagram
    participant M as main线程
    participant T as 异步任务
    participant C as callback
    participant W as WaitGroup

    M->>W: 构造 WaitGroup(1)
    M->>T: task->start()
    M->>W: wait()
    T->>C: 任务完成后执行回调
    alt 成功路径
        C->>C: 解析响应或结果
        C->>W: done()
    else 失败路径
        C->>C: 打印错误
        C->>W: done()
    end
    W-->>M: 计数归零，wait 返回
```

### 2.3 与 `getchar()` 的区别

`01_hello_workflow.cc` 中服务端使用：

```cpp
getchar();
server.stop();
```

这是让服务端一直运行，直到用户按键退出。

客户端示例使用：

```cpp
waitGroup.wait();
```

这是等待一个或多个异步任务自然完成。

两者区别：

| 方式 | 适合场景 | 特点 |
| --- | --- | --- |
| `getchar()` | 服务端常驻、教学演示。 | 需要手动按键结束。 |
| `WaitGroup` | 等待异步客户端任务完成。 | 任务完成后自动解除阻塞。 |

## 3. 任务状态与错误信息

### 3.1 `get_state()` 和 `get_error()`

所有 workflow 任务都提供：

```cpp
int state = task->get_state();
int error = task->get_error();
```

常见状态：

| 状态 | 含义 |
| --- | --- |
| `WFT_STATE_UNDEFINED` | 未定义或尚未完成。 |
| `WFT_STATE_SUCCESS` | 任务成功。 |
| `WFT_STATE_TOREPLY` | 服务端任务专用，表示需要回复。 |
| `WFT_STATE_NOREPLY` | 服务端任务专用，表示不回复。 |
| `WFT_STATE_SYS_ERROR` | 系统错误。 |
| `WFT_STATE_SSL_ERROR` | SSL/TLS 错误。 |
| `WFT_STATE_DNS_ERROR` | DNS 解析错误，客户端任务常见。 |
| `WFT_STATE_TASK_ERROR` | 任务层错误。 |
| `WFT_STATE_ABORTED` | 任务被中止。 |

将错误转换成可读字符串：

```cpp
WFGlobal::get_error_string(state, task->get_error())
```

> [!NOTE]
> 网络失败、DNS 失败、连接失败、SSL 失败通常体现在 `get_state()` 和 `get_error()`；HTTP 状态码和 MySQL SQL 错误需要继续解析响应对象。

任务层成功之后，还要继续区分 HTTP 状态码、MySQL 错误包和业务结果：

```mermaid
flowchart TD
    A[任务回调开始] --> B{get_state 是否成功}
    B -->|否| C[任务层失败<br/>DNS/TCP/SSL/超时等]
    C --> D[WFGlobal::get_error_string]
    B -->|是| E{任务类型}
    E -->|HTTP| F[读取 HttpResponse]
    F --> G[检查 HTTP status code]
    G --> H[再判断业务是否成功]
    E -->|MySQL| I[读取 MySQLResponse]
    I --> J{是否 MYSQL_PACKET_ERROR}
    J -->|是| K[get_error_code/get_error_msg]
    J -->|否| L[MySQLResultCursor 解析结果]
```

## 4. `01_hello_workflow.cc`：最小 HTTP 服务端

### 4.1 创建 `WFHttpServer`

源码：

```cpp
WFHttpServer server([](WFHttpTask* task) {
    HttpResponse* resp = task->get_resp();
    resp->append_output_body("<html>Hello World!</html>");
});
```

`WFHttpServer` 定义在：

```cpp
#include <workflow/WFHttpServer.h>
```

从 workflow 头文件可以看到：

```cpp
using WFHttpServer = WFServer<protocol::HttpRequest,
                              protocol::HttpResponse>;
```

也就是说，`WFHttpServer` 是 `WFServer` 针对 HTTP 请求和 HTTP 响应的类型别名。

### 4.2 服务端处理函数

`WFHttpServer` 构造函数接收一个处理函数：

```cpp
[](WFHttpTask *task) {
    // ...
}
```

每收到一个 HTTP 请求，workflow 创建一个 `WFHttpTask`，并调用这个处理函数。

在服务端任务中：

```cpp
HttpRequest *req = task->get_req();
HttpResponse *resp = task->get_resp();
```

- `req` 表示客户端发来的 HTTP 请求。
- `resp` 表示将要返回给客户端的 HTTP 响应。

本示例只设置响应体：

```cpp
resp->append_output_body("<html>Hello World!</html>");
```

### 4.3 `append_output_body()`

`append_output_body()` 来自 `HttpMessage`，用于追加要发送的 body。

常见写法：

```cpp
resp->append_output_body("<html>Hello World!</html>");
```

也可以追加带长度的字节序列：

```cpp
resp->append_output_body(buf, size);
```

> [!IMPORTANT]
> `append_output_body(const char*)` 会按 C 字符串处理，依赖 `strlen()` 计算长度。如果 body 是二进制数据或包含 `'\0'`，应该使用 `append_output_body(buf, size)`。

`01_hello_workflow.cc` 的服务端处理流程很短，所有请求都进入构造 `WFHttpServer` 时传入的回调：

```mermaid
sequenceDiagram
    participant C as Client
    participant S as WFHttpServer
    participant T as WFHttpTask
    participant R as HttpResponse

    C->>S: HTTP 请求
    S->>T: 为本次请求创建服务端任务
    S->>T: 调用 server callback
    T->>R: task->get_resp()
    R->>R: append_output_body("<html>Hello World!</html>")
    S-->>C: 发送 HTTP 响应
```

### 4.4 启动服务

源码：

```cpp
if (server.start(8888) == 0) {
    getchar();
    server.stop();
} else {
    cerr << "ERROR: Server start FAILED!" << endl;
    exit(1);
}
```

要点：

- `server.start(8888)` 监听 8888 端口。
- 返回 `0` 表示启动成功。
- `getchar()` 保持进程不退出。
- `server.stop()` 优雅停止服务器。
- 启动失败可能是端口被占用、权限不足、网络环境异常。

测试命令：

```bash
curl -i http://127.0.0.1:8888/
```

> [!NOTE]
> 本示例没有显式设置 `Content-Type`。浏览器仍可能显示 HTML，但真实项目更推荐设置响应头，例如 `resp->set_header_pair("Content-Type", "text/html; charset=utf-8")`。

## 5. `02_fetch_baidu.cc`：HTTP 客户端任务

### 5.1 创建 HTTP 任务

源码：

```cpp
WFHttpTask* task = WFTaskFactory::create_http_task(
    "http://www.baidu.com",
    3,
    3,
    http_callback
);
```

`create_http_task()` 定义在：

```cpp
#include <workflow/WFTaskFactory.h>
```

参数：

| 参数 | 示例值 | 含义 |
| --- | --- | --- |
| `url` | `"http://www.baidu.com"` | 目标资源 URL。 |
| `redirect_max` | `3` | 最大自动重定向次数。 |
| `retry_max` | `3` | 网络失败后的最大重试次数。 |
| `callback` | `http_callback` | 任务完成时调用的函数。 |

### 5.2 设置 HTTP 请求

源码：

```cpp
HttpRequest* req = task->get_req();

req->set_method("GET");
req->set_header_pair("User-Agent", "WorkflowHttpClient");
req->set_header_pair("Connection", "close");
```

相关接口：

| 接口 | 作用 |
| --- | --- |
| `set_method("GET")` | 设置请求方法。 |
| `set_request_uri("/")` | 设置请求 URI，示例中注释掉了，默认由 URL 推导。 |
| `set_header_pair(name, value)` | 设置或覆盖请求头。 |
| `add_header_pair(name, value)` | 添加请求头。 |
| `append_output_body(data)` | 添加请求体，POST/PUT 时常用。 |

`User-Agent` 用于告诉服务器客户端身份。某些服务器会根据 `User-Agent` 返回不同内容，或者拒绝缺失 `User-Agent` 的请求。

`Connection: close` 表示请求结束后关闭 TCP 连接，不保持长连接。

> [!NOTE]
> HTTP GET 是常见默认方法，但教学代码显式调用 `set_method("GET")` 有助于看清请求构造过程。

### 5.3 启动任务并等待

源码：

```cpp
task->start();
waitGroup.wait();
```

执行流程：

1. `start()` 提交异步任务。
2. workflow 在后台执行 DNS、连接、发送请求、接收响应。
3. 任务完成后调用 `http_callback()`。
4. 回调里调用 `waitGroup.done()`。
5. 主线程从 `waitGroup.wait()` 返回。

`02_fetch_baidu.cc` 的实际执行时序如下：

```mermaid
sequenceDiagram
    participant M as main
    participant T as WFHttpTask
    participant B as www.baidu.com
    participant C as http_callback
    participant W as WaitGroup

    M->>T: create_http_task(url, 3, 3, callback)
    M->>T: get_req()
    M->>T: 设置 GET 方法
    M->>T: 设置 User-Agent 请求头
    M->>T: 设置 Connection close
    M->>T: start()
    M->>W: wait()
    T->>B: 发送 HTTP GET
    B-->>T: 返回 HTTP 响应
    T->>C: 执行回调
    C->>C: get_state / get_error
    C->>C: 打印响应行、响应头、响应体
    C->>W: done()
    W-->>M: wait 返回，main 退出
```

### 5.4 HTTP 回调中的状态检查

源码：

```cpp
int state = task->get_state();
if (state != WFT_STATE_SUCCESS) {
    cerr << WFGlobal::get_error_string(state, task->get_error()) << endl;
    waitGroup.done();
    return;
}
```

必须先检查状态。只有状态成功时，读取 `task->get_resp()` 才有业务意义。

常见失败原因：

- DNS 解析失败。
- TCP 连接失败。
- 连接或响应超时。
- SSL 错误。
- 任务被取消。

### 5.5 解析 HTTP 响应行

源码：

```cpp
HttpResponse* resp = task->get_resp();

cout << resp->get_http_version() << " "
     << resp->get_status_code() << " "
     << resp->get_reason_phrase() << "\r\n";
```

响应行格式：

```text
<HTTP版本号> <状态码> <原因短语>
```

例如：

```text
HTTP/1.1 200 OK
```

接口含义：

| 接口 | 含义 |
| --- | --- |
| `get_http_version()` | HTTP 版本。 |
| `get_status_code()` | 状态码字符串。 |
| `get_reason_phrase()` | 原因短语。 |

> [!CAUTION]
> `get_status_code()` 返回的是字符串形式，例如 `"200"`。如果要做数值比较，应转换成整数或使用字符串比较时保持类型一致。

### 5.6 遍历 HTTP 响应头

源码：

```cpp
HttpHeaderCursor cursor(resp);
string name;
string value;

while (cursor.next(name, value)) {
    cout << name << ": " << value << "\r\n";
}
```

`HttpHeaderCursor` 定义在：

```cpp
#include <workflow/HttpUtil.h>
```

作用：

- 遍历 HTTP request/response 的 header。
- `next(name, value)` 成功时返回 `true`。
- 没有更多 header 时返回 `false`。

常见响应头：

| Header | 含义 |
| --- | --- |
| `Content-Type` | 响应体类型。 |
| `Content-Length` | 响应体长度。 |
| `Location` | 重定向目标。 |
| `Set-Cookie` | 服务端设置 cookie。 |
| `Connection` | 连接策略。 |

> [!IMPORTANT]
> HTTP header 名大小写不敏感。业务逻辑不要依赖 `Content-Type`、`content-type` 这类大小写形式。

### 5.7 读取 HTTP 响应体

源码：

```cpp
const void* body;
size_t size;
resp->get_parsed_body(&body, &size);
cout << static_cast<const char*>(body) << endl;
```

`get_parsed_body()` 来自 `HttpMessage`：

```cpp
bool get_parsed_body(const void **body, size_t *size) const;
```

它返回一段已解析的 body 内存：

- `body`：起始地址。
- `size`：字节数。

更安全的文本输出方式：

```cpp
cout.write(static_cast<const char *>(body), size);
cout << endl;
```

> [!CAUTION]
> 示例中的 `cout << static_cast<const char*>(body)` 假设响应体是以 `'\0'` 结尾的文本。HTTP body 本质是字节序列，不保证以 `'\0'` 结尾，也可能是图片、压缩数据等二进制内容。严谨代码应使用 `size`。

## 6. `practice/01_wget.cc`：保存 HTTP 响应到文件

### 6.1 命令行参数

源码：

```cpp
int main(int argc, char* argv[]) {
    if (argc != 3) {
        cerr << "用法：" << argv[0] << " <URL> <文件名>" << endl;
        return 1;
    }

    char* url = argv[1];
    char* filename = argv[2];
}
```

参数含义：

- `argc`：命令行参数个数。
- `argv[0]`：程序名。
- `argv[1]`：URL。
- `argv[2]`：输出文件名。

运行示例：

```bash
./a.out http://www.baidu.com index.html
```

### 6.2 使用 `task->user_data` 传递上下文

源码：

```cpp
task->user_data = filename;
```

回调中取出：

```cpp
const char* filename = static_cast<const char*>(task->user_data);
```

`user_data` 是 workflow 任务对象上的 `void *` 字段，可以传递用户自定义上下文。

特点：

- 类型是 `void*`，需要自己转换回原类型。
- 不负责内存所有权管理。
- 常用于把文件名、业务对象指针、上下文结构体传给回调。

> [!IMPORTANT]
> `user_data` 只保存指针，不会复制对象，也不会延长对象生命周期。传进去的地址必须在回调执行时仍然有效。

本示例中：

```cpp
char* filename = argv[2];
task->user_data = filename;
```

`argv` 在整个 `main()` 生命周期内有效，而 `main()` 会等待任务完成，所以这里可以使用。

如果要传复杂数据，推荐定义结构体并明确管理生命周期：

```cpp
struct Context {
    std::string filename;
};
```

`practice/01_wget.cc` 中，命令行参数、`user_data` 和响应写文件的关系如下：

```mermaid
flowchart TD
    A[main 读取 argv[1] URL] --> B[create_http_task]
    C[main 读取 argv[2] 文件名] --> D[task->user_data = filename]
    B --> E[设置 GET 请求和请求头]
    E --> F[task->start]
    F --> G[http_callback]
    D --> G
    G --> H[static_cast 取回文件名]
    G --> I[task->get_resp 取响应]
    H --> J[ofstream 以 binary 打开]
    I --> K[写响应行和响应头]
    I --> L[get_parsed_body 取 body 和 size]
    L --> M[outfile.write 按 size 写入]
    K --> N[waitGroup.done]
    M --> N
```

### 6.3 二进制写文件

源码：

```cpp
ofstream outfile(filename, ios::binary);
```

`ios::binary` 表示以二进制模式打开文件。

为什么重要：

- 文本模式可能在某些平台做换行转换。
- HTTP 响应体可能是图片、压缩包、可执行文件等二进制数据。
- 二进制模式可以尽量原样写入字节。

写入 body：

```cpp
outfile.write(static_cast<const char*>(body), size);
```

`write()` 按给定长度写入，不依赖 `'\0'` 结束符，适合二进制数据。

> [!IMPORTANT]
> 保存 HTTP body 时应该用 `outfile.write(ptr, size)`，不要用 `outfile << static_cast<const char*>(body)`。后者按 C 字符串写入，遇到 `'\0'` 会截断。

### 6.4 响应行、响应头和响应体

practice 代码把响应行、响应头、响应体都写入同一个文件：

```cpp
outfile << resp->get_http_version() << " "
        << resp->get_status_code() << " "
        << resp->get_reason_phrase() << "\r\n";

while (cursor.next(name, value)) {
    outfile << name << ": " << value << "\r\n";
}

outfile.write(static_cast<const char*>(body), size);
```

这里有一个协议细节：响应头和响应体之间应该写入空行：

```cpp
outfile << "\r\n";
```

源码中写的是：

```cpp
cout << "\r\n";
```

这只向终端输出空行，并没有写入文件。

> [!CAUTION]
> 如果目标是保存完整 HTTP 报文，应该把头部和主体之间的空行写入 `outfile`。如果目标只是保存网页内容，通常只需要写 body，不需要写响应行和响应头。

## 7. `03_mysql_insert.cc`：MySQL INSERT 任务

### 7.1 创建 MySQL 任务

源码：

```cpp
WFMySQLTask* task = WFTaskFactory::create_mysql_task(
    "mysql://root:123456@localhost:3306/demo",
    3,
    mysql_callback
);
```

参数：

| 参数 | 含义 |
| --- | --- |
| MySQL URL | 连接信息。 |
| `retry_max` | 最大重试次数。 |
| `mysql_callback` | MySQL 任务完成后的回调函数。 |

MySQL URL 结构：

```text
mysql://<user>:<password>@<host>:<port>/<database>
```

示例：

```text
mysql://root:123456@localhost:3306/demo
```

拆解：

| 部分 | 值 |
| --- | --- |
| 用户名 | `root` |
| 密码 | `123456` |
| 主机 | `localhost` |
| 端口 | `3306` |
| 数据库 | `demo` |

> [!CAUTION]
> 示例把数据库密码直接写在源码中，只适合学习。真实项目应使用配置文件、环境变量或密钥管理系统，并避免把密码提交到版本库。

### 7.2 设置 SQL

源码：

```cpp
protocol::MySQLRequest* req = task->get_req();
string sql = "INSERT INTO tbl_user (username, password, salt) VALUES ('ls', 'abc123', 'very high')";
req->set_query(sql);
```

`MySQLRequest::set_query()` 用于设置要执行的 SQL。

支持形式：

```cpp
void set_query(const char *query);
void set_query(const std::string& query);
void set_query(const char *query, size_t length);
```

本示例执行 INSERT，属于 DML。

常见 SQL 分类：

| 分类 | 示例 | 含义 |
| --- | --- | --- |
| DDL | `CREATE TABLE`、`ALTER TABLE` | 定义表结构。 |
| DML | `INSERT`、`UPDATE`、`DELETE` | 修改数据。 |
| DQL | `SELECT` | 查询数据。 |
| DCL | `GRANT`、`REVOKE` | 权限控制。 |

### 7.3 MySQL 回调状态检查

源码：

```cpp
int state = task->get_state();
if (state != WFT_STATE_SUCCESS) {
    cerr << WFGlobal::get_error_string(state, task->get_error()) << endl;
    waitGroup.done();
    return;
}
```

这一步检查的是 workflow 任务层状态，例如：

- 是否连上 MySQL。
- 网络是否失败。
- 协议处理是否失败。

任务成功后，还要继续检查 MySQL 响应包。

### 7.4 检查 MySQL error packet

源码：

```cpp
MySQLResponse* resp = task->get_resp();
if (resp->get_packet_type() == MYSQL_PACKET_ERROR) {
    cerr << "error_code: " << resp->get_error_code()
         << ", error_msg: " << resp->get_error_msg() << endl;
    waitGroup.done();
    return;
}
```

MySQL 执行 SQL 后可能返回错误包，例如：

- SQL 语法错误。
- 表不存在。
- 字段不存在。
- 违反唯一约束。
- 权限不足。

相关接口：

| 接口 | 含义 |
| --- | --- |
| `get_packet_type()` | 获取 MySQL 响应包类型。 |
| `get_error_code()` | 获取 MySQL 错误码。 |
| `get_error_msg()` | 获取错误消息。 |
| `get_sql_state()` | 获取 SQL state。 |

> [!IMPORTANT]
> MySQL 任务成功不代表 SQL 成功。必须先检查 `task->get_state()`，再检查 `resp->get_packet_type()` 是否为 `MYSQL_PACKET_ERROR`。

### 7.5 处理 DML 结果

源码：

```cpp
MySQLResultCursor cursor(resp);
if (cursor.get_cursor_status() == MYSQL_STATUS_OK) {
    unsigned long long rows = cursor.get_affected_rows();
    cout << rows << "rows affected" << endl;

    unsigned long long id = cursor.get_insert_id();
    cout << "insert id: " << id << endl;
}
```

`MySQLResultCursor` 用于访问 MySQL 响应结果。

对于 INSERT、UPDATE、DELETE 这类 DML：

- `MYSQL_STATUS_OK` 表示操作成功，返回的是 OK 结果。
- `get_affected_rows()` 获取受影响行数。
- `get_insert_id()` 获取自增主键生成的 id。

> [!NOTE]
> `get_insert_id()` 只有在表存在自增字段且本次 INSERT 产生自增值时才有明显意义，否则可能为 `0`。

`03_mysql_insert.cc` 的 INSERT 任务处理链路如下：

```mermaid
sequenceDiagram
    participant M as main
    participant T as WFMySQLTask
    participant DB as MySQL Server
    participant C as mysql_callback
    participant W as WaitGroup

    M->>T: create_mysql_task(mysql_url, 3, callback)
    M->>T: get_req()
    M->>T: 设置 INSERT SQL
    M->>T: start()
    M->>W: wait()
    T->>DB: 发送 SQL
    DB-->>T: 返回 MySQLResponse
    T->>C: 执行回调
    C->>C: get_state 检查任务层状态
    C->>C: get_packet_type 检查 MySQL 错误包
    C->>C: MySQLResultCursor 读取 affected_rows/insert_id
    C->>W: done()
    W-->>M: wait 返回，main 退出
```

## 8. `04_mysql_select.cc`：MySQL SELECT 结果集

### 8.1 信号处理

源码：

```cpp
signal(SIGINT, sig_handler);

void sig_handler(int)
{
    waitGroup.done();
}
```

`SIGINT` 通常来自 Ctrl-C。注册信号处理函数后，用户按 Ctrl-C 时会调用 `sig_handler()`。

本示例中 `sig_handler()` 调用 `waitGroup.done()`，用于解除 `waitGroup.wait()` 的阻塞。

> [!CAUTION]
> 严格来说，信号处理函数中能安全调用的函数非常有限。教学代码用它解除阻塞便于演示；真实项目中应使用更稳妥的退出机制。

根据实际代码，SELECT 示例的退出机制和 INSERT 示例不同：回调负责打印结果，但没有调用 `waitGroup.done()`，程序需要 Ctrl-C 触发信号处理函数后退出。

```mermaid
sequenceDiagram
    participant M as main
    participant T as WFMySQLTask
    participant DB as MySQL Server
    participant C as mysql_callback
    participant W as WaitGroup
    participant U as User

    M->>W: WaitGroup(1)
    M->>M: signal(SIGINT, sig_handler)
    M->>T: create_mysql_task
    M->>T: 设置 SELECT SQL
    M->>T: start()
    M->>W: wait()
    T->>DB: 发送 SELECT
    DB-->>T: 返回结果集
    T->>C: 执行 mysql_callback
    C->>C: 检查 state 和 packet type
    C->>C: fetch_row 循环打印 MySQLCell
    Note over C,W: 实际代码没有调用 waitGroup.done()
    U->>M: Ctrl-C
    M->>W: sig_handler 调用 done()
    W-->>M: wait 返回，程序退出
```

### 8.2 执行 SELECT

源码：

```cpp
string sql = "SELECT * FROM tbl_user";
MySQLRequest* req = task->get_req();
req->set_query(sql);
```

SELECT 属于 DQL，会返回结果集。

处理结果集：

```cpp
MySQLResultCursor cursor(resp);
if (cursor.get_cursor_status() == MYSQL_STATUS_GET_RESULT) {
    // 遍历结果集
}
```

`MYSQL_STATUS_GET_RESULT` 表示当前 cursor 指向一个查询结果集。

### 8.3 获取字段数与行数

源码：

```cpp
cout << "fields: " << cursor.get_field_count() << endl;
cout << "rows: " << cursor.get_rows_count() << endl;
```

含义：

- `get_field_count()`：字段数量，也就是列数。
- `get_rows_count()`：行数量。

例如 `SELECT id, username FROM tbl_user`：

- 字段数为 2。
- 行数取决于表中匹配记录数量。

### 8.4 遍历每一行

源码：

```cpp
vector<MySQLCell> record;
while (cursor.fetch_row(record)) {
    for (const MySQLCell& cell : record) {
        display_cell(cell);
        cout << "\t";
    }
    cout << endl;
}
```

`fetch_row(record)` 每次读取一行：

- 成功读取一行返回 `true`。
- `record` 中保存这一行的所有单元格。
- 读完所有行后返回 `false`。

`record` 使用 `vector<MySQLCell>`，所以：

- 单元格顺序与 SELECT 返回字段顺序一致。
- 不直接包含字段名。

如果想按字段名访问，可以使用 workflow 提供的 map 版本：

```cpp
std::map<std::string, MySQLCell> row;
cursor.fetch_row(row);
```

### 8.5 `MySQLCell` 类型判断与转换

源码：

```cpp
void display_cell(const MySQLCell& cell)
{
    if (cell.is_null()) {
        cout << "(NULL)";
    } else if (cell.is_int()) {
        cout << cell.as_int();
    } else if (cell.is_ulonglong()) {
        cout << cell.as_ulonglong();
    } else if (cell.is_float()) {
        cout << cell.as_float();
    } else if (cell.is_double()) {
        cout << cell.as_double();
    } else if (cell.is_string()) {
        cout << cell.as_string();
    } else if (cell.is_date()) {
        cout << cell.as_date();
    } else if (cell.is_time()) {
        cout << cell.as_time();
    } else if (cell.is_datetime()) {
        cout << cell.as_datetime();
    }
}
```

`MySQLCell` 代表结果集中的一个单元格。

常用判断接口：

| 接口 | 含义 |
| --- | --- |
| `is_null()` | 是否为 SQL `NULL`。 |
| `is_int()` | 是否为整数。 |
| `is_ulonglong()` | 是否为无符号长长整型。 |
| `is_float()` | 是否为 float。 |
| `is_double()` | 是否为 double。 |
| `is_string()` | 是否为字符串或可作为字符串处理的类型。 |
| `is_date()` | 是否为 DATE。 |
| `is_time()` | 是否为 TIME。 |
| `is_datetime()` | 是否为 DATETIME 或 TIMESTAMP。 |

常用转换接口：

| 接口 | 返回值 |
| --- | --- |
| `as_int()` | `int` |
| `as_ulonglong()` | `unsigned long long` |
| `as_float()` | `float` |
| `as_double()` | `double` |
| `as_string()` | `std::string` |
| `as_date()` | `std::string` |
| `as_time()` | `std::string` |
| `as_datetime()` | `std::string` |

> [!IMPORTANT]
> 读取 `MySQLCell` 时要先判断类型再调用对应的 `as_xxx()`。类型不匹配时，workflow 的转换接口可能返回默认值或空字符串，容易掩盖数据问题。

`display_cell()` 对不同 MySQL 字段类型的分支判断可以理解为下面的活动图：

```mermaid
flowchart TD
    A[display_cell 接收 const MySQLCell&] --> B{is_null}
    B -->|是| C[输出 NULL]
    B -->|否| D{is_int}
    D -->|是| E[as_int]
    D -->|否| F{is_ulonglong}
    F -->|是| G[as_ulonglong]
    F -->|否| H{is_float}
    H -->|是| I[as_float]
    H -->|否| J{is_double}
    J -->|是| K[as_double]
    J -->|否| L{is_string}
    L -->|是| M[as_string]
    L -->|否| N{is_date}
    N -->|是| O[as_date]
    N -->|否| P{is_time}
    P -->|是| Q[as_time]
    P -->|否| R{is_datetime}
    R -->|是| S[as_datetime]
```

### 8.6 `const MySQLCell&` 的意义

源码：

```cpp
void display_cell(const MySQLCell& cell)
```

这里使用 `const` 引用传参：

- 避免拷贝 `MySQLCell`，效率更高。
- `const` 表示函数不会修改 cell。
- 适合只读展示逻辑。

遍历时也使用：

```cpp
for (const MySQLCell& cell : record)
```

这同样避免拷贝每个单元格。

## 9. MySQL cursor 状态与包类型

### 9.1 MySQL 包类型

`mysql_types.h` 中定义了包类型：

| 包类型 | 含义 |
| --- | --- |
| `MYSQL_PACKET_OK` | OK 包，常见于 INSERT/UPDATE/DELETE 成功。 |
| `MYSQL_PACKET_ERROR` | 错误包。 |
| `MYSQL_PACKET_GET_RESULT` | 查询结果集。 |
| `MYSQL_PACKET_EOF` | EOF 包。 |
| `MYSQL_PACKET_NULL` | NULL 包。 |
| `MYSQL_PACKET_OTHER` | 其他包。 |

示例中主要用：

```cpp
resp->get_packet_type() == MYSQL_PACKET_ERROR
```

### 9.2 MySQL cursor 状态

`mysql_types.h` 中定义了 cursor 状态：

| 状态 | 含义 |
| --- | --- |
| `MYSQL_STATUS_NOT_INIT` | 未初始化。 |
| `MYSQL_STATUS_OK` | OK 结果，适合 DML。 |
| `MYSQL_STATUS_GET_RESULT` | 查询结果集，适合 SELECT。 |
| `MYSQL_STATUS_ERROR` | cursor 解析错误。 |
| `MYSQL_STATUS_END` | 结果集读取结束。 |

使用方式：

```cpp
if (cursor.get_cursor_status() == MYSQL_STATUS_OK) {
    // DML
}

if (cursor.get_cursor_status() == MYSQL_STATUS_GET_RESULT) {
    // DQL
}
```

> [!NOTE]
> DML 和 DQL 的结果结构不同：DML 关注受影响行数和插入 id，DQL 关注字段数、行数和每一行的单元格。

MySQL 任务成功后，还需要继续按包类型和 cursor 状态分流：

```mermaid
flowchart TD
    A[WFMySQLTask 回调] --> B{get_state 成功?}
    B -->|否| C[任务层错误<br/>打印 error string]
    B -->|是| D[get_resp]
    D --> E{packet_type 是 MYSQL_PACKET_ERROR?}
    E -->|是| F[SQL 或约束错误<br/>get_error_code/get_error_msg]
    E -->|否| G[创建 MySQLResultCursor]
    G --> H{cursor status}
    H -->|MYSQL_STATUS_OK| I[DML 成功<br/>affected_rows / insert_id]
    H -->|MYSQL_STATUS_GET_RESULT| J[DQL 结果集<br/>field_count / rows_count / fetch_row]
    H -->|MYSQL_STATUS_ERROR| K[结果游标错误]
    H -->|MYSQL_STATUS_END| L[结果集结束]
```

## 10. C++ 标准库知识点

### 10.1 `iostream`

示例中常用：

```cpp
cout << "message" << endl;
cerr << "error" << endl;
```

- `cout`：标准输出。
- `cerr`：标准错误输出，适合错误日志。
- `endl`：输出换行并刷新缓冲区。

### 10.2 `std::string`

用于保存 SQL、header 名和值等：

```cpp
string sql = "SELECT * FROM tbl_user";
```

`set_query(sql)`、`set_header_pair(name, value)` 都支持 `std::string`。

### 10.3 `std::vector`

MySQL SELECT 中：

```cpp
vector<MySQLCell> record;
```

用于保存一行记录的所有单元格。

特点：

- 连续存储。
- 按下标访问方便。
- 遍历顺序与插入顺序一致。

### 10.4 `std::ofstream`

wget 练习中：

```cpp
ofstream outfile(filename, ios::binary);
```

用途：

- 创建或打开输出文件。
- 用 `<<` 写文本。
- 用 `write(ptr, size)` 写二进制。

检查文件是否打开成功：

```cpp
if (!outfile) {
    cerr << "错误：无法打开或创建文件" << filename << endl;
}
```

### 10.5 `static_cast`

示例：

```cpp
const char* filename = static_cast<const char*>(task->user_data);
outfile.write(static_cast<const char*>(body), size);
```

`static_cast` 用于显式类型转换。

这里的转换：

- `void*` 转回 `const char*`。
- `const void*` 转成 `const char*` 以便按字节写入。

> [!CAUTION]
> `static_cast` 不会检查指针真实指向的对象类型。转换类型必须和原始存入的类型一致，否则会产生未定义行为。

## 11. 接口速查

### 11.1 `WFHttpServer`

| 接口 | 作用 |
| --- | --- |
| `WFHttpServer(process)` | 创建 HTTP 服务端。 |
| `start(port)` | 启动并监听端口。 |
| `stop()` | 停止服务端。 |

### 11.2 `WFTaskFactory`

| 接口 | 作用 |
| --- | --- |
| `create_http_task(url, redirect_max, retry_max, callback)` | 创建 HTTP 客户端任务。 |
| `create_mysql_task(url, retry_max, callback)` | 创建 MySQL 任务。 |

### 11.3 `WFHttpTask`

| 接口/字段 | 作用 |
| --- | --- |
| `get_req()` | 获取 `HttpRequest`。 |
| `get_resp()` | 获取 `HttpResponse`。 |
| `start()` | 启动客户端任务。 |
| `get_state()` | 获取任务状态。 |
| `get_error()` | 获取错误码。 |
| `user_data` | 用户自定义上下文指针。 |

### 11.4 `HttpRequest`

| 接口 | 作用 |
| --- | --- |
| `set_method(method)` | 设置请求方法。 |
| `set_request_uri(uri)` | 设置请求 URI。 |
| `set_header_pair(name, value)` | 设置 header。 |
| `append_output_body(data)` | 添加请求体。 |

### 11.5 `HttpResponse`

| 接口 | 作用 |
| --- | --- |
| `get_http_version()` | 获取 HTTP 版本。 |
| `get_status_code()` | 获取状态码。 |
| `get_reason_phrase()` | 获取原因短语。 |
| `get_parsed_body(&body, &size)` | 获取响应体地址和长度。 |
| `append_output_body(data)` | 服务端追加响应体。 |
| `set_header_pair(name, value)` | 设置响应头。 |

### 11.6 `HttpHeaderCursor`

| 接口 | 作用 |
| --- | --- |
| `next(name, value)` | 遍历下一个 header。 |
| `find(name, value)` | 查找指定 header。 |
| `erase()` | 删除当前 header。 |
| `rewind()` | 回到遍历起点。 |

### 11.7 `WFMySQLTask`

| 接口/字段 | 作用 |
| --- | --- |
| `get_req()` | 获取 `MySQLRequest`。 |
| `get_resp()` | 获取 `MySQLResponse`。 |
| `start()` | 启动任务。 |
| `get_state()` | 获取任务状态。 |
| `get_error()` | 获取错误码。 |
| `user_data` | 用户自定义上下文指针。 |

### 11.8 `MySQLRequest`

| 接口 | 作用 |
| --- | --- |
| `set_query(sql)` | 设置要执行的 SQL。 |
| `get_query()` | 获取已设置的 SQL。 |
| `query_is_unset()` | 判断 SQL 是否未设置。 |

### 11.9 `MySQLResponse`

| 接口 | 作用 |
| --- | --- |
| `get_packet_type()` | 获取响应包类型。 |
| `is_ok_packet()` | 是否 OK 包。 |
| `is_error_packet()` | 是否错误包。 |
| `get_error_code()` | 获取错误码。 |
| `get_error_msg()` | 获取错误信息。 |
| `get_affected_rows()` | 获取受影响行数。 |
| `get_last_insert_id()` | 获取最后插入 id。 |

### 11.10 `MySQLResultCursor`

| 接口 | 作用 |
| --- | --- |
| `get_cursor_status()` | 获取 cursor 状态。 |
| `get_field_count()` | 获取字段数。 |
| `get_rows_count()` | 获取行数。 |
| `fetch_row(vector<MySQLCell>&)` | 读取一行到 vector。 |
| `fetch_row(map<string, MySQLCell>&)` | 读取一行到 map。 |
| `fetch_all(rows)` | 读取所有行。 |
| `get_affected_rows()` | 获取 DML 受影响行数。 |
| `get_insert_id()` | 获取插入 id。 |
| `rewind()` | 重置 cursor 位置。 |

### 11.11 `MySQLCell`

| 接口 | 作用 |
| --- | --- |
| `is_null()` | 判断 NULL。 |
| `is_int()` | 判断 int。 |
| `is_ulonglong()` | 判断 unsigned long long。 |
| `is_float()` | 判断 float。 |
| `is_double()` | 判断 double。 |
| `is_string()` | 判断字符串。 |
| `is_date()` | 判断 DATE。 |
| `is_time()` | 判断 TIME。 |
| `is_datetime()` | 判断 DATETIME/TIMESTAMP。 |
| `as_int()` | 转 int。 |
| `as_ulonglong()` | 转 unsigned long long。 |
| `as_float()` | 转 float。 |
| `as_double()` | 转 double。 |
| `as_string()` | 转 string。 |
| `as_date()` | 转 date 字符串。 |
| `as_time()` | 转 time 字符串。 |
| `as_datetime()` | 转 datetime 字符串。 |

## 12. 编译与运行参考

如果系统已安装 workflow 库，单文件可以类似这样编译：

```bash
g++ 01_hello_workflow.cc -o server -lworkflow -lpthread
```

MySQL 示例通常还需要链接 workflow 依赖的 MySQL/SSL 等库，具体命令取决于本机安装方式。

运行 HTTP 服务端：

```bash
./server
curl -i http://127.0.0.1:8888/
```

运行 HTTP 客户端：

```bash
./a.out
```

运行 wget 练习：

```bash
./a.out http://www.baidu.com index.html
```

运行 MySQL 示例前，需要确认：

- MySQL 服务已启动。
- 用户名、密码、端口、数据库名正确。
- 数据库 `demo` 存在。
- 表 `tbl_user` 存在。
- 表字段包含 `username`、`password`、`salt`。

可以参考建表语句：

```sql
CREATE DATABASE IF NOT EXISTS demo;

USE demo;

CREATE TABLE IF NOT EXISTS tbl_user (
    id BIGINT UNSIGNED PRIMARY KEY AUTO_INCREMENT,
    username VARCHAR(64) NOT NULL,
    password VARCHAR(128) NOT NULL,
    salt VARCHAR(128) NOT NULL
);
```

> [!NOTE]
> 上面的 SQL 只是根据示例字段推导出的练习表结构，不代表项目中唯一正确的表结构。

## 13. 易错点总结

- 客户端任务 `start()` 不阻塞，必须用 `WaitGroup` 等机制等待。
- 回调里的成功路径和失败路径都要调用 `waitGroup.done()`。
- 任务状态成功不等于 HTTP 业务成功，也不等于 SQL 执行成功。
- HTTP body 是字节序列，不一定是 `'\0'` 结尾字符串。
- 保存响应体时应使用 `write(ptr, size)`，不要依赖 `operator<<` 写 C 字符串。
- `task->user_data` 是裸 `void*`，不负责类型安全和生命周期管理。
- MySQL 连接 URL 中明文密码只适合学习，不适合真实项目。
- 执行 MySQL 后要先检查 workflow 任务状态，再检查 MySQL error packet。
- DML 使用 `MYSQL_STATUS_OK`，DQL 使用 `MYSQL_STATUS_GET_RESULT`。
- `MySQLCell` 要先判断类型，再调用对应 `as_xxx()`。
- `signal()` 示例只适合教学演示，真实服务要设计更可靠的退出机制。

> [!IMPORTANT]
> 本章最重要的主线是 workflow 的异步任务生命周期：创建任务，设置 request，调用 `start()`，在 callback 中检查 `state/error`，再解析 response，最后通知等待方任务已完成。
