# 静态资源服务器详细说明文档

## 1. 项目概述

本项目使用 `workflow` 框架实现了一个带登录注册功能的静态资源服务器。

服务器完成了以下功能：

1. 用户注册。
2. 用户登录。
3. 登录成功后返回 Token。
4. 下载静态资源前验证 Token。
5. Token 不存在、错误或超时，返回 `401 unauthorized`。
6. 使用 workflow 的 MySQL 异步任务访问数据库。
7. 使用 workflow 的文件 IO 任务读取磁盘文件。

## 2. 需求对应关系

### 2.1 登录注册

要求：

```text
每个用户都有自己的随机盐值，数据库存储的哈希后的密码。
```

代码实现位置：

```cpp
void handle_register(WFHttpTask* httpTask)
```

注册时会执行：

```cpp
string salt = CryptoUtil::generate_salt(16);
string hashcode = CryptoUtil::hash_password(password, salt);
```

然后把 `username`、`hashcode`、`salt` 写入 `tbl_user`。

### 2.2 登录成功返回 Token

要求：

```text
登录成功，服务器通过响应体给客户端返回一个 Token。
```

代码实现位置：

```cpp
void login_callback(WFMySQLTask* task, HttpResponse* resp, string password)
```

密码验证成功后会执行：

```cpp
string token = CryptoUtil::generate_token(user);
set_text_response(resp, "200", token + "\n");
```

也就是说，登录成功后 HTTP 响应体里只有 Token 文本。

### 2.3 Token 有效期 30 分钟

Token 有效期在 `CryptoUtil.cc` 中设置：

```cpp
jwt_add_grant_int(jwt, "expire", time(NULL) + 1800);
```

`1800` 秒就是 30 分钟。

### 2.4 每次下载资源都验证 Token

代码实现位置：

```cpp
void process(WFHttpTask* httpTask)
```

处理静态资源下载前会执行：

```cpp
string token = get_token_from_request(req, uri);

User user;
if (token.empty() || !CryptoUtil::verify_token(token, user)) {
    set_unauthorized(resp);
    return;
}
```

只有 Token 校验成功，才会继续执行：

```cpp
handle_static_file(httpTask, path);
```

### 2.5 未登录或 Token 超时返回 401

代码实现位置：

```cpp
void set_unauthorized(HttpResponse* resp)
```

函数内容：

```cpp
resp->set_status_code("401");
resp->set_reason_phrase("Unauthorized");
resp->append_output_body("401 unauthorized\n");
```

## 3. 目录结构

```text
Static_Resource_Server/
├── CryptoUtil.cc
├── CryptoUtil.h
├── Makefile
├── README.md
└── static_resource_server.cc
```

各文件作用：

| 文件 | 作用 |
| --- | --- |
| `static_resource_server.cc` | 静态资源服务器主程序，包含 HTTP 路由、注册、登录、Token 校验和文件下载 |
| `CryptoUtil.h` | 加密工具类头文件，定义 `User` 结构体和 `CryptoUtil` 类 |
| `CryptoUtil.cc` | 加密工具类实现，包含生成 salt、哈希密码、生成 Token、验证 Token |
| `Makefile` | 编译脚本 |
| `README.md` | 当前说明文档 |

静态资源目录使用项目根目录已有的：

```text
resources/
├── index.html
└── dir/
    └── a.txt
```

访问 `/index.html` 时，服务器实际读取：

```text
resources/index.html
```

访问 `/dir/a.txt` 时，服务器实际读取：

```text
resources/dir/a.txt
```

## 4. 编译和运行

进入服务器目录：

```bash
cd Static_Resource_Server
```

编译：

```bash
make
```

编译后会生成：

```text
static_resource_server
```

从项目根目录运行：

```bash
./Static_Resource_Server/static_resource_server
```

也可以进入 `Static_Resource_Server` 后运行：

```bash
./static_resource_server
```

代码中做了资源目录判断：

```cpp
if (stat("resources", &statbuf) == 0 && S_ISDIR(statbuf.st_mode)) {
    root = "resources";
} else {
    root = "../resources";
}
```

所以从项目根目录启动和从 `Static_Resource_Server` 目录启动都可以找到静态资源。

服务器启动成功后会输出：

```text
Static resource server is running at http://127.0.0.1:8888
```

停止服务器：

```text
按 Ctrl+C
```

## 5. 依赖库说明

