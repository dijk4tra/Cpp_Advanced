# 07_Wfrest 学习笔记

本文整理 `07_Wfrest` 目录中所有代码文件涉及到的 C++ 进阶知识点、wfrest/workflow 使用方式、HTTP 服务端开发模式、异步任务编排、文件上传下载、MySQL 异步访问、密码哈希、JWT 鉴权以及静态资源服务实现。

`07_Wfrest/www` 中的内容只作为静态资源演示使用，前端相关知识点只做必要说明。

## 目录代码概览

| 文件 | 主要内容 |
| --- | --- |
| `01_hello_wfrest.cc` | 最小 wfrest HTTP 服务、GET/POST 路由、字符串响应、优雅退出 |
| `02_register_routes.cc` | 静态路由、参数路由、通配符路由、路由路径信息 |
| `03_query_parameters.cc` | URL 查询参数解析、默认值、参数存在性判断 |
| `04_request_header.cc` | 请求头读取、请求头遍历、大小写无关头字段 |
| `05_request_body.cc` | 请求体、`x-www-form-urlencoded`、`multipart/form-data` 解析 |
| `06_upload_file.cc` | 文件上传、表单文件字段、文件名净化、异步保存文件 |
| `07_download_file.cc` | 文件下载、相对路径/绝对路径、范围读取 |
| `08_static_file_server.cc` | 静态资源映射、目录映射、首页映射 |
| `09_series_handler.cc` | wfrest `SeriesHandler`、向请求序列追加 Workflow 任务 |
| `10_mysql_example.cc` | wfrest MySQL 封装、默认 JSON 响应、自定义 MySQL 回调 |
| `Wfrest_Server/CryptoUtil.h` | `User` 数据结构、密码哈希与 JWT 工具类接口 |
| `Wfrest_Server/CryptoUtil.cc` | salt 生成、OpenSSL EVP 摘要、libjwt token 生成与验证 |
| `Wfrest_Server/wfrest_server_practice.cc` | 注册、登录、JWT 鉴权、受保护静态资源服务的练习版 |
| `Wfrest_Server/wfrest_static_resources_server.cc` | 更完整的注册/登录/鉴权静态资源服务器，包含简单 SQL 转义 |
| `Wfrest_Server/Makefile` | C++17 编译、wfrest/workflow/libjwt/OpenSSL 链接 |
| `resources/`、`www/`、`Wfrest_Server/resources/` | 文件下载、静态资源映射和受保护资源的测试文件 |

> [!NOTE]
> 本目录的学习主线是：用 wfrest 简化 Workflow HTTP server 的使用，同时保留 Workflow 的异步任务模型。也就是说，wfrest 不是同步阻塞框架，它的文件、MySQL、定时器等操作最终仍会以任务方式加入当前请求的 `SeriesWork`。

## wfrest 与 Workflow 的关系

wfrest 是基于 Workflow 的 C++ RESTful Web 框架。Workflow 提供底层网络、协议、异步任务、任务序列、文件 IO、MySQL、Redis 等能力；wfrest 在此基础上封装了更易用的 HTTP Server API。

本地头文件中可以看到：

- `wfrest::HttpServer` 继承自 `WFServer<HttpReq, HttpResp>`。
- `wfrest::Handler` 类型为 `std::function<void(const HttpReq *, HttpResp *)>`。
- `wfrest::SeriesHandler` 类型为 `std::function<void(const HttpReq *, HttpResp *, SeriesWork *)>`。
- `HttpReq` 继承自 `protocol::HttpRequest`，增加了路由参数、查询参数、表单、JSON、cookie 等解析接口。
- `HttpResp` 继承自 `protocol::HttpResponse`，增加了 `String()`、`Json()`、`File()`、`Save()`、`MySQL()`、`Redis()`、`Timer()`、`Compute()` 等高层响应接口。

```mermaid
flowchart TD
    A[客户端 HTTP 请求] --> B[workflow WFServer]
    B --> C[wfrest HttpServer]
    C --> D[BluePrint/Router 路由匹配]
    D --> E{Handler 类型}
    E -->|Handler| F[void req resp]
    E -->|SeriesHandler| G[void req resp series]
    F --> H[设置 HttpResp]
    G --> I[向 SeriesWork 追加任务]
    I --> H
    H --> J[workflow 发送 HTTP 响应]
```

> [!IMPORTANT]
> wfrest 的 `HttpReq *` 和 `HttpResp *` 由框架管理，通常只在当前请求处理链和它追加的异步回调中使用。不要把它们保存到全局变量，也不要在请求生命周期结束后继续访问。

## 通用服务端骨架

本目录大多数示例都采用同一个基本结构：

```cpp
static WFFacilities::WaitGroup waitGroup(1);

void sig_handler(int)
{
    waitGroup.done();
}

int main()
{
    signal(SIGINT, sig_handler);

    HttpServer server;
    server.GET("/hello", [](const HttpReq* req, HttpResp* resp) {
        resp->String("world\n");
    });

    if (server.start(8888) == 0) {
        waitGroup.wait();
        server.stop();
    } else {
        exit(1);
    }
}
```

这个骨架涉及几个核心点：

1. `signal(SIGINT, sig_handler)` 注册 Ctrl+C 信号处理函数。
2. `WFFacilities::WaitGroup waitGroup(1)` 用于阻塞主线程，避免 `main()` 结束导致服务退出。
3. 收到 SIGINT 后调用 `waitGroup.done()`。
4. `server.start(8888)` 返回 `0` 表示监听成功。
5. `server.stop()` 停止服务。

> [!NOTE]
> 这里的 `WaitGroup` 类似 Go 语言中的等待组。它不负责业务并发控制，只是让主线程等待某个事件发生。本目录中这个事件就是 Ctrl+C。

> [!CAUTION]
> 严格来说，POSIX 信号处理函数中可安全调用的函数有限。示例中直接调用 `waitGroup.done()` 是教学写法，实际生产项目通常会使用更稳妥的信号通知机制，例如 `signalfd`、pipe、自定义事件循环通知，或将信号处理逻辑限制在 async-signal-safe 操作内。

## `01_hello_wfrest.cc`：最小 HTTP 服务

示例注册了两个路由：

```cpp
server.GET("/hello", [](const HttpReq* req, HttpResp* resp) {
    resp->String("world\n");
});

server.POST("/echo", [](const HttpReq* req, HttpResp* resp) {
    resp->String(req->body());
});
```

### GET 路由

`server.GET(route, handler)` 注册 GET 方法。handler 签名为：

```cpp
void(const HttpReq* req, HttpResp* resp)
```

`req` 表示请求，`resp` 表示响应。通过 `resp->String()` 写入响应体。

### POST 路由

`server.POST("/echo", ...)` 读取请求体并原样返回：

```cpp
resp->String(req->body());
```

`req->body()` 返回请求体字符串引用。对于普通文本、JSON、表单原始内容都可以这样拿到原始 body。