本程序用到了这些库：

| 依赖 | 用途 |
| --- | --- |
| `workflow` | HTTP 服务器、MySQL 异步任务、文件 IO 异步任务 |
| `jwt` | 生成和验证 JWT Token |
| `openssl crypto` | SHA256 哈希计算 |
| `pthread` | workflow 底层线程支持 |

`Makefile` 中的链接参数：

```makefile
LDFLAGS := -lworkflow -ljwt -lssl -lcrypto -lpthread
```

## 6. 数据库说明

程序连接本机 MySQL：

```cpp
static const string MYSQL_URL = "mysql://root:123456@127.0.0.1:3306/demo?character_set=utf8mb4";
```

含义：

| 部分 | 含义 |
| --- | --- |
| `root` | MySQL 用户名 |
| `123456` | MySQL 密码 |
| `127.0.0.1:3306` | 本机 MySQL 地址和端口 |
| `demo` | 数据库名 |
| `character_set=utf8mb4` | 使用 utf8mb4 字符集 |

用户表结构：

```sql
CREATE TABLE `tbl_user` (
  `id` int NOT NULL AUTO_INCREMENT,
  `username` varchar(255) NOT NULL,
  `password` varchar(255) NOT NULL,
  `salt` varchar(64) NOT NULL,
  `created_at` datetime DEFAULT CURRENT_TIMESTAMP,
  `tomb` int DEFAULT '0',
  PRIMARY KEY (`id`),
  UNIQUE KEY `username` (`username`)
) ENGINE=InnoDB AUTO_INCREMENT=10 DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_ai_ci;
```

字段解释：

| 字段 | 含义 |
| --- | --- |
| `id` | 用户 id，自增主键 |
| `username` | 用户名，唯一 |
| `password` | 哈希后的密码，不是明文密码 |
| `salt` | 每个用户自己的随机盐值 |
| `created_at` | 用户创建时间 |
| `tomb` | 逻辑删除标记，`0` 表示正常用户 |

## 7. HTTP 接口

### 7.1 注册接口

请求地址：

```text
POST /register
```

请求体格式：

```text
username=用户名&password=密码
```

测试命令：

```bash
curl -X POST http://127.0.0.1:8888/register \
  -d "username=alice&password=123456"
```

注册成功：

```text
register ok
```

用户名重复：

```text
register failed: username already exists
```

缺少用户名或密码：

```text
username and password are required
```

### 7.2 登录接口

请求地址：

```text
POST /login
```

请求体格式：

```text
username=用户名&password=密码
```

测试命令：

```bash
curl -X POST http://127.0.0.1:8888/login \
  -d "username=alice&password=123456"
```

登录成功后，响应体是一长串 Token：

```text
eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9...
```

登录失败时返回：

```text
401 unauthorized
```

### 7.3 静态资源下载接口

请求地址：

```text
GET /资源路径
```

例如：

```text
GET /index.html
GET /dir/a.txt
```

下载资源时必须携带 Token。

推荐方式是使用 HTTP 请求头：

```bash
TOKEN="登录接口返回的Token"

curl -H "Authorization: Bearer $TOKEN" \
  http://127.0.0.1:8888/index.html
```

为了方便在浏览器地址栏中测试，也支持 URL 参数：

```text
http://127.0.0.1:8888/index.html?token=登录接口返回的Token
```

访问子目录文件：

```text
http://127.0.0.1:8888/dir/a.txt?token=登录接口返回的Token
```

如果 Token 缺失、错误或超时：

```text
401 unauthorized
```

## 8. 完整测试流程

### 8.1 启动服务

```bash
./Static_Resource_Server/static_resource_server
```

### 8.2 注册用户

```bash
curl -X POST http://127.0.0.1:8888/register \
  -d "username=alice&password=123456"
```

### 8.3 登录获取 Token

```bash
curl -X POST http://127.0.0.1:8888/login \
  -d "username=alice&password=123456"
```

把返回的 Token 保存下来：

```bash
TOKEN="这里替换成登录返回的Token"
```

### 8.4 不带 Token 下载资源

```bash
curl -i http://127.0.0.1:8888/index.html
```

预期结果：

```http
HTTP/1.1 401 Unauthorized
```

### 8.5 带 Token 下载资源

```bash
curl -i -H "Authorization: Bearer $TOKEN" \
  http://127.0.0.1:8888/index.html
```

预期结果：

```http
HTTP/1.1 200 OK
Content-Disposition: attachment; filename=index.html
```

### 8.6 浏览器地址栏测试

浏览器地址栏只能方便地发 GET 请求，所以它主要用来测试静态资源下载。

先用 `curl` 登录拿到 Token，然后在浏览器地址栏输入：

```text
http://127.0.0.1:8888/index.html?token=你的Token
```

如果 Token 有效，浏览器会显示或下载 `index.html`。

如果 Token 无效，会显示：

```text
401 unauthorized
```

## 9. 源码阅读顺序

建议按这个顺序阅读代码：

1. 先看 `main()`。
2. 再看 `process()`。
3. 再看 `handle_register()`。
4. 再看 `register_callback()`。
5. 再看 `handle_login()`。
6. 再看 `login_callback()`。
7. 再看 `get_token_from_request()`。
8. 再看 `handle_static_file()`。
9. 最后看 `pread_callback()`。

这样阅读的原因是：`main()` 创建服务器，`process()` 是所有请求的入口，后面的函数都是被 `process()` 按不同路径调用的。

## 10. 程序整体运行流程

启动服务器后，workflow 会监听 `8888` 端口。

每当客户端发来一个 HTTP 请求，workflow 都会创建一个 `WFHttpTask`，然后调用：

```cpp
void process(WFHttpTask* httpTask)
```

`process()` 会做三件事：

1. 解析请求方法和路径。
2. 判断请求属于注册、登录还是静态资源下载。
3. 调用对应的处理函数。

大致流程如下：

```text
客户端请求
    |
    v
process()
    |
    +-- POST /register --> handle_register()
    |
    +-- POST /login -----> handle_login()
    |
    +-- GET /xxx --------> 验证 Token --> handle_static_file()
```

## 11. `main()` 函数说明

`main()` 是程序入口：

```cpp
int main()
{
    srand(time(NULL));
    signal(SIGINT, sig_handler);
    WFHttpServer server(process);

    if (server.start(SERVER_PORT) == 0) {
        waitGroup.wait();
        server.stop();
    }
}
```

逐步解释：

1. `srand(time(NULL))` 用当前时间初始化随机数种子，后面生成 salt 时会用到 `rand()`。
2. `signal(SIGINT, sig_handler)` 注册 Ctrl+C 信号处理函数。
3. `WFHttpServer server(process)` 创建 HTTP 服务器，并告诉 workflow 每个请求都交给 `process()` 处理。
4. `server.start(SERVER_PORT)` 在 `8888` 端口启动服务。
5. `waitGroup.wait()` 阻塞主线程，让服务器一直运行。
6. 按 Ctrl+C 后，`sig_handler()` 调用 `waitGroup.done()`，主线程继续执行。
7. `server.stop()` 停止服务器。

## 12. `process()` 函数说明

`process()` 是 HTTP 请求的总入口。

它先取得请求对象和响应对象：

```cpp
HttpRequest* req = httpTask->get_req();
HttpResponse* resp = httpTask->get_resp();
```

再取得请求方法和 URI：

```cpp
string method = req->get_method();
string uri = req->get_request_uri();
```

例如客户端请求：

```text
GET /index.html?token=abc HTTP/1.1
```

那么：

```text
method = "GET"
uri = "/index.html?token=abc"
```

代码会把 `?` 后面的查询字符串去掉：

```cpp
size_t queryPos = uri.find('?');
string path = uri.substr(0, queryPos);
```

得到：

```text
path = "/index.html"
```

然后按路径分发：

```cpp
if (path == "/register") {
    handle_register(httpTask);
    return;
}

if (path == "/login") {
    handle_login(httpTask);
    return;
}
```

如果不是 `/register` 或 `/login`，就当作静态资源请求。

静态资源请求必须是 GET：

```cpp
if (method != "GET") {
    set_text_response(resp, "405", "method not allowed\n");
    return;
}
```

然后验证 Token：

```cpp
string token = get_token_from_request(req, uri);

User user;
if (token.empty() || !CryptoUtil::verify_token(token, user)) {
    set_unauthorized(resp);
    return;
}
```

如果验证成功，才读取文件：

```cpp
handle_static_file(httpTask, path);
```

## 13. 注册流程详解

注册入口：

```cpp
void handle_register(WFHttpTask* httpTask)
```