> [!IMPORTANT]
> `req->body()` 只是读取原始请求体，不代表已经按 JSON 或表单格式解析。解析方式取决于 `Content-Type`，例如 `form_kv()` 解析 `application/x-www-form-urlencoded`，`form()` 解析 `multipart/form-data`。

## `02_register_routes.cc`：路由注册与匹配

wfrest 支持静态路由、参数路由和通配符路由。

### 静态路由

```cpp
server.GET("/user/xixi", [](const HttpReq* req, HttpResp* resp) {
    resp->String("...\n");
});
```

静态路由要求路径完全匹配。`/user/xixi` 和 `/user/peanut` 是不同路径。

### 参数路由

```cpp
server.GET("/user/{name}", [](const HttpReq* req, HttpResp* resp) {
    const string& name = req->param("name");
    resp->set_status(HttpStatusOK);
    resp->String("Hello " + name + "\n");
});
```

`{name}` 表示路径参数。请求 `/user/peanut` 时：

- 路由模板：`/user/{name}`
- 当前路径：`/user/peanut`
- 参数名：`name`
- 参数值：`peanut`

`req->param("name")` 获取路径参数。wfrest 还提供模板特化版本：

```cpp
int id = req->param<int>("id");
size_t n = req->param<size_t>("n");
double score = req->param<double>("score");
```

这些转换内部使用 `std::stoi`、`std::stoul`、`std::stod`。

> [!CAUTION]
> `param<int>()` 这类转换如果遇到非法数字字符串，可能抛出标准库异常。生产代码中需要在路由层或业务层做格式校验，不能默认用户输入一定合法。

### 通配符路由

```cpp
server.GET("/wildcard/*", [](const HttpReq* req, HttpResp* resp) {
    cout << "match_path: " << req->match_path() << endl;
    cout << "full_path: " << req->full_path() << endl;
    cout << "current_path: " << req->current_path() << endl;
});
```

常用路径接口：

| 接口 | 含义 |
| --- | --- |
| `req->match_path()` | `*` 匹配到的路径部分 |
| `req->full_path()` | 注册的路由模板 |
| `req->current_path()` | 客户端当前请求路径 |

例如请求 `/wildcard/a/b/c`，可能得到：

- `full_path()`：`/wildcard/*`
- `current_path()`：`/wildcard/a/b/c`
- `match_path()`：通配符匹配到的剩余路径

### 参数与通配符组合

```cpp
server.GET("/user/{name}/*", [](const HttpReq* req, HttpResp* resp) {
    string name = req->param("name");
    cout << req->match_path() << endl;
});
```

这适合实现用户空间下的任意资源路径，例如：

```text
/user/alice/avatar/2026/01.png
```

可以把 `alice` 作为用户标识，把后面的 `avatar/2026/01.png` 作为资源路径。

> [!IMPORTANT]
> 路由越宽泛，越需要注意注册顺序和匹配优先级。通配符路由通常应放在具体业务路由之后，否则可能提前吃掉更具体的路径。

## `03_query_parameters.cc`：查询参数

查询参数是 URL 中 `?` 后面的键值对，例如：

```text
/query?username=peanut&password=123456
```

### 遍历所有查询参数

```cpp
const map<string, string>& all_queries = req->query_list();
for (const auto& [name, value] : all_queries) {
    cout << name << ": " << value << endl;
}
```

这里用到了 C++17 结构化绑定：

```cpp
for (const auto& [key, value] : map_obj) {
    ...
}
```

`query_list()` 返回 `const map<string, string>&`，说明调用方不能修改解析后的查询参数。

### 获取单个查询参数

```cpp
const string& username = req->query("username");
const string& password = req->query("password");
const string& info = req->query("info");
```

如果参数不存在，`req->query()` 返回空字符串。

### 查询参数默认值

```cpp
const string& address = req->default_query("address", "china");
```

当 `address` 不存在时，返回默认值 `"china"`。

### 判断参数是否存在

示例中指出：

```cpp
// URL: /has_query?username=peanut&password=
if (req->has_query("password")) {
    cout << "存在参数 password" << endl;
}
```

`password=` 表示参数存在，但值为空。如果只判断 `req->query("password").empty()`，会误以为参数不存在。

> [!IMPORTANT]
> HTTP 查询参数中，“不存在”和“存在但为空”是两个不同状态。需要区分时使用 `has_query()`，不要只靠字符串是否为空判断。

## `04_request_header.cc`：请求头处理

### 获取指定请求头

```cpp
const string& host = req->header("Host");
const string& contentType = req->header("Content-Type");
const string& xyz = req->header("xyz");
```

如果请求头不存在，`header()` 返回空字符串。

### 判断请求头是否存在

```cpp
if (req->has_header("Host")) {
    cout << req->header("Host") << endl;
}
```

和查询参数一样，头字段也需要区分“不存在”和“存在但值为空”。

### 遍历所有请求头

wfrest 没有直接提供遍历头字段的专用封装，示例借助 Workflow 的 `protocol::HttpHeaderCursor`：

```cpp
HttpHeaderCursor cursor(req);
string name;
string value;
while (cursor.next(name, value)) {
    cout << name << ": " << value << endl;
}
```

`HttpHeaderCursor` 来自 `workflow/HttpUtil.h`，可以遍历底层 HTTP message 的头字段。

> [!NOTE]
> HTTP 头字段名大小写不敏感。wfrest 的内部头字段 map 使用了大小写无关比较器，因此 `Host`、`host`、`HOST` 在语义上应作为同一个头字段处理。

## `05_request_body.cc`：请求体与表单解析

HTTP 请求体常见格式包括：

- 纯文本：`text/plain`
- JSON：`application/json`
- URL 编码表单：`application/x-www-form-urlencoded`
- 多部分表单：`multipart/form-data`

### 原始请求体

```cpp
string& body = req->body();
cout << body << endl;
```

这种方式只拿原始 body，不做格式解析。

### `application/x-www-form-urlencoded`

客户端请求示例：

```bash
curl -v http://127.0.0.1:8888/form-urlencoded \
  -H "Content-Type: application/x-www-form-urlencoded" \
  -d 'user=admin&password=123456'
```

服务端处理：

```cpp
if (req->content_type() != APPLICATION_URLENCODED) {
    resp->set_status(HttpStatusBadRequest);
    return;
}

map<string, string>& form = req->form_kv();
for (const auto& [key, value] : form) {
    cout << key << ": " << value << endl;
}
```

关键点：

- `content_type()` 返回 `wfrest::http_content_type` 枚举。
- `APPLICATION_URLENCODED` 对应 MIME `application/x-www-form-urlencoded`。
- `form_kv()` 返回解析后的键值表单。

### `multipart/form-data`

客户端请求示例：

```bash
curl -X POST http://127.0.0.1:8888/form-data \
  -F "file=@/path/file" \
  -H "Content-Type: multipart/form-data"
```

服务端处理：

```cpp
if (req->content_type() != MULTIPART_FORM_DATA) {
    resp->set_status(HttpStatusBadRequest);
    return;
}

const Form& formData = req->form();
for (const auto& [key, file] : formData) {
    cout << "key: " << key << endl;
    cout << "filename: " << file.first << endl;
    cout << "content: " << file.second << endl;
}
```

`Form` 的结构在注释中说明为：

```cpp
using Form = map<string, pair<string, string>>;
```

其中：

- `key`：表单字段名。
- `file.first`：上传文件名。
- `file.second`：上传文件内容。

> [!CAUTION]
> `multipart/form-data` 可能包含大文件。示例中直接把文件内容作为 `std::string` 放在内存里，适合学习和小文件演示。生产环境需要限制请求体大小、流式写入、检查磁盘空间，并处理并发上传带来的内存压力。

## `06_upload_file.cc`：文件上传与安全文件名

上传文件的核心流程：

```mermaid
flowchart TD
    A[POST /upload-file] --> B{Content-Type 是否 multipart/form-data}
    B -->|否| C[400 Bad Request]
    B -->|是| D[req->form 解析表单]
    D --> E[遍历每个上传字段]
    E --> F[取得 filename 和 content]
    F --> G[PathUtil::base 去掉路径]
    G --> H[resp->Save 异步保存]
    H --> I[返回保存成功消息]
```

### 文件上传接口

```cpp
server.POST("/upload-file", [](const HttpReq* req, HttpResp* resp) {
    if (req->content_type() != MULTIPART_FORM_DATA) {
        resp->set_status(HttpStatusBadRequest);
        return;
    }

    const Form& form = req->form();
    for (const auto& [key, file] : form) {
        const string& filename = file.first;
        const string& content = file.second;
        string basename = PathUtil::base(filename);
        resp->Save(basename, std::move(content), "Save " + basename + " Success\n");
    }
});
```

### `PathUtil::base()`

本地 `wfrest/PathUtil.h` 中提供：

```cpp
static std::string base(const std::string &filepath);
```

它用于从路径中取最后的文件名。例如：

```text
../../etc/passwd  -> passwd
/tmp/demo.txt     -> demo.txt
avatar.png        -> avatar.png
```

### 为什么不能信任上传文件名

浏览器或 curl 上传时的 filename 来自客户端，可能包含危险路径：

```text
../demo.txt
../../.ssh/authorized_keys
/etc/passwd
C:\Windows\system.ini
```

如果服务端直接执行：

```cpp
resp->Save(filename, std::move(content));
```

攻击者可能尝试覆盖服务器上的敏感文件，形成路径穿越问题。

> [!IMPORTANT]
> 上传文件名必须当作不可信输入。至少要取 basename、限制字符集、限制扩展名、限制保存目录，并避免覆盖已有关键文件。

### `resp->Save()`

`HttpResp` 提供多组 `Save()` 重载：

```cpp
void Save(const std::string &file_dst, const std::string &content);
void Save(const std::string &file_dst, std::string &&content);
void Save(const std::string &file_dst, std::string &&content, const std::string &notify_msg);
```

其底层会创建文件写任务并加入当前请求序列。示例使用右值引用版本：

```cpp
resp->Save(basename, std::move(content), "Save " + basename + " Success\n");
```

这里 `std::move(content)` 表达“内容可以被移动”，避免不必要复制。

> [!CAUTION]
> 示例中 `content` 声明为 `const string&`，对 `const string&` 调用 `std::move` 得到的是 `const string&&`，通常不能触发真正的移动构造，仍可能退化为复制。要真正移动，需要拥有一个非 const 的 `std::string` 对象。教学上重点是理解移动语义，但生产代码要注意对象所有权和 const 限制。

## `07_download_file.cc`：文件下载和范围读取

示例使用 `resp->File()` 返回文件：

```cpp
resp->File("/home/lws/my_project/Cpp_Advanced/07_Wfrest/a.txt");
resp->File("resources/b.txt");
resp->File("resources/a.txt", 6);
resp->File("resources/a.txt", 6, 11);
```

`HttpResp` 中的相关重载：

```cpp
void File(const std::string &path);
void File(const std::string &path, size_t start);
void File(const std::string &path, size_t start, size_t end);
```

### 相对路径与绝对路径

- 绝对路径：从文件系统根目录开始，例如 `/home/lws/.../a.txt`。
- 相对路径：从当前进程工作目录开始，例如 `resources/b.txt`。

示例注释中建议一般使用相对路径，因为相对路径更适合部署目录迁移，也不暴露服务器真实目录结构。

> [!CAUTION]
> 相对路径相对于“程序运行时的当前工作目录”，不是相对于源码文件所在目录。用 `./server`、`cd 07_Wfrest && ./a.out`、IDE 运行时，工作目录可能不同，导致文件找不到。

### 范围读取

```cpp
resp->File("resources/a.txt", 6);
resp->File("resources/a.txt", 6, 11);
```

可用于只返回文件的一部分。常见用途包括：

- 大文件分片下载。
- 断点续传。
- 视频、音频部分读取。
- 测试文件 IO 偏移。

> [!NOTE]
> `resp->File()` 是服务端主动按指定范围读取文件；HTTP 标准中的 Range 请求还涉及客户端 `Range` 头、服务端 `206 Partial Content`、`Content-Range` 等语义。学习时要区分“框架文件读取范围参数”和“完整 HTTP Range 协议支持”。

## `08_static_file_server.cc`：静态资源服务

示例展示两种方式。

### 手动通配符路由方式

代码中注释掉的版本：

```cpp
server.GET("/static/*", [](const HttpReq* req, HttpResp* resp) {
    string parent = "./www/static/";
    string file = req->match_path();
    resp->File(parent + file);
});
```

这是静态文件服务的基本原理：

1. 用通配符匹配 URL 中的资源路径。
2. 把资源路径拼接到静态目录。
3. 用 `resp->File()` 读取并返回文件。

这种方式可以帮助理解静态资源服务器本质，但需要自己处理：

- 路径穿越。
- MIME 类型。
- 文件不存在。
- 目录请求。
- 缓存头。
- Range 请求。
- 权限控制。

### `server.Static()` 方式

示例实际使用：

```cpp
server.Static("/static", "./www/static");
server.Static("/public", "./www");
server.Static("/", "./www/index.html");
```

含义：

| 调用 | URL | 文件系统路径 |
| --- | --- | --- |
| `Static("/static", "./www/static")` | `/static/a.txt` | `./www/static/a.txt` |
| `Static("/public", "./www")` | `/public/index.html` | `./www/index.html` |
| `Static("/", "./www/index.html")` | `/` | `./www/index.html` |

`www/index.html` 和 `www/static/*.txt` 只是演示资源，前端知识点只需理解：