### 13.1 读取请求体

```cpp
string body = get_request_body(req);
```

假设客户端发送：

```bash
curl -X POST http://127.0.0.1:8888/register \
  -d "username=alice&password=123456"
```

那么请求体就是：

```text
username=alice&password=123456
```

### 13.2 解析表单

```cpp
map<string, string> form = parse_form(body);
string username = form["username"];
string password = form["password"];
```

解析后：

```text
username = "alice"
password = "123456"
```

### 13.3 生成 salt

```cpp
string salt = CryptoUtil::generate_salt(16);
```

salt 是一个随机字符串，例如：

```text
a8K2mP0zQw7YxL3n
```

每个用户都会生成自己的 salt。

### 13.4 计算密码哈希

```cpp
string hashcode = CryptoUtil::hash_password(password, salt);
```

这里不是直接保存 `123456`，而是保存哈希结果。

大致过程：

```text
password + salt --> SHA256 --> 十六进制字符串
```

数据库保存的是哈希字符串，不保存明文密码。

### 13.5 插入数据库

```cpp
string query = "INSERT INTO tbl_user(username, password, salt) VALUES('"
             + sql_escape(username) + "', '"
             + sql_escape(hashcode) + "', '"
             + sql_escape(salt) + "')";
```

生成的 SQL 类似：

```sql
INSERT INTO tbl_user(username, password, salt)
VALUES('alice', '哈希后的密码', '随机salt')
```

### 13.6 创建 MySQL 任务

```cpp
WFMySQLTask* mysqlTask = WFTaskFactory::create_mysql_task(
    MYSQL_URL,
    3,
    bind(register_callback, _1, resp, username));
```

参数解释：

| 参数 | 含义 |
| --- | --- |
| `MYSQL_URL` | 数据库连接地址 |
| `3` | 失败后最多重试 3 次 |
| `register_callback` | MySQL 任务结束后调用的回调函数 |

### 13.7 设置 SQL 并加入序列

```cpp
mysqlTask->get_req()->set_query(query);
series_of(httpTask)->push_back(mysqlTask);
```

这两行非常重要。

`set_query()` 设置要执行的 SQL。

`series_of(httpTask)->push_back(mysqlTask)` 表示：把 MySQL 任务加入当前 HTTP 请求的任务序列中。

workflow 会保证：

```text
HTTP 请求进入 process()
    |
    v
执行 MySQL 任务
    |
    v
执行 register_callback()
    |
    v
发送 HTTP 响应
```

这就是 workflow 中常见的任务串联方式。

## 14. 注册回调详解

注册回调：

```cpp
void register_callback(WFMySQLTask* task, HttpResponse* resp, string username)
```

这个函数会在 INSERT SQL 执行结束后被 workflow 调用。

### 14.1 判断任务状态

```cpp
int state = task->get_state();
if (state != WFT_STATE_SUCCESS) {
    set_text_response(resp, "500", "register failed: mysql task error\n");
    return;
}
```

这里判断的是网络层或任务层是否成功。

例如：

1. MySQL 没启动。
2. 连接失败。
3. 网络错误。

这些属于任务失败。

### 14.2 判断 SQL 是否报错

```cpp
MySQLResponse* mysqlResp = task->get_resp();
if (mysqlResp->is_error_packet()) {
    ...
}
```

任务成功不代表 SQL 成功。

例如用户名重复时，MySQL 可以正常连接，但 SQL 会失败，因为 `username` 有唯一索引。

用户名重复的错误码是 `1062`：

```cpp
if (mysqlResp->get_error_code() == 1062) {
    set_text_response(resp, "409", "register failed: username already exists\n");
}
```

### 14.3 注册成功

```cpp
set_text_response(resp, "200", "register ok\n");
```

注册成功后返回普通文本。

## 15. 登录流程详解

登录入口：

```cpp
void handle_login(WFHttpTask* httpTask)
```

登录前半部分和注册类似：

1. 读取请求体。
2. 解析 `username` 和 `password`。
3. 校验二者不能为空。

然后根据用户名查询数据库：

```cpp
string query = "SELECT id, username, password, salt, created_at "
               "FROM tbl_user "
               "WHERE username = '" + sql_escape(username) + "' AND tomb = 0 "
               "LIMIT 1";
```

查询结果包含：