- 浏览器访问 `/` 通常期望返回 HTML 首页。
- HTML 中引用的静态文件通常通过 `/static/...` 或 `/public/...` 这类 URL 获取。
- 后端静态映射负责把 URL 路径转换为服务器本地文件路径。

> [!IMPORTANT]
> 手写静态资源服务时，路径拼接是高风险点。不要把用户传入的 URL 片段未经校验直接拼接到服务器目录后面。

## `09_series_handler.cc`：SeriesHandler 与请求生命周期扩展

普通 handler 签名为：

```cpp
void(const HttpReq*, HttpResp*)
```

`SeriesHandler` 签名为：

```cpp
void(const HttpReq*, HttpResp*, SeriesWork*)
```

示例：

```cpp
server.GET("/series", [](const HttpReq* req, HttpResp* resp, SeriesWork* series) {
    WFTimerTask* timerTask = WFTaskFactory::create_timer_task(3, 0, [](WFTimerTask*) {
        cout << "定时器任务完成 (3秒)" << endl;
    });
    series->push_back(timerTask);
    resp->String("Hello, SeriesTask!\n");
});
```

### `SeriesWork` 是什么

Workflow 中每个请求本质上运行在一个任务序列里。`SeriesWork` 表示串行任务链，前一个任务完成后才执行下一个任务。

`series->push_back(task)` 的含义是：把一个新任务追加到当前请求所在序列后面。

```mermaid
sequenceDiagram
    participant C as Client
    participant S as wfrest Handler
    participant Q as SeriesWork
    participant T as TimerTask

    C->>S: GET /series
    S->>Q: push_back(timerTask)
    S->>S: resp->String(...)
    Q->>C: 发送 HTTP 响应
    Q->>T: 执行 3 秒定时器
    T-->>Q: timer callback
```

> [!NOTE]
> 追加到同一个 `SeriesWork` 的任务会影响当前请求序列的完成时机。示例中响应内容已经设置，但整个任务序列还会继续执行定时器任务。

> [!CAUTION]
> 如果在请求序列里追加耗时任务，要清楚它是否会延迟响应发送、占用连接或影响客户端体验。对于与响应无关的后台任务，通常需要单独设计任务队列或异步处理机制。

## `10_mysql_example.cc`：wfrest MySQL 封装

示例展示了三种 MySQL 返回方式。

### 默认 MySQL 响应

```cpp
resp->MySQL("mysql://root:123456@localhost", "SHOW DATABASES");
```

wfrest 会：

1. 创建 Workflow MySQL 任务。
2. 把任务加入当前请求序列。
3. 根据 MySQL 结果构造默认响应。

### 执行多条 SQL

```cpp
string url = "mysql://root:123456@localhost/demo";
string sql = "SHOW DATABASES; SELECT * FROM tbl_user";
resp->MySQL(url, sql);
```

MySQL 协议支持一次请求中包含多条语句时，结果可能有多个 result set。处理多结果集时需要理解 `MySQLResultCursor::next_result_set()`。

### 自定义 MySQL 回调

```cpp
resp->MySQL(url, sql, [resp](MySQLResultCursor* cursor) {
    json result = json::array();
    vector<MySQLCell> record;
    while (cursor->fetch_row(record)) {
        result.push_back(record[0].as_string());
    }
    resp->Json(result.dump());
});
```

这里涉及：

- `nlohmann::json` 构造 JSON 数组。
- `MySQLResultCursor` 逐行读取结果。
- `MySQLCell::as_string()` 做字段类型转换。
- `resp->Json()` 返回 JSON 响应。

### `MySQLResultCursor` 常用接口

| 接口 | 作用 |
| --- | --- |
| `get_cursor_status()` | 判断当前结果状态 |
| `get_rows_count()` | 当前结果集行数 |
| `fetch_row(vector<MySQLCell>&)` | 按列顺序读取一行 |
| `fetch_row(map<string, MySQLCell>&)` | 按字段名读取一行 |
| `fetch_all(...)` | 读取全部行 |
| `get_affected_rows()` | DML 影响行数 |
| `get_insert_id()` | 插入后的自增 id |
| `next_result_set()` | 移动到下一个结果集 |

常见状态：

| 状态 | 含义 |
| --- | --- |
| `MYSQL_STATUS_OK` | 非查询语句成功，例如 INSERT/UPDATE |
| `MYSQL_STATUS_GET_RESULT` | 有 SELECT 结果集可读取 |
| `MYSQL_STATUS_ERROR` | MySQL 执行或协议解析错误 |
| `MYSQL_STATUS_END` | 结果集结束 |

> [!IMPORTANT]
> SELECT 类 SQL 通常检查 `MYSQL_STATUS_GET_RESULT`；INSERT/UPDATE/DELETE 类 SQL 通常检查 `MYSQL_STATUS_OK` 和 `get_affected_rows()`。不要用同一种状态判断所有 SQL。

> [!CAUTION]
> 示例中 MySQL URL 包含明文账号密码：`mysql://root:123456@localhost/demo`。真实项目不要把数据库凭据硬编码在源码里，应使用配置文件、环境变量、密钥管理服务，并限制数据库账号权限。

## nlohmann::json 使用点

本目录在 MySQL 示例和练习服务中使用：

```cpp
using json = nlohmann::json;
```

常见写法：

```cpp
json result;
result["code"] = 200;
result["message"] = "success";
result["token"] = token;

resp->String(result.dump());
```

或构造数组：

```cpp
json arr = json::array();
arr.push_back("demo");
resp->Json(arr.dump());
```

区别：

- `result.dump()` 把 JSON 对象序列化为字符串。
- `resp->String()` 返回普通字符串响应，具体 `Content-Type` 取决于框架默认行为。
- `resp->Json()` 更明确地表达返回 JSON，通常会设置 JSON 类型。

> [!NOTE]
> 当接口语义是 JSON API 时，优先使用 `resp->Json()` 或显式设置 `Content-Type: application/json`。只用 `resp->String(result.dump())` 虽然内容是 JSON 字符串，但语义上不如 JSON 响应明确。

## `CryptoUtil`：密码哈希、salt 与 JWT

`CryptoUtil.h` 定义：

```cpp
struct User {
    int id;
    std::string username;
    std::string hashcode;
    std::string salt;
    std::string createdAt;
};

class CryptoUtil {
public:
    static std::string generate_salt(int length = 8);
    static std::string hash_password(const std::string& password,
                                     const std::string& salt,
                                     const EVP_MD* md = EVP_sha256());
    static std::string generate_token(const User& user,
                                      jwt_alg_t algorithm = JWT_ALG_HS256);
    static bool verify_token(const std::string& token, User& user);

private:
    CryptoUtil() = delete;
};
```

### 静态工具类

`CryptoUtil()` 被删除：

```cpp
CryptoUtil() = delete;
```

这表示该类不能被实例化，只作为命名空间式工具类使用：

```cpp
CryptoUtil::generate_salt();
CryptoUtil::hash_password(password, salt);
```

### salt 生成

```cpp
string CryptoUtil::generate_salt(int length)
{
    const char* alpha = "0123456789"
                        "abcdefghijklmnopqrstuvwxyz"
                        "ABCDEFGHIJKLMNOPQRSTUVWXYZ";

    string result;
    for (int i = 0; i < length; ++i) {
        result += alpha[rand() % 62];
    }
    return result;
}
```

这里从 62 个字符中随机选取，默认长度 8。

> [!CAUTION]
> `rand()` 不适合密码学用途，且默认 salt 长度 8 偏短。生产环境应使用密码学安全随机数，例如 OpenSSL `RAND_bytes()`，并使用更长 salt。

### OpenSSL EVP 摘要接口

密码哈希流程：

```cpp
EVP_MD_CTX* ctx = EVP_MD_CTX_new();
unsigned char hash[EVP_MAX_MD_SIZE];
unsigned int hash_len;

EVP_DigestInit_ex(ctx, md, NULL);
EVP_DigestUpdate(ctx, password.c_str(), password.size());
EVP_DigestUpdate(ctx, salt.c_str(), salt.size());
EVP_DigestFinal_ex(ctx, hash, &hash_len);

EVP_MD_CTX_free(ctx);
```

EVP 是 OpenSSL 的高层加密接口，优点是算法可替换。默认参数：

```cpp
const EVP_MD* md = EVP_sha256()
```

表示默认使用 SHA-256。

### 二进制哈希转十六进制字符串

```cpp
char result[2 * EVP_MAX_MD_SIZE + 1] = { '\0' };
for (int i = 0; i < hash_len; i++) {
    sprintf(result + 2 * i, "%02x", hash[i]);
}
```

每个字节转成两个十六进制字符，例如一个 32 字节 SHA-256 摘要会变成 64 个 hex 字符。

> [!IMPORTANT]
> `SHA-256(password + salt)` 是理解密码哈希的好例子，但生产环境不建议直接用通用哈希保存密码。应使用专门的密码哈希算法，例如 bcrypt、scrypt、Argon2 或 PBKDF2，并设置合适成本参数。

### JWT 生成

```cpp
jwt_t* jwt;
jwt_new(&jwt);
jwt_set_alg(jwt, algorithm, (unsigned char*)SECRET_KEY, strlen(SECRET_KEY));
jwt_add_grant(jwt, "sub", "login");
jwt_add_grant_int(jwt, "id", user.id);
jwt_add_grant(jwt, "username", user.username.c_str());
jwt_add_grant_int(jwt, "expire", time(NULL) + 1800);

char* token = jwt_encode_str(jwt);
string result = token;
jwt_free(jwt);
free(token);
```

关键点：

- `SECRET_KEY` 是 HMAC 签名密钥。
- 默认算法是 `JWT_ALG_HS256`。
- payload 中保存 `sub`、`id`、`username`、`expire`。
- `expire` 设置为当前时间后 1800 秒，即 30 分钟。
- `jwt_encode_str()` 返回 C 风格字符串，需要 `free()`。
- `jwt_t*` 需要 `jwt_free()`。

> [!CAUTION]
> JWT payload 默认只是 Base64URL 编码，不是加密。不要把密码、salt、身份证号、银行卡号等敏感信息放进 token。

### JWT 验证

```cpp
int err = jwt_decode(&jwt, token.c_str(), (unsigned char*)SECRET_KEY, strlen(SECRET_KEY));
if (err) {
    return false;
}

if (strcmp("login", jwt_get_grant(jwt, "sub")) != 0) {
    jwt_free(jwt);
    return false;
}

if (jwt_get_grant_int(jwt, "expire") < time(NULL)) {
    jwt_free(jwt);
    return false;
}

user.id = jwt_get_grant_int(jwt, "id");
user.username = jwt_get_grant(jwt, "username");
jwt_free(jwt);
return true;
```

验证流程：

1. 用同一个 `SECRET_KEY` 解码并验证签名。
2. 检查 `sub` 是否为 `login`。
3. 检查 `expire` 是否过期。
4. 取出用户 id 和用户名。

> [!IMPORTANT]
> 示例使用自定义字段 `expire`。JWT 标准中更常用的是 `exp`。如果要接入通用 JWT 中间件、网关或第三方库，建议使用标准 claim，例如 `exp`、`iat`、`sub`。

> [!CAUTION]
> `SECRET_KEY` 硬编码在源码中，并且长度较短。生产环境应使用高强度密钥，通过环境变量或密钥管理系统注入，并支持密钥轮换。

## `wfrest_static_resources_server.cc`：注册、登录、鉴权静态资源服务器

该文件把前面多个知识点组合成一个小型后端：

- 表单解析。
- MySQL 查询与插入。
- 密码加盐哈希。
- JWT 登录态。
- Bearer Token 解析。
- 受保护静态文件服务。
- 路径穿越防护。
- JSON 响应。
- 异步 MySQL 回调中的变量捕获。

整体路由：

```cpp
server.POST("/register", do_register);
server.POST("/login", do_login);
server.GET("/*", do_send);
```

### 整体流程图

```mermaid
flowchart TD
    A[客户端] --> B{请求路径}
    B -->|POST /register| C[解析 username/password]
    C --> D[SELECT 检查用户名]
    D --> E{已存在?}
    E -->|是| F[400 用户名已存在]
    E -->|否| G[生成 salt 与 hash]
    G --> H[INSERT tbl_user]
    H --> I[返回注册结果]

    B -->|POST /login| J[解析 username/password]
    J --> K[SELECT 用户记录]
    K --> L{查到唯一用户?}
    L -->|否| M[400 用户名或密码错误]
    L -->|是| N[用数据库 salt 哈希输入密码]
    N --> O{hash 相等?}
    O -->|否| M
    O -->|是| P[生成 JWT]
    P --> Q[返回 token]

    B -->|GET /*| R[解析 Authorization Bearer]
    R --> S{token 有效?}
    S -->|否| T[401 Unauthorized]
    S -->|是| U[检查资源路径]
    U --> V{文件存在?}
    V -->|否| W[404 Not Found]
    V -->|是| X[resp->File 返回文件]
```

### 表单解析函数

```cpp
bool get_username_password(const HttpReq *req, string& username, string& password)
{
    map<string, string>& form = req->form_kv();

    if (form.count("username")) {
        username = form["username"];
    } else {
        return false;
    }

    if (form.count("password")) {
        password = form["password"];
    } else {
        return false;
    }

    return !username.empty() && !password.empty();
}
```

这个函数从 `x-www-form-urlencoded` 表单中读取 `username` 和 `password`。

> [!NOTE]
> 代码中注释掉了 `content_type()` 校验。学习时可以观察 wfrest 的容错行为；实际接口建议恢复校验，只接受明确的 `application/x-www-form-urlencoded` 或改用 JSON 请求体。