| 字段 | 用途 |
| --- | --- |
| `id` | 生成 Token 时写入用户 id |
| `username` | 生成 Token 时写入用户名 |
| `password` | 数据库中保存的哈希密码 |
| `salt` | 该用户的盐值 |
| `created_at` | 用户创建时间 |

然后创建 MySQL 查询任务：

```cpp
WFMySQLTask* mysqlTask = WFTaskFactory::create_mysql_task(
    MYSQL_URL,
    3,
    bind(login_callback, _1, resp, password));
```

注意这里把用户输入的明文 `password` 传给了回调函数。

因为真正比较密码是在 MySQL 查询结果回来之后进行的，只有拿到数据库里的 salt，才能重新计算哈希。

## 16. 登录回调详解

登录回调：

```cpp
void login_callback(WFMySQLTask* task, HttpResponse* resp, string password)
```

### 16.1 检查任务状态和 SQL 状态

和注册回调一样，先检查：

```cpp
task->get_state()
```

再检查：

```cpp
mysqlResp->is_error_packet()
```

### 16.2 读取查询结果

```cpp
MySQLResultCursor cursor(mysqlResp);
vector<MySQLCell> row;

if (!cursor.fetch_row(row)) {
    set_unauthorized(resp);
    return;
}
```

如果没有查到用户，说明：

1. 用户名不存在。
2. 或者该用户 `tomb != 0`。

此时返回 `401 unauthorized`。

### 16.3 把数据库记录放入 User

```cpp
User user;
user.id = row[0].as_int();
user.username = row[1].as_string();
user.hashcode = row[2].as_string();
user.salt = row[3].as_string();
```

`User` 结构体定义在 `CryptoUtil.h`：

```cpp
struct User {
    int id;
    std::string username;
    std::string hashcode;
    std::string salt;
    std::string createdAt;
};
```

### 16.4 校验密码

服务器重新计算哈希：

```cpp
string hashcode = CryptoUtil::hash_password(password, user.salt);
```

然后和数据库中的哈希比较：

```cpp
if (hashcode != user.hashcode) {
    set_unauthorized(resp);
    return;
}
```

如果不一致，说明密码错误。

### 16.5 生成 Token

密码正确后：

```cpp
string token = CryptoUtil::generate_token(user);
```

然后把 Token 返回给客户端：

```cpp
set_text_response(resp, "200", token + "\n");
```

## 17. CryptoUtil 说明

`CryptoUtil` 现成工具类，本项目直接复用。

### 17.1 `generate_salt`

```cpp
static std::string generate_salt(int length = 8);
```

作用：生成随机 salt。

代码中的字符范围：

```text
0-9
a-z
A-Z
```

本服务器注册时调用：

```cpp
CryptoUtil::generate_salt(16)
```

生成 16 位 salt。

### 17.2 `hash_password`

```cpp
static std::string hash_password(
    const std::string& password,
    const std::string& salt,
    const EVP_MD* md = EVP_sha256());
```

作用：把 `password` 和 `salt` 合在一起做 SHA256 哈希。

核心流程：

```cpp
EVP_DigestUpdate(ctx, password.c_str(), password.size());
EVP_DigestUpdate(ctx, salt.c_str(), salt.size());
EVP_DigestFinal_ex(ctx, hash, &hash_len);
```

然后把二进制哈希结果转换成十六进制字符串。

### 17.3 `generate_token`

```cpp
static std::string generate_token(const User& user, jwt_alg_t algorithm = JWT_ALG_HS256);
```

作用：根据用户信息生成 JWT Token。

Token 中写入：

```cpp
jwt_add_grant(jwt, "sub", "login");
jwt_add_grant_int(jwt, "id", user.id);
jwt_add_grant(jwt, "username", user.username.c_str());
jwt_add_grant_int(jwt, "expire", time(NULL) + 1800);
```

含义：

| 字段 | 含义 |
| --- | --- |
| `sub` | Token 主题，这里固定为 `login` |
| `id` | 用户 id |
| `username` | 用户名 |
| `expire` | 过期时间，当前时间加 1800 秒 |

### 17.4 `verify_token`

```cpp
static bool verify_token(const std::string& token, User& user);
```

作用：

1. 校验 Token 签名是否正确。
2. 校验 `sub` 是否等于 `login`。
3. 校验 `expire` 是否已经过期。
4. 从 Token 中解析出用户 id 和用户名。

如果验证成功，返回 `true`，并填充 `user`：

```cpp
user.id = jwt_get_grant_int(jwt, "id");
user.username = jwt_get_grant(jwt, "username");
```

如果验证失败，返回 `false`。

## 18. Token 获取方式

函数：

```cpp
string get_token_from_request(HttpRequest* req, const string& uri)
```

这个函数支持三种 Token 携带方式。

### 18.1 Authorization Bearer

推荐方式：

```http
Authorization: Bearer Token内容
```

代码：

```cpp
string authorization = get_header_value(req, "Authorization");
string bearer = "Bearer ";
if (authorization.compare(0, bearer.size(), bearer) == 0) {
    return authorization.substr(bearer.size());
}
```

### 18.2 Authorization 直接放 Token

为了方便测试，也支持：

```http
Authorization: Token内容
```

代码：

```cpp
if (!authorization.empty()) {
    return authorization;
}
```

### 18.3 URL 参数

为了方便浏览器地址栏测试，也支持：

```text
/index.html?token=Token内容
```

代码：

```cpp
size_t queryPos = uri.find('?');
if (queryPos != string::npos) {
    string query = uri.substr(queryPos + 1);
    map<string, string> queryMap = parse_form(query);
    if (queryMap.count("token") != 0) {
        return queryMap["token"];
    }
}
```

## 19. 静态资源下载流程详解

静态资源处理函数：

```cpp
void handle_static_file(WFHttpTask* httpTask, string path)
```

### 19.1 `/` 映射到 `/index.html`

```cpp
if (path == "/") {
    path += "index.html";
}
```

访问：

```text
http://127.0.0.1:8888/?token=xxx
```

等价于访问：

```text
http://127.0.0.1:8888/index.html?token=xxx
```

### 19.2 拒绝 `..` 路径

```cpp
if (path.find("..") != string::npos) {
    set_text_response(resp, "403", "403 forbidden\n");
    return;
}
```

原因是 `..` 可能让用户访问 `resources` 目录之外的文件。

例如：

```text
/../CryptoUtil.cc
```

这种路径不应该允许。

### 19.3 拼接真实文件路径

```cpp
string filePath = get_resource_root() + path;
```

例如：

```text
path = "/index.html"
get_resource_root() = "resources"
filePath = "resources/index.html"
```

### 19.4 打开文件

```cpp
int fd = open(filePath.c_str(), O_RDONLY);
if (fd == -1) {
    set_text_response(resp, "404", "<html>404 Not Found.</html>");
    return;
}
```

如果文件不存在，返回 `404`。

### 19.5 获取文件大小

```cpp
struct stat statbuf;
fstat(fd, &statbuf);
size_t size = statbuf.st_size;
```

`fstat()` 可以根据文件描述符获取文件信息。

### 19.6 申请内存

```cpp
char* buf = (char*)malloc(size);
assert(buf != NULL && "malloc failed");
```

文件内容会被读取到 `buf` 中。

### 19.7 设置 HTTP 任务结束回调释放内存

```cpp
httpTask->set_callback([buf](WFHttpTask*) {
    free(buf);
});
```

这里很重要。

后面使用的是：

```cpp
resp->append_output_body_nocopy(args->buf, bytesRead);
```

`append_output_body_nocopy()` 的意思是：响应体直接引用这块内存，不再复制一份。

所以不能在 `pread_callback()` 中立刻 `free(buf)`，否则 HTTP 响应还没发完，内存就已经释放了。

正确做法是：等整个 HTTP 任务结束后再释放。

### 19.8 创建 pread 文件 IO 任务

```cpp
WFFileIOTask* preadTask = WFTaskFactory::create_pread_task(
    fd,
    buf,
    size,
    0,
    bind(pread_callback, _1, resp, filename));
```

参数解释：

| 参数 | 含义 |
| --- | --- |
| `fd` | 文件描述符 |
| `buf` | 读取到哪里 |
| `size` | 读取多少字节 |
| `0` | 从文件偏移 0 开始读 |
| `pread_callback` | 读取完成后的回调函数 |

### 19.9 加入 HTTP 任务序列

```cpp
series_of(httpTask)->push_back(preadTask);
```

这样 workflow 会先执行 `preadTask`，读取完成后再发送响应。

## 20. 文件读取回调详解

文件读取回调：

```cpp
void pread_callback(WFFileIOTask* task, HttpResponse* resp, string filename)
```