### SQL 字符串转义

`wfrest_static_resources_server.cc` 中新增：

```cpp
string escape_sql(const string& s)
{
    string res;
    for (char ch : s) {
        if (ch == '\'') {
            res += "\\'";
        } else if (ch == '\\') {
            res += "\\\\";
        } else {
            res += ch;
        }
    }
    return res;
}
```

它处理两类字符：

- 单引号 `'` 转为 `\'`。
- 反斜杠 `\` 转为 `\\`。

目的是降低手动拼接 SQL 时的语法截断和注入风险。例如用户名 `abc'def` 如果不转义，会破坏 SQL 字符串边界。

> [!IMPORTANT]
> `escape_sql()` 只是教学级简单处理，不等价于完整 SQL 防注入方案。正式项目应使用参数化查询、预处理语句，或成熟 ORM/数据库访问层。

### 注册逻辑

注册函数 `do_register()` 的流程：

1. 调用 `get_username_password()` 解析表单。
2. 生成查询 SQL，检查用户名是否存在。
3. `resp->MySQL()` 异步执行查询。
4. 回调中检查 `MYSQL_STATUS_GET_RESULT`。
5. 如果已存在，返回 400。
6. 如果不存在，生成 salt 和 hash。
7. 拼接 INSERT SQL。
8. 再次调用 `resp->MySQL()` 插入用户。
9. 插入回调中检查 `MYSQL_STATUS_OK`。
10. 返回 JSON 结果。

```mermaid
sequenceDiagram
    participant C as Client
    participant H as do_register
    participant M1 as MySQL SELECT
    participant M2 as MySQL INSERT
    participant R as HttpResp

    C->>H: POST /register
    H->>H: parse form
    H->>M1: resp->MySQL(check_sql)
    M1-->>H: cursor
    alt username exists
        H->>R: 400 JSON
    else username available
        H->>H: salt + hash
        H->>M2: resp->MySQL(insert_sql)
        M2-->>H: cursor
        H->>R: JSON register result
    end
```

### 异步回调变量捕获

注册逻辑中：

```cpp
resp->MySQL(MYSQL_URL, check_sql, [resp, username, password](MySQLResultCursor *cursor) {
    ...
});
```

登录逻辑中：

```cpp
resp->MySQL(MYSQL_URL, sql, [resp, password](MySQLResultCursor *cursor) {
    ...
});
```

这里 `username`、`password` 必须按值捕获。因为 `resp->MySQL()` 是异步任务，`do_register()` 或 `do_login()` 返回后，局部变量就销毁了。如果用引用捕获：

```cpp
[&username, &password]
```

回调执行时可能访问悬空引用，导致未定义行为。

> [!IMPORTANT]
> 异步回调中捕获局部变量时，默认优先按值捕获。只有能证明被引用对象的生命周期覆盖回调执行周期时，才考虑引用捕获。

### 嵌套异步任务

注册流程中，第一次 MySQL 回调里又调用了一次：

```cpp
resp->MySQL(MYSQL_URL, insert_sql, [resp](MySQLResultCursor *cursor) {
    ...
});
```

这相当于把“查重”和“插入”串联起来。只有查重完成后，才会创建插入任务。

> [!NOTE]
> wfrest 的 `resp->MySQL()` 会把 MySQL task 加入当前请求的序列。嵌套调用可以表达“前一步结果决定下一步任务”的动态工作流。

### 登录逻辑

登录函数 `do_login()` 流程：

1. 解析表单 username/password。
2. 查询用户表：

```sql
SELECT id, username, password, salt, created_at
FROM tbl_user
WHERE username='...'
LIMIT 1;
```

3. 检查是否得到唯一用户。
4. 从结果集中读取：

```cpp
vector<MySQLCell> record;
user.id = record[0].as_int();
user.username = record[1].as_string();
user.hashcode = record[2].as_string();
user.salt = record[3].as_string();
user.createdAt = record[4].as_string();
```

5. 用用户输入密码和数据库 salt 重新计算 hash。
6. 与数据库保存的 hash 比对。
7. 成功则生成 JWT。
8. 返回 JSON，其中包含 token。

### Bearer Token 解析

```cpp
bool get_bearer_token(const HttpReq *req, string& token)
{
    if (!req->has_header("Authorization")) {
        return false;
    }

    const string& auth = req->header("Authorization");
    const string prefix = "Bearer ";

    if (auth.size() <= prefix.size()) {
        return false;
    }

    if (auth.substr(0, prefix.size()) != prefix) {
        return false;
    }

    token = auth.substr(prefix.size());
    return !token.empty();
}
```

标准格式：

```http
Authorization: Bearer eyJhbGciOiJIUzI1NiIs...
```

解析逻辑：

- 必须有 `Authorization` 请求头。
- 必须以 `"Bearer "` 开头。
- 截取后面的 token 字符串。
- token 不能为空。

> [!NOTE]
> `Bearer` 是 OAuth 2.0 中常见的 token 传递方式。含义是“持有这个 token 的请求方即可被视为拥有对应权限”，所以 token 泄漏风险很高。

### 受保护静态资源逻辑

```cpp
void do_send(const HttpReq *req, HttpResp *resp)
{
    string token;
    if (!get_bearer_token(req, token)) {
        resp->set_status(HttpStatusUnauthorized);
        resp->String("<html>401 Unauthorized: missing token</html>");
        return;
    }

    User user;
    if (!CryptoUtil::verify_token(token, user)) {
        resp->set_status(HttpStatusUnauthorized);
        resp->String("<html>401 Unauthorized: invalid or expired token</html>");
        return;
    }

    string path = req->current_path();
    if (path == "/") {
        path = "/index.html";
    }

    if (!is_safe_path(path)) {
        resp->set_status(HttpStatusBadRequest);
        resp->String("<html>Bad Request: unsafe path</html>");
        return;
    }

    string real_path = RESOURCE_ROOT + path;

    if (!filesystem::exists(real_path) || !filesystem::is_regular_file(real_path)) {
        resp->set_status(HttpStatusNotFound);
        resp->String("<html>404 Not Found</html>");
        return;
    }

    resp->File(real_path);
}
```

这个函数实现了“先鉴权，再发文件”。

### 路径穿越防护

```cpp
bool is_safe_path(const string& path)
{
    if (path.empty()) {
        return false;
    }

    if (path.find("..") != string::npos) {
        return false;
    }

    return true;
}
```

攻击示例：

```http
GET /../../etc/passwd HTTP/1.1
Authorization: Bearer ...
```

如果直接拼接：

```cpp
string real_path = "resources" + "/../../etc/passwd";
```

就可能越过资源目录。

> [!CAUTION]
> 只检查字符串中是否包含 `".."` 是基础防护，不是完整路径安全方案。更严谨的做法是对路径做规范化，计算 canonical path，确认最终路径仍位于资源根目录下。

### `std::filesystem`

代码中使用 C++17 文件系统库：

```cpp
if (!filesystem::exists(real_path) || !filesystem::is_regular_file(real_path)) {
    resp->set_status(HttpStatusNotFound);
    return;
}
```

含义：

- `filesystem::exists(path)` 判断路径是否存在。
- `filesystem::is_regular_file(path)` 判断是否为普通文件。

这可以避免把目录当成文件返回。

> [!NOTE]
> `std::filesystem` 是 C++17 标准库组成部分。较旧编译器可能需要额外链接 `-lstdc++fs`，但现代 GCC 通常不需要。

## `wfrest_server_practice.cc` 与 `wfrest_static_resources_server.cc` 的差异

两个文件整体功能相近，主要差异：

| 项目 | `wfrest_server_practice.cc` | `wfrest_static_resources_server.cc` |
| --- | --- | --- |
| SQL 转义 | 直接拼接用户输入 | 使用 `escape_sql()` 简单转义 |
| 注释完整度 | 基础注释 | 对流程、异步捕获、安全点说明更详细 |
| 注册 SQL | 用户名、hash、salt 直接拼接 | 用户名、hash、salt 经 `escape_sql()` |
| 学习定位 | 原始练习版本 | 更适合作为完整学习版本 |

> [!IMPORTANT]
> 两个版本都仍是教学示例。即使使用 `escape_sql()`，也不应作为生产级用户系统直接使用。

## Makefile 与链接知识点

`Wfrest_Server/Makefile`：

```makefile
CXX := g++
CXXFLAGS := -std=c++17 -g -Wall -I..
LDFLAGS := -lwfrest -lworkflow -ljwt -lssl -lcrypto -lpthread

TARGET := wfrest_static_resources_server
SRCS := wfrest_static_resources_server.cc CryptoUtil.cc

all: $(TARGET)

$(TARGET): $(SRCS) CryptoUtil.h
	$(CXX) $(CXXFLAGS) $(SRCS) -o $(TARGET) $(LDFLAGS)

clean:
	rm -f $(TARGET)
```

### 编译选项

| 选项 | 含义 |
| --- | --- |
| `-std=c++17` | 使用 C++17，支持结构化绑定、`std::filesystem` 等 |
| `-g` | 生成调试信息 |
| `-Wall` | 开启常见警告 |
| `-I..` | 把上级目录加入头文件搜索路径 |

### 链接库

| 库 | 用途 |
| --- | --- |
| `-lwfrest` | wfrest HTTP server 封装 |
| `-lworkflow` | Workflow 异步任务和网络框架 |
| `-ljwt` | libjwt，生成和验证 JWT |
| `-lssl` | OpenSSL SSL/TLS 库 |
| `-lcrypto` | OpenSSL 加密算法库，EVP 摘要用到 |
| `-lpthread` | POSIX 线程库，Workflow 运行需要 |

> [!CAUTION]
> 链接顺序在 g++ 中可能影响解析结果。通常把源文件放前面，库放后面，即 `g++ a.cc -o app -lxxx`。

## 前端和静态资源必要知识点

`07_Wfrest/www` 中包含：

- `index.html`
- `static/a.txt`
- `static/b.txt`
- `1.text`
- `2.text`

这些文件用于验证静态资源映射：

```cpp
server.Static("/static", "./www/static");
server.Static("/public", "./www");
server.Static("/", "./www/index.html");
```

必要理解点：

1. 浏览器请求 `/` 时，服务端返回 `index.html`。
2. 浏览器请求 `/static/a.txt` 时，服务端映射到 `./www/static/a.txt`。
3. 浏览器请求 `/public/1.text` 时，服务端映射到 `./www/1.text`。
4. 后端静态资源服务本质是 URL path 到本地文件路径的转换。
5. 文件扩展名通常用于推断 MIME 类型，例如 `.html` 对应 `text/html`，`.txt` 对应 `text/plain`。

> [!NOTE]
> 本目录的 `index.html` 内容非常短，只用于验证后端文件返回是否成功，不涉及复杂 HTML/CSS/JavaScript。

## HTTP 状态码使用

代码中用到了多个状态码：

| 状态码 | 常量 | 使用场景 |
| --- | --- | --- |
| 200 | `HttpStatusOK` | 请求成功 |
| 400 | `HttpStatusBadRequest` | 请求参数错误、表单字段缺失、用户名已存在、登录失败 |
| 401 | `HttpStatusUnauthorized` | token 缺失、无效或过期 |
| 404 | `HttpStatusNotFound` | 静态资源不存在 |
| 500 | `HttpStatusInternalServerError` | 数据库查询或插入失败 |

> [!IMPORTANT]
> HTTP 状态码表达协议层结果，JSON 中的 `code` 字段表达业务层结果。两者最好保持一致，例如 HTTP 400 时 JSON 中也写 `code: 400`。

## 请求处理中的内存与生命周期

### `const string&` 返回值

很多接口返回引用：

```cpp
const string& req->query("username");
const string& req->header("Host");
const string& req->param("name");
```

这些引用指向 `HttpReq` 内部保存的数据。只要 `HttpReq` 还活着，引用有效；请求结束后就不能再访问。

> [!CAUTION]
> 不要把 `req->query()`、`req->header()`、`req->param()` 返回的引用保存到异步回调中。需要跨异步边界使用时，应复制成 `std::string` 并按值捕获。

### `HttpResp*` 的异步使用

代码中多次在 MySQL 回调中捕获 `resp`：

```cpp
[resp](MySQLResultCursor *cursor) {
    resp->String(...);
}
```

这是因为 `resp->MySQL()` 创建的任务属于当前请求序列，回调仍在该请求序列生命周期内执行。这个模式是 wfrest 设计支持的。

但不要把 `resp` 交给独立线程或脱离当前请求序列的后台任务长期保存。

## 安全知识点汇总

### 1. SQL 注入

风险代码：

```cpp
"WHERE username='" + username + "'"
```

攻击输入可能包含：

```text
' OR '1'='1
```

示例改进：

```cpp
escape_sql(username)
```

更好方案：

- 参数化查询。
- 预处理语句。
- 最小权限数据库用户。
- 输入长度限制。
- 审计日志。

### 2. 密码存储

示例：

```cpp
hash_password(password, salt, EVP_sha256())
```

学习点：

- 不保存明文密码。
- 每个用户使用不同 salt。
- 登录时重新计算 hash 并比对。

生产改进：

- 使用 bcrypt/scrypt/Argon2/PBKDF2。
- salt 使用安全随机数。
- 支持密码哈希版本升级。
- 避免日志打印密码。

### 3. JWT

风险点：

- 密钥硬编码。
- token 泄漏后可被直接使用。
- payload 不加密。
- 自定义 `expire` 不如标准 `exp` 通用。

生产改进：