### 20.1 获取参数

```cpp
FileIOArgs* args = task->get_args();
close(args->fd);
long bytesRead = task->get_retval();
```

`task->get_args()` 可以拿到创建 `pread` 任务时传入的参数。

`close(args->fd)` 关闭文件描述符。

`get_retval()` 是实际读取的字节数。

### 20.2 判断读取是否成功

```cpp
int state = task->get_state();
if (state != WFT_STATE_SUCCESS) {
    resp->set_status_code("500");
    resp->append_output_body("<html>500 Server Internal Error.</html>");
    return;
}
```

如果文件读取失败，返回 500。

### 20.3 设置下载文件名

```cpp
resp->add_header_pair("Content-Disposition", "attachment; filename=" + filename);
```

这个响应头告诉浏览器：这是一个附件下载，文件名是 `filename`。

### 20.4 写入响应体

```cpp
resp->append_output_body_nocopy(args->buf, bytesRead);
```

把文件内容放入 HTTP 响应体。

## 21. 辅助函数说明

### 21.1 `set_text_response`

```cpp
void set_text_response(HttpResponse* resp, const string& code, const string& body)
```

作用：

1. 设置 HTTP 状态码。
2. 设置响应类型为普通文本。
3. 写入响应体。

用于返回错误信息或简单提示信息。

### 21.2 `set_unauthorized`

```cpp
void set_unauthorized(HttpResponse* resp)
```

作用：统一返回 401。

用于：

1. 用户不存在。
2. 密码错误。
3. 没有 Token。
4. Token 错误。
5. Token 超时。

### 21.3 `get_request_body`

```cpp
string get_request_body(HttpRequest* req)
```

作用：从 workflow 已解析好的 HTTP 请求中取出请求体。

核心代码：

```cpp
const void* body = NULL;
size_t size = 0;

if (req->get_parsed_body(&body, &size) && body != NULL) {
    return string((const char*)body, size);
}
```

### 21.4 `parse_form`

```cpp
map<string, string> parse_form(const string& body)
```

作用：解析简单表单。

输入：

```text
username=alice&password=123456
```

输出：

```text
form["username"] = "alice"
form["password"] = "123456"
```

这个函数也被用来解析 URL 查询参数：

```text
token=xxxxx
```

### 21.5 `url_decode`

```cpp
string url_decode(const string& text)
```

作用：把 URL 编码后的字符串还原。

例如：

```text
hello+world  -> hello world
%41          -> A
```

### 21.6 `sql_escape`

```cpp
string sql_escape(const string& text)
```

作用：对 SQL 字符串中的单引号和反斜杠做简单转义。

例如：

```text
O'Reilly
```

会变成：

```text
O''Reilly
```

这样拼接 SQL 时不会因为单引号导致语句提前结束。

说明：正式生产项目应使用参数化 SQL 或 MySQL 官方转义函数。为了保持代码直观，当前只做了简单处理。

### 21.7 `get_header_value`

```cpp
string get_header_value(HttpRequest* req, const string& name)
```

作用：从 HTTP 请求头中查找指定字段。

例如查找：

```text
Authorization
```

核心代码：

```cpp
HttpHeaderCursor cursor(req);
if (cursor.find(name, value)) {
    return value;
}
```

### 21.8 `get_resource_root`

```cpp
string get_resource_root()
```

作用：判断静态资源目录在哪里。

如果从项目根目录启动：

```text
resources
```

如果从 `Static_Resource_Server` 目录启动：

```text
../resources
```

这个函数用 `static string root` 缓存结果，所以只在第一次调用时判断一次。

## 22. workflow 任务串联说明

workflow 的一个核心概念是：任务可以被串起来执行。

本程序中有两种典型串联。

### 22.1 注册和登录中的 MySQL 任务

以登录为例：

```cpp
WFMySQLTask* mysqlTask = WFTaskFactory::create_mysql_task(...);
mysqlTask->get_req()->set_query(query);
series_of(httpTask)->push_back(mysqlTask);
```

执行顺序：

```text
HTTP 请求进入 process()
    |
    v
handle_login()
    |
    v
创建 MySQL 查询任务
    |
    v
把 MySQL 任务 push_back 到当前序列
    |
    v
MySQL 查询完成
    |
    v
login_callback()
    |
    v
HTTP 响应返回给客户端
```

### 22.2 静态资源下载中的文件 IO 任务