- 使用高强度密钥。
- HTTPS 传输。
- 合理过期时间。
- refresh token 机制。
- token 黑名单或版本号机制。
- 使用标准 claim。

### 4. 文件上传

风险点：

- 路径穿越。
- 文件覆盖。
- 大文件导致内存或磁盘耗尽。
- 恶意文件类型。

生产改进：

- basename + 随机服务器文件名。
- 限制大小和扩展名。
- 保存到专用目录。
- 文件扫描。
- 不允许直接执行上传文件。

### 5. 静态文件下载

风险点：

- 读取资源根目录外文件。
- 暴露服务器目录结构。
- MIME 类型不正确导致浏览器误解析。

生产改进：

- canonical path 校验。
- 白名单资源目录。
- 缓存头和 MIME 类型控制。
- 不返回隐藏文件或配置文件。

## 常用 curl 测试命令

### 基础 GET

```bash
curl -v http://127.0.0.1:8888/hello
```

### echo POST

```bash
curl -v -X POST http://127.0.0.1:8888/echo \
  -H "Content-Type: text/plain" \
  -d "hello wfrest"
```

### 查询参数

```bash
curl -v "http://127.0.0.1:8888/query?username=peanut&password=123456"
```

### URL 编码表单

```bash
curl -v -X POST http://127.0.0.1:8888/form-urlencoded \
  -H "Content-Type: application/x-www-form-urlencoded" \
  -d "user=admin&password=123456"
```

### 文件上传

```bash
curl -v -X POST http://127.0.0.1:8888/upload-file \
  -F "file=@demo.txt"
```

### 注册

```bash
curl -v -X POST http://127.0.0.1:8888/register \
  -H "Content-Type: application/x-www-form-urlencoded" \
  -d "username=alice&password=123456"
```

### 登录并获取 token

```bash
curl -v -X POST http://127.0.0.1:8888/login \
  -H "Content-Type: application/x-www-form-urlencoded" \
  -d "username=alice&password=123456"
```

### 携带 token 访问受保护资源

```bash
curl -v http://127.0.0.1:8888/index.html \
  -H "Authorization: Bearer <token>"
```

## MySQL 表结构推测

代码中使用的字段：

```sql
SELECT id, username, password, salt, created_at
FROM tbl_user
WHERE username='...'
LIMIT 1;
```

可推测表结构类似：

```sql
CREATE TABLE tbl_user (
    id INT PRIMARY KEY AUTO_INCREMENT,
    username VARCHAR(255) NOT NULL UNIQUE,
    password VARCHAR(255) NOT NULL,
    salt VARCHAR(64) NOT NULL,
    created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP
);
```

> [!NOTE]
> 这里是根据源码 SQL 推测的学习用表结构，实际数据库需要以本机 MySQL 中的真实 schema 为准。

## 代码中的 C++ 进阶知识点

### Lambda 表达式

路由处理函数大量使用 lambda：

```cpp
[](const HttpReq* req, HttpResp* resp) {
    resp->String("world\n");
}
```

异步回调使用捕获列表：

```cpp
[resp, username, password](MySQLResultCursor *cursor) {
    ...
}
```

核心点：

- `[]`：不捕获外部变量。
- `[resp]`：按值捕获指针变量。
- `[resp, password]`：按值捕获多个变量。
- `[&]`：按引用捕获所有用到的外部变量，异步场景慎用。

### 结构化绑定

```cpp
for (const auto& [key, value] : form) {
    ...
}
```

要求 C++17，适合遍历 `map`、`pair`、`tuple`。

### `std::move`

```cpp
resp->Save(basename, std::move(content), ...);
```

`std::move` 本身不移动数据，只把表达式转换成右值，是否真正移动取决于目标函数和对象类型。

### RAII 与 C 库资源释放

`CryptoUtil.cc` 使用 C 风格库：

```cpp
EVP_MD_CTX* ctx = EVP_MD_CTX_new();
...
EVP_MD_CTX_free(ctx);

char* token = jwt_encode_str(jwt);
...
free(token);
```

这些资源需要手动释放。更现代的 C++ 写法可以用智能指针包装释放器：

```cpp
std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> ctx(EVP_MD_CTX_new(), EVP_MD_CTX_free);
```

> [!NOTE]
> 本目录代码以教学清晰为主，手动展示了 C API 的创建与释放。实际工程中建议用 RAII 封装，降低异常或早返回导致资源泄漏的风险。

### `std::filesystem`

```cpp
#include <filesystem>

filesystem::exists(real_path)
filesystem::is_regular_file(real_path)
```

这是 C++17 标准库文件系统组件，适合做路径存在性、文件类型、目录遍历、路径规范化等操作。

### `static const` 内部链接

```cpp
static const char* SECRET_KEY = "$^Hk16NV";
```

文件作用域的 `static` 使变量具有内部链接，只在当前编译单元可见。

> [!CAUTION]
> 内部链接只是不让其他 `.cc` 直接链接访问，不是安全保护。密钥仍然会出现在二进制文件中，不能用它来隐藏敏感信息。

## 推荐学习顺序

1. 运行 `01_hello_wfrest.cc`，理解服务启动、路由和响应。
2. 学习 `02_register_routes.cc`，掌握 RESTful 路径设计。
3. 学习 `03_query_parameters.cc` 和 `04_request_header.cc`，掌握请求元信息读取。
4. 学习 `05_request_body.cc`，理解 body、表单和 `Content-Type`。
5. 学习 `06_upload_file.cc`、`07_download_file.cc`、`08_static_file_server.cc`，掌握文件 IO 与静态资源服务。
6. 学习 `09_series_handler.cc`，把 wfrest 和 Workflow 异步任务模型连起来。
7. 学习 `10_mysql_example.cc`，掌握数据库异步访问。
8. 学习 `Wfrest_Server`，理解完整登录鉴权和资源保护流程。

## 小结

`07_Wfrest` 的核心价值在于把前面 Workflow 的异步任务思想和 HTTP 服务端业务开发连接起来：

- `HttpServer` 负责监听端口并注册 RESTful 路由。
- `HttpReq` 负责读取请求路径、查询参数、请求头、请求体和表单。
- `HttpResp` 负责构造字符串、JSON、文件、MySQL 等响应。
- `SeriesWork` 允许在一次请求生命周期中追加异步任务。
- `resp->MySQL()`、`resp->File()`、`resp->Save()` 将数据库、文件 IO 封装成请求序列中的任务。
- `CryptoUtil` 展示了 OpenSSL EVP 和 libjwt 的基础用法。
- 完整练习服务展示了注册、登录、token 验证、静态资源授权访问的后端基本闭环。

> [!IMPORTANT]
> 学习本目录时不要只看 API 调用，还要关注异步生命周期、安全边界和输入可信度。Web 服务端开发的大量 bug 都来自“把用户输入当可信数据”或“把异步回调当同步代码写”。