```cpp
WFFileIOTask* preadTask = WFTaskFactory::create_pread_task(...);
series_of(httpTask)->push_back(preadTask);
```

执行顺序：

```text
HTTP 请求进入 process()
    |
    v
Token 校验成功
    |
    v
handle_static_file()
    |
    v
创建 pread 文件读取任务
    |
    v
把 pread 任务 push_back 到当前序列
    |
    v
pread_callback()
    |
    v
HTTP 响应返回文件内容
```

## 23. 为什么没有过度工程化

本项目的目标是学习：

1. HTTP 请求和响应。
2. workflow HTTP Server。
3. workflow MySQL Task。
4. workflow File IO Task。
5. 密码加盐哈希。
6. Token 登录状态验证。

因此代码没有拆成复杂的类，也没有引入路由框架、配置文件系统、连接池封装或高级安全库。

当前写法更适合初学者：

1. 所有主逻辑都在 `static_resource_server.cc`。
2. 每个函数只负责一段清晰流程。
3. 注册、登录、下载三个业务流程分开。
4. 注释按执行顺序解释。
5. 可以直接对照 curl 请求观察服务器行为。

## 24. 重要注意事项

### 24.1 Token 放在 URL 中只适合测试

程序支持：

```text
/index.html?token=xxx
```

这是为了方便浏览器地址栏测试。

真实项目更推荐：

```http
Authorization: Bearer xxx
```

因为 URL 可能出现在浏览器历史记录、日志和代理记录中。

### 24.2 SQL 拼接只为保持代码直观

本程序使用字符串拼接 SQL，并写了简单的 `sql_escape()`。

正式项目中更推荐：

1. 参数化 SQL。
2. 预编译语句。
3. 数据库连接池。
4. 更严格的输入校验。

当前代码没有引入这些内容，是为了避免代码过于复杂。

### 24.3 密码不会明文保存

数据库中的 `password` 字段保存的是哈希值。

注册时：

```text
用户明文密码 + 随机 salt -> SHA256 -> 保存哈希
```

登录时：

```text
用户输入密码 + 数据库 salt -> SHA256 -> 和数据库哈希比较
```

### 24.4 401 的含义

本程序中这些情况都会返回 `401 unauthorized`：

1. 下载资源时没有 Token。
2. Token 格式错误。
3. Token 签名错误。
4. Token 超过 30 分钟。
5. 登录时用户名不存在。
6. 登录时密码错误。

## 25. 常见问题

### 25.1 为什么浏览器不能直接测试注册和登录？

浏览器地址栏默认发 GET 请求。

注册和登录接口要求 POST：

```text
POST /register
POST /login
```

所以建议用 `curl` 测试注册和登录，用浏览器地址栏测试带 Token 的静态资源下载。

### 25.2 为什么下载文件时浏览器可能直接下载？

因为服务器设置了：

```cpp
Content-Disposition: attachment; filename=文件名
```

这个响应头会提示浏览器把内容当作附件。

### 25.3 为什么运行时提示端口启动失败？

如果看到：

```text
ERROR: Server start FAILED!
```

常见原因是 `8888` 端口已经被其它程序占用。

可以关闭占用端口的程序，或者修改源码中的端口：

```cpp
static const int SERVER_PORT = 8888;
```

### 25.4 为什么登录失败返回 401，而不是提示用户名或密码哪个错？

这是常见做法。

如果明确告诉“用户名不存在”或“密码错误”，攻击者可以借此猜测哪些用户名存在。

本程序中统一返回：

```text
401 unauthorized
```

代码也更简单。

## 26. 总结

这个静态资源服务器的核心可以概括为三条链路。

注册链路：

```text
POST /register
  -> 解析 username/password
  -> 生成 salt
  -> 计算密码哈希
  -> INSERT tbl_user
  -> 返回 register ok
```

登录链路：

```text
POST /login
  -> 解析 username/password
  -> SELECT tbl_user
  -> 使用数据库 salt 重新计算哈希
  -> 比较密码
  -> 生成 Token
  -> 返回 Token
```

下载链路：

```text
GET /资源路径
  -> 读取 Token
  -> 验证 Token 签名和超时时间
  -> 映射到 resources 目录
  -> open/fstat/malloc
  -> workflow pread 读取文件
  -> 返回文件内容
```

理解这三条链路，就可以理解整个程序的代码。
