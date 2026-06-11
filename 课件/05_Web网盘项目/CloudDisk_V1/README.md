# CloudDisk 第一期技术文档：基本功能实现

本文档说明 `CloudDisk_V1` 网盘项目第一期的后端实现。

第一期的核心目标是：实现一个能被前端页面正常调用的个人网盘后端。它支持用户注册、登录、获取当前用户信息、查看文件列表、上传文件、下载文件。真实文件内容保存到服务器本地磁盘，MySQL 保存用户信息和文件元数据。

读完本文档后，你应该能掌握：

1. 第一期项目整体由哪些模块组成，每个模块负责什么。
2. wfrest 如何注册静态资源路由和 API 路由。
3. 前端和后端约定的统一 JSON 响应格式是什么。
4. `tbl_user` 和 `tbl_file` 两张表分别保存什么数据。
5. 注册接口如何完成 JSON 校验、密码加盐哈希、写入 MySQL。
6. 登录接口如何查询用户、校验密码、生成 JWT。
7. 后续接口为什么只定义 `User user` 就能访问 `user.id`。
8. 文件列表、上传、下载接口如何通过 token 中的用户 id 做权限控制。
9. `req->form()`、`req->param<int>("id")`、`resp->MySQL()`、`resp->File()` 的作用。
10. 为什么 `MYSQL_STATUS_GET_RESULT`、`MYSQL_STATUS_OK`、`get_rows_count()`、`fetch_row()` 能用于判断不同错误。
11. 本地文件保存路径 `./storage/{uid}/{hashcode}` 的设计含义。
12. 如何编译、运行、测试接口，以及常见错误如何排查。

本文档主要讲后端。前端页面已经写好，负责浏览器中的注册、登录、文件列表、上传和下载交互。

---

## 一、第一期实现了什么

第一期是整个 Web 网盘项目的基础版本，功能如下：

```text
用户注册
用户登录
获取当前用户信息
查询当前用户的文件列表
上传文件到服务器本地磁盘
从服务器本地磁盘下载文件
```

后端使用：

```text
wfrest          HTTP 服务器和路由
workflow        wfrest 底层异步任务框架，包含 MySQL 任务能力
nlohmann/json   JSON 解析和生成
OpenSSL EVP     密码哈希、文件哈希
libjwt          JWT 生成和校验
MySQL           用户数据和文件元数据
本地磁盘         真实文件内容
```

第一期文件保存位置：

```text
./storage/{uid}/{hashcode}
```

示例：

```text
uid      = 3
hashcode = 0d1cfa62d86f...

真实文件路径：
./storage/3/0d1cfa62d86f...
```

注意：第一期适合学习后端 API 开发流程。第二期会把真实文件内容从本地磁盘迁移到阿里云 OSS。

---

## 二、整体架构

第一期后端架构如下：

```text
浏览器前端
  |
  | HTTP 请求
  v
CloudDiskServer
  |
  | 用户、文件元数据
  v
MySQL

CloudDiskServer
  |
  | 文件二进制内容
  v
服务器本地磁盘 ./storage
```

按照接口分类：

```text
注册 / 登录
  -> CloudDiskServer
  -> MySQL tbl_user

获取当前用户信息
  -> CloudDiskServer
  -> 校验 JWT
  -> 从 JWT 还原用户信息

文件列表
  -> CloudDiskServer
  -> 校验 JWT
  -> MySQL tbl_file

上传文件
  -> CloudDiskServer
  -> 校验 JWT
  -> 解析 multipart/form-data
  -> 计算文件 hashcode
  -> 写入 ./storage/{uid}/{hashcode}
  -> MySQL tbl_file INSERT 元数据

下载文件
  -> CloudDiskServer
  -> 校验 JWT
  -> MySQL tbl_file 确认 fileId 属于当前用户
  -> 从 ./storage/{uid}/{hashcode} 读取文件
  -> 返回文件内容给浏览器
```

后端最重要的原则是：不要相信客户端传来的数据。

因此文件相关接口不会让前端传 `uid`，而是从 JWT 中解析当前登录用户 id。这样用户只能查看、上传、下载自己的文件。

---

## 三、目录结构

`CloudDisk_V1` 主要文件如下：

```text
CloudDisk_V1/
├── CMakeLists.txt
├── CloudDiskServer.cc
├── CloudDiskServer.h
├── CryptoUtil.cc
├── CryptoUtil.h
├── build.sh
├── main.cc
├── server
└── www/
    ├── index.html
    └── static/
        ├── api.js
        ├── login.html
        ├── register.html
        └── style.css
```

后端重点文件：

```text
CloudDiskServer.cc
  HTTP 路由、请求校验、MySQL 查询、本地文件保存和下载、统一 JSON 响应。

CloudDiskServer.h
  CloudDiskServer 类声明，内部组合 wfrest::HttpServer。

CryptoUtil.cc / CryptoUtil.h
  salt 生成、密码哈希、文件 hashcode、JWT 生成和校验。

main.cc
  创建 CloudDiskServer，注册路由，启动 8888 端口。

CMakeLists.txt
  编译 server，并链接 crypto、ssl、jwt、wfrest。

build.sh
  简单编译脚本。

www/
  前端页面和静态资源。
```

运行后，上传文件会自动产生本地目录：

```text
CloudDisk_V1/storage/
```

这个目录不是手写源码，而是运行时保存用户上传文件的目录。

---

## 四、数据库设计

第一期只涉及两张表：

```text
tbl_user
tbl_file
```

### 1. 用户表 `tbl_user`

建表语句：

```sql
CREATE TABLE tbl_user(
    id int PRIMARY KEY AUTO_INCREMENT,
    username varchar(255) NOT NULL UNIQUE,
    password varchar(255) NOT NULL,
    salt varchar(64) NOT NULL,
    created_at datetime DEFAULT CURRENT_TIMESTAMP(),
    tomb int DEFAULT 0
);
```

字段含义：

```text
id
  用户 id，主键，自增。

username
  用户名，必须唯一。

password
  密码哈希值，不保存明文密码。

salt
  每个用户独立的随机盐值。

created_at
  用户创建时间。

tomb
  逻辑删除标记，0 表示正常用户。
```

注册时写入：

```text
username
password = hash_password(用户输入密码, salt)
salt
```

登录时读取：

```text
id
username
password
salt
created_at
```

然后用数据库中的 `salt` 重新计算用户输入密码的哈希值，和数据库中的 `password` 比较。

### 2. 文件表 `tbl_file`

建表语句：

```sql
CREATE TABLE tbl_file(
    id int PRIMARY KEY AUTO_INCREMENT,
    uid int NOT NULL,
    filename varchar(255) NOT NULL,
    hashcode varchar(255) NOT NULL,
    size int DEFAULT 0,
    created_at datetime DEFAULT CURRENT_TIMESTAMP(),
    last_update datetime DEFAULT CURRENT_TIMESTAMP() ON UPDATE CURRENT_TIMESTAMP(),
    UNIQUE KEY (uid, filename)
);
```

字段含义：

```text
id
  文件 id，主键，自增。前端下载时使用 /api/v1/file/{id}。

uid
  文件属于哪个用户。对应 tbl_user.id。

filename
  用户上传时的原始文件名，例如 a.txt。

hashcode
  根据文件内容计算出来的哈希值。后端用它定位本地真实文件。

size
  文件大小，单位是字节。

created_at
  文件上传时间。

last_update
  文件最后更新时间。
```

`UNIQUE KEY (uid, filename)` 表示：

```text
同一个用户不能上传两个同名文件。
不同用户可以上传同名文件。
```

示例：

```text
用户 3 上传 a.txt

tbl_file:
  uid      = 3
  filename = a.txt
  hashcode = 0d1cfa62...
  size     = 734

本地磁盘:
  ./storage/3/0d1cfa62...
```

MySQL 保存的是元数据；真实文件内容保存在本地磁盘。

---

## 五、核心类：`CloudDiskServer`

`CloudDiskServer.h` 中定义了一个服务器类：

```cpp
class CloudDiskServer {
public:
    CloudDiskServer() { }

    void register_routes();

    int start(unsigned short port)
    {
        return server_.start(port);
    }

    void stop() { server_.stop(); }
    void list_routes() { server_.list_routes(); }

private:
    void register_www_module();
    void register_auth_module();
    void register_user_module();
    void register_file_module();

private:
    wfrest::HttpServer server_;
};
```

这个类是对 `wfrest::HttpServer` 的一层封装。

这样做的好处：

```text
main.cc
  只负责创建服务、注册路由、启动端口。

CloudDiskServer
  专门负责路由组织和后端业务逻辑。
```

`CloudDiskServer` 内部组合了一个 `wfrest::HttpServer`：

```cpp
wfrest::HttpServer server_;
```

组合的含义是：`CloudDiskServer` 不是继承 `HttpServer`，而是“拥有一个” `HttpServer`。对学习项目来说，这样能把服务启动接口包装得更清楚。

---

## 六、程序启动流程

`main.cc`：

```cpp
int main()
{
    signal(SIGINT, sig_handler);
    srand(time(NULL));

    CloudDiskServer server;
    server.register_routes();

    if (server.start(8888) == 0) {
        waitGroup.wait();
        server.stop();
    } else {
        cerr << "Error: Server start FAILED!" << endl;
    }
}
```

生命周期：

```text
程序启动
  -> 注册 SIGINT 信号处理函数
  -> srand(time(NULL)) 初始化随机数种子
  -> 创建 CloudDiskServer server
  -> register_routes() 注册所有路由
  -> start(8888) 监听 HTTP 请求
  -> 浏览器访问前端和 API
  -> Ctrl+C 触发 sig_handler()
  -> waitGroup.done()
  -> waitGroup.wait() 返回
  -> server.stop()
  -> 程序退出
```

`WFFacilities::WaitGroup waitGroup(1)` 的作用是让主线程阻塞等待。否则 `server.start(8888)` 成功后，`main()` 很快结束，服务进程也会退出。

`SIGINT` 通常来自 Ctrl+C。按 Ctrl+C 后，信号处理函数调用：

```cpp
waitGroup.done();
```

主线程从：

```cpp
waitGroup.wait();
```

返回，然后执行：

```cpp
server.stop();
```

---

## 七、路由注册

`register_routes()`：

```cpp
void CloudDiskServer::register_routes()
{
    register_www_module();
    register_auth_module();
    register_user_module();
    register_file_module();
}
```

路由按功能拆成四组：

```text
register_www_module()
  静态资源。

register_auth_module()
  注册、登录。

register_user_module()
  当前用户信息。

register_file_module()
  文件列表、上传、下载。
```

### 1. 静态资源路由

```cpp
server_.Static("/", "./www/index.html");
server_.Static("/static", "./www/static");
```

含义：

```text
GET /
  返回 ./www/index.html

GET /static/xxx
  返回 ./www/static/xxx
```

前端页面中 `api.js` 会调用 `/api/v1/...` 接口。静态资源和 API 共用同一个 8888 端口。

### 2. API 路由总览

```text
POST /api/v1/auth/register
POST /api/v1/auth/login
GET  /api/v1/user/me
GET  /api/v1/files
POST /api/v1/files
GET  /api/v1/file/{id}
```

其中：

```text
/api/v1
  API 版本前缀。

auth
  认证模块。

user
  用户模块。

files
  文件集合，列表和上传。

file/{id}
  单个文件，下载。
```

---

## 八、统一响应格式

前端 `www/static/api.js` 中会执行：

```js
const response = await fetch(...);
const result = await response.json();
```

所以 API 错误不能返回普通字符串或 HTML，必须返回 JSON。

后端封装了三个工具函数：

```cpp
static void response_json(HttpResp* resp, int status_code, const json& body)
static void response_success(HttpResp* resp, int status_code, const string& message, const json& data)
static void response_error(HttpResp* resp, int status_code, const string& message)
```

成功格式：

```json
{
  "status": "success",
  "message": "登录成功",
  "data": {}
}
```

失败格式：

```json
{
  "status": "error",
  "message": "无效的访问令牌"
}
```

`response_json()` 做三件事：

```cpp
resp->set_status(status_code);
resp->add_header("Content-Type", "application/json");
resp->String(body.dump());
```

含义：

```text
set_status
  设置 HTTP 状态码，例如 200、400、401、500。

add_header("Content-Type", "application/json")
  告诉浏览器响应体是 JSON。

String(body.dump())
  把 nlohmann::json 序列化成字符串，写入 HTTP 响应体。
```

下载接口是唯一例外：成功时返回文件内容，不返回 JSON；失败时仍返回 JSON 错误。

---

## 九、JSON 请求解析

注册和登录接口要求：

```http
Content-Type: application/json
```

工具函数：

```cpp
static bool parse_json_body(const HttpReq* req, json& body)
{
    if (req->content_type() != APPLICATION_JSON) {
        return false;
    }

    body = json::parse(req->body(), nullptr, false);
    return !body.is_discarded();
}
```

这段代码有两个检查：

```text
1. req->content_type() != APPLICATION_JSON
   请求头不是 application/json，直接认为请求格式错误。

2. body.is_discarded()
   Content-Type 虽然是 JSON，但请求体内容不是合法 JSON。
```

`json::parse(req->body(), nullptr, false)` 的第三个参数 `false` 表示解析失败时不抛异常，而是返回一个 `discarded` 状态的 JSON 对象。教学项目这样写更容易理解，不需要额外写 try-catch。

读取字符串字段：

```cpp
static string json_string(const json& body, const string& key)
{
    if (!body.contains(key) || !body[key].is_string()) {
        return "";
    }
    return body[key].get<string>();
}
```

如果字段不存在或不是字符串，就返回空字符串。接口中再统一判断：

```cpp
if (username.empty() || password.empty()) {
    response_error(resp, HttpStatusBadRequest, "用户名和密码不能为空");
    return;
}
```

---

## 十、SQL 字符串处理

项目中 SQL 使用字符串拼接，例如：

```cpp
"WHERE username='" + escape_sql(username) + "'"
```

工具函数：

```cpp
static string escape_sql(const string& s)
```

作用是处理字符串中的：

```text
单引号 '
反斜线 \
```

例如用户名：

```text
abc'def
```

如果直接拼 SQL：

```sql
WHERE username='abc'def'
```

SQL 字符串会被中途截断，甚至可能产生 SQL 注入风险。

`escape_sql()` 会把它处理成：

```sql
WHERE username='abc\'def'
```

说明：正式项目应使用参数化查询或预编译语句。这里是教学项目，为了保持代码直观，使用简单转义函数降低风险。

---

## 十一、密码、salt 和 hash

`CryptoUtil` 负责密码哈希、文件哈希和 JWT。

### 1. salt 是什么

salt 是每个用户注册时生成的一段随机字符串：

```cpp
string salt = CryptoUtil::generate_salt();
```

默认长度是 8：

```cpp
static std::string generate_salt(int length = 8);
```

如果两个用户密码都设置为 `123456`，但 salt 不同，最终保存到数据库的密码哈希也会不同。

### 2. 注册时如何保存密码

注册流程：

```cpp
string salt = CryptoUtil::generate_salt();
string password_hash = CryptoUtil::hash_password(password, salt);
```

数据库保存：

```text
password = password_hash
salt     = salt
```

后端不保存明文密码。

### 3. 登录时如何校验密码

登录时从数据库读取：

```text
password  数据库中保存的密码哈希
salt      数据库中保存的盐值
```

然后用用户输入的密码重新计算：

```cpp
string input_hash = CryptoUtil::hash_password(password, user.salt);
```

最后比较：

```cpp
input_hash == user.password
```

如果一致，说明密码正确；否则返回：

```json
{
  "status": "error",
  "message": "用户名或密码错误"
}
```

### 4. `hash_password()` 的内部逻辑

`CryptoUtil::hash_password()` 使用 OpenSSL EVP 接口：

```cpp
EVP_DigestUpdate(context, salt.c_str(), salt.size());
EVP_DigestUpdate(context, password.c_str(), password.size());
```

也就是计算：

```text
hash(salt + password)
```

然后把二进制哈希结果转成十六进制字符串，便于保存到 MySQL。

---

## 十二、JWT 登录态

登录成功后，后端生成 JWT：

```cpp
string token = CryptoUtil::generate_token(user);
```

JWT 中保存：

```text
sub        = LoginToken
id         = 用户 id
username   = 用户名
created_at = 注册时间
exp        = 过期时间，当前为 1 小时
```

前端保存 token：

```js
localStorage.setItem('accessToken', response.data.accessToken);
```

后续请求带上：

```http
Authorization: Bearer xxxxxx
```

### 1. 解析 Bearer Token

后端函数：

```cpp
static bool get_bearer_token(const HttpReq* req, string& token)
```

要求请求头必须类似：

```http
Authorization: Bearer eyJhbGciOiJIUzI1Ni...
```

如果没有 `Authorization`、不是 `Bearer ` 开头、或者 token 为空，都返回 false。

### 2. 校验登录态

后端函数：

```cpp
static bool check_login(const HttpReq* req, User& user)
```

它做三件事：

```text
1. get_bearer_token(req, token)
   从请求头取出 token。

2. CryptoUtil::verify_token(token, user)
   校验 token 签名、主题 sub、过期时间 exp。

3. verify_token 成功后，把 token 中的 id、username、created_at 写入 user。
```

这解释了一个常见疑问：

```cpp
User user;
if (!check_login(req, user)) {
    ...
}

cout << user.id << endl;
```

为什么定义了一个 `User user` 后，就能访问 `user.id`？

原因不是“登录时的 user 一直存在”。HTTP 是无状态的，每次请求都是新的。这里的 `user` 是本次请求中的局部变量。`check_login(req, user)` 会从本次请求头中的 JWT 解码出用户信息，并写入这个局部变量。

所以：

```text
登录时：
  数据库查出的 User -> generate_token(user) -> token 发给前端

后续请求：
  前端带 token -> verify_token(token, user) -> 重新填充本次请求的 User
```

### 3. 为什么后续接口不让前端传 uid

文件列表、上传、下载都使用：

```cpp
User user;
check_login(req, user);
```

然后使用：

```cpp
user.id
```

作为当前用户 id。

这样做比让前端传 `uid` 安全得多。因为前端传来的 `uid` 可以被用户随意修改，而 JWT 需要后端密钥签名，用户不能伪造。

---

## 十三、MySQL 回调和结果判断

wfrest 提供：

```cpp
resp->MySQL(DatabaseURL, sql, callback);
```

它会发起一个异步 MySQL 任务。SQL 执行完成后，进入回调函数：

```cpp
[](MySQLResultCursor* cursor) {
    ...
}
```

### 1. 为什么 lambda 要按值捕获

例如注册接口：

```cpp
resp->MySQL(DatabaseURL, sql, [resp, username](MySQLResultCursor* cursor) {
    ...
});
```

`username` 要按值捕获，而不是引用捕获。

原因：

```text
resp->MySQL 是异步任务。
外层 HTTP 回调函数返回后，局部变量 username 会销毁。
MySQL 回调可能稍后才执行。
如果用引用捕获，就可能引用已经销毁的变量。
```

登录接口中的 `password` 同理：

```cpp
[resp, password](MySQLResultCursor* cursor)
```

### 2. `MYSQL_STATUS_OK`

`INSERT`、`UPDATE`、`DELETE` 这类不返回结果集的 SQL，执行成功通常是：

```cpp
cursor->get_cursor_status() == MYSQL_STATUS_OK
```

注册接口执行：

```sql
INSERT INTO tbl_user ...
```

上传接口执行：

```sql
INSERT INTO tbl_file ...
```

所以这两个接口用 `MYSQL_STATUS_OK` 判断写入是否成功。

### 3. `MYSQL_STATUS_GET_RESULT`

`SELECT` 查询成功时会返回结果集，对应状态是：

```cpp
cursor->get_cursor_status() == MYSQL_STATUS_GET_RESULT
```

如果 SELECT 后不是这个状态，说明不是“查不到数据”，而是 SQL 执行本身失败，例如：

```text
数据库连接失败
SQL 语法错误
表名或字段名写错
MySQL 服务异常
```

这些属于服务器端问题，所以返回：

```json
{
  "status": "error",
  "message": "内部服务器错误"
}
```

HTTP 状态码是 500。

### 4. `get_rows_count()`

```cpp
cursor->get_rows_count()
```

表示 SELECT 结果集中有多少行。

登录接口中：

```cpp
if (cursor->get_rows_count() == 0) {
    response_error(resp, HttpStatusUnauthorized, "用户名或密码错误");
    return;
}
```

含义：

```text
SQL 执行成功，但没有查到这个用户名对应的正常用户。
```

登录失败时不区分“用户名不存在”和“密码错误”，统一返回 `用户名或密码错误`。这样可以避免攻击者通过接口探测哪些用户名已经注册。

下载接口中：

```cpp
WHERE id = file_id AND uid = 当前登录用户id
```

如果结果行数为 0，可能是：

```text
文件 id 不存在
文件存在但不属于当前用户
```

这两种情况都不能下载，所以返回 404 `文件不存在`。

### 5. `fetch_row(row)`

```cpp
vector<MySQLCell> row;
if (!cursor->fetch_row(row)) {
    ...
}
```

`fetch_row(row)` 的作用是从结果集中真正读取一行数据，放到 `row` 中。

`get_rows_count()` 和 `fetch_row(row)` 不是完全重复：

```text
get_rows_count()
  判断结果集理论上有多少行，业务含义清楚。

fetch_row(row)
  真正读取一行数据，是访问 row[0]、row[1] 前的保护。
```

如果不检查 `fetch_row(row)`，后面直接访问：

```cpp
row[0].as_int()
```

在 row 为空时可能越界，导致程序崩溃。

### 6. 文件列表为什么不判断 `get_rows_count() == 0`

文件列表接口：

```cpp
while (cursor->fetch_row(row)) {
    ...
}
```

如果用户没有任何文件，`fetch_row(row)` 第一次就返回 false，循环不进入，最终返回：

```json
{
  "files": []
}
```

这不是错误，而是正常状态。

---

## 十四、本地文件存储设计

第一期真实文件保存到本地磁盘：

```cpp
static const string FileStorageRoot = "./storage";
```

用户目录：

```cpp
static string user_storage_dir(int uid)
{
    return FileStorageRoot + "/" + to_string(uid);
}
```

真实文件路径：

```cpp
static string storage_file_path(int uid, const string& hashcode)
{
    return user_storage_dir(uid) + "/" + hashcode;
}
```

示例：

```text
uid      = 3
filename = 笔记.txt
hashcode = a948904f2f0f479...

数据库 tbl_file:
  uid      = 3
  filename = 笔记.txt
  hashcode = a948904f2f0f479...

本地磁盘:
  ./storage/3/a948904f2f0f479...
```

为什么不用原始文件名保存？

```text
原始文件名可能包含空格、中文、特殊字符。
不同用户可以上传同名文件。
后端更适合用稳定的 hashcode 定位真实内容。
原始文件名仍保存在 MySQL，下载时通过 Content-Disposition 还给浏览器。
```

### 1. 创建目录

上传前创建用户目录：

```cpp
std::error_code ec;
filesystem::create_directories(dir, ec);
if (ec) {
    response_error(resp, HttpStatusInternalServerError, "内部服务器错误");
    return;
}
```

`create_directories()` 可以递归创建多级目录。

例如：

```text
dir = ./storage/3
```

如果 `storage` 不存在，它会先创建 `storage`，再创建 `storage/3`。

`ec` 用来接收错误。如果没有权限、磁盘异常等导致目录创建失败，`ec` 会表示错误，接口返回 500。

### 2. 写入文件

```cpp
ofstream ofs(real_path, ios::binary);
ofs.write(content.data(), content.size());
ofs.close();
```

`ios::binary` 表示按二进制写入。无论上传的是 txt、jpg、pdf、zip，都按原始字节保存，不做文本转换。

---

## 十五、路由和接口详解

### 1. 注册

```text
POST /api/v1/auth/register
```

请求体：

```json
{
  "username": "alice",
  "password": "123456",
  "confirm": "123456"
}
```

处理流程：

```text
1. parse_json_body(req, body)
   检查 Content-Type，解析 JSON。

2. json_string(body, "username")
   取用户名。

3. json_string(body, "password")
   取密码。

4. json_string(body, "confirm")
   取确认密码。

5. 检查 username/password 非空。

6. 检查 password == confirm。

7. generate_salt()
   生成 salt。

8. hash_password(password, salt)
   生成密码哈希。

9. INSERT tbl_user
   写入用户表。

10. 返回 userId 和 username。
```

成功响应：

```http
HTTP/1.1 201 Created
Content-Type: application/json
```

```json
{
  "status": "success",
  "message": "注册成功",
  "data": {
    "userId": 1,
    "username": "alice"
  }
}
```

错误响应：

| 场景 | HTTP 状态码 | message |
| --- | --- | --- |
| 请求类型不是 JSON 或 JSON 解析失败 | 400 | 请求格式有误 |
| 用户名或密码为空 | 400 | 用户名和密码不能为空 |
| 两次密码不一致 | 400 | 两次输入的密码不一致 |
| INSERT 失败，常见为用户名重复 | 409 | 用户名已存在 |

说明：`tbl_user.username` 有唯一约束，所以重复用户名会导致 INSERT 失败。

### 2. 登录

```text
POST /api/v1/auth/login
```

请求体：

```json
{
  "username": "alice",
  "password": "123456"
}
```

处理流程：

```text
1. 检查 JSON 请求体。
2. 取 username/password。
3. 检查 username/password 非空。
4. SELECT tbl_user WHERE username = ? AND tomb = 0。
5. 如果 SQL 执行失败，返回 500。
6. 如果结果集为空，返回 401 用户名或密码错误。
7. fetch_row(row) 取出用户记录。
8. 用数据库中的 salt 重新计算输入密码的哈希。
9. 比较哈希。
10. 生成 JWT。
11. 返回 accessToken。
```

查询 SQL：

```sql
SELECT id, username, password, salt, created_at
FROM tbl_user
WHERE username='alice' AND tomb=0
LIMIT 1;
```

成功响应：

```json
{
  "status": "success",
  "message": "登录成功",
  "data": {
    "accessToken": "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9...",
    "tokenType": "Bearer",
    "user": {
      "userId": 1,
      "username": "alice"
    }
  }
}
```

错误响应：

| 场景 | HTTP 状态码 | message |
| --- | --- | --- |
| 请求类型不是 JSON 或 JSON 解析失败 | 400 | 请求格式有误 |
| 用户名或密码为空 | 400 | 用户名和密码不能为空 |
| SQL 执行失败 | 500 | 内部服务器错误 |
| 用户不存在 | 401 | 用户名或密码错误 |
| 取行失败 | 401 | 用户名或密码错误 |
| 密码哈希不匹配 | 401 | 用户名或密码错误 |

### 3. 获取当前用户信息

```text
GET /api/v1/user/me
```

请求头：

```http
Authorization: Bearer xxxxx
```

处理流程：

```text
1. check_login(req, user)
2. 从 JWT 中还原 user.id、user.username、user.createdAt
3. 返回当前用户信息
```

成功响应：

```json
{
  "status": "success",
  "message": "获取个人信息成功",
  "data": {
    "userId": 1,
    "username": "alice",
    "createdAt": "2026-06-11 10:00:00"
  }
}
```

错误响应：

| 场景 | HTTP 状态码 | message |
| --- | --- | --- |
| 没有 token | 401 | 无效的访问令牌 |
| token 类型不是 Bearer | 401 | 无效的访问令牌 |
| token 校验失败或过期 | 401 | 无效的访问令牌 |

本接口不查 MySQL。它信任通过后端密钥校验的 JWT。

### 4. 文件列表

```text
GET /api/v1/files
```

请求头：

```http
Authorization: Bearer xxxxx
```

查询 SQL：

```sql
SELECT id, filename, size, created_at, last_update
FROM tbl_file
WHERE uid = 当前登录用户id
ORDER BY last_update DESC, id DESC;
```

处理流程：

```text
1. check_login(req, user)
2. 用 user.id 作为 uid 查询 tbl_file
3. 遍历结果集
4. 组装 data.files 数组
5. 返回文件列表
```

成功响应：

```json
{
  "status": "success",
  "message": "获取文件列表成功",
  "data": {
    "files": [
      {
        "fileId": 1,
        "filename": "a.txt",
        "size": 1024,
        "createdAt": "2026-06-11 10:00:00",
        "updatedAt": "2026-06-11 10:00:00"
      }
    ]
  }
}
```

如果用户没有文件：

```json
{
  "status": "success",
  "message": "获取文件列表成功",
  "data": {
    "files": []
  }
}
```

错误响应：

| 场景 | HTTP 状态码 | message |
| --- | --- | --- |
| token 无效 | 401 | 无效的访问令牌 |
| SQL 执行失败 | 500 | 内部服务器错误 |

### 5. 上传文件

```text
POST /api/v1/files
```

请求头：

```http
Authorization: Bearer xxxxx
Content-Type: multipart/form-data; boundary=...
```

前端代码：

```js
const formData = new FormData();
formData.append('file', file);
return uploadRequest('/files', formData);
```

后端解析：

```cpp
Form& form = req->form();
string filename = form["file"].first;
string content = form["file"].second;
```

`Form` 可以理解为：

```text
map<string, pair<string, string>>
```

对文件字段 `file` 来说：

```text
form["file"].first
  原始文件名，例如 a.txt。

form["file"].second
  文件二进制内容。
```

处理流程：

```text
1. check_login(req, user)
2. 检查 Content-Type 必须是 multipart/form-data
3. req->form() 解析 multipart 请求体
4. 检查表单中必须有 file 字段
5. 取 filename 和 content
6. 检查 filename 非空
7. generate_hashcode(content.data(), content.size())
8. 拼出用户目录 ./storage/{uid}
9. 拼出真实文件路径 ./storage/{uid}/{hashcode}
10. create_directories() 创建用户目录
11. ofstream 按二进制写入文件内容
12. INSERT tbl_file 写入元数据
13. 返回 fileId 和 filename
```

成功响应：

```http
HTTP/1.1 201 Created
Content-Type: application/json
```

```json
{
  "status": "success",
  "message": "上传成功",
  "data": {
    "fileId": 22,
    "filename": "a.txt"
  }
}
```

错误响应：

| 场景 | HTTP 状态码 | message |
| --- | --- | --- |
| token 无效 | 401 | 无效的访问令牌 |
| 请求体不是 multipart/form-data | 400 | 请求格式有误 |
| 没有 file 字段 | 400 | 请求格式有误 |
| filename 为空 | 400 | 请求格式有误 |
| 创建目录失败 | 500 | 内部服务器错误 |
| 打开本地文件失败 | 500 | 内部服务器错误 |
| SQL 执行失败 | 500 | 内部服务器错误 |

注意：当前代码是先写本地文件，再写 MySQL。如果本地文件写入成功但 MySQL 失败，磁盘上会留下没有数据库记录的文件。教学项目暂不处理补偿清理逻辑；生产项目通常需要事务、清理任务或对象存储回滚。

### 6. 下载文件

```text
GET /api/v1/file/{id}
```

示例：

```text
GET /api/v1/file/22
```

路径参数：

```cpp
int file_id = req->param<int>("id");
```

路由写的是：

```cpp
server_.GET("/api/v1/file/{id}", ...)
```

所以 wfrest 会把 `/api/v1/file/22` 中的 `22` 提取为名为 `id` 的路径参数。`req->param<int>("id")` 表示取出这个路径参数，并转换成 `int`。

权限控制 SQL：

```sql
SELECT filename, hashcode
FROM tbl_file
WHERE id = file_id AND uid = 当前登录用户id
LIMIT 1;
```

这个 SQL 很重要。它同时检查：

```text
文件 id 是否存在
文件是否属于当前登录用户
```

如果不加 `uid = 当前登录用户id`，用户可能通过猜测 fileId 下载其它用户的文件。

处理流程：

```text
1. check_login(req, user)
2. 从路径参数取 file_id
3. SELECT tbl_file WHERE id = file_id AND uid = user.id
4. 如果查不到，返回 404 文件不存在
5. 取 filename 和 hashcode
6. 拼出真实文件路径 ./storage/{uid}/{hashcode}
7. 检查本地文件存在并且是普通文件
8. 设置 Content-Disposition 响应头
9. resp->File(real_path) 返回文件内容
```

下载成功响应不是 JSON：

```http
HTTP/1.1 200 OK
Content-Disposition: attachment; filename="a.txt"
Content-Type: application/octet-stream

文件二进制内容...
```

代码：

```cpp
resp->set_status(HttpStatusOK);
resp->add_header("Content-Disposition",
                 "attachment; filename=\"" + escape_header_filename(filename) + "\"");
resp->File(real_path);
```

`Content-Disposition: attachment` 告诉浏览器这是下载附件。

`filename="a.txt"` 告诉浏览器保存文件时使用的原始文件名。

`resp->File(real_path)` 是 wfrest 提供的文件响应接口，会读取本地文件并写入 HTTP 响应体。

错误响应：

| 场景 | HTTP 状态码 | message |
| --- | --- | --- |
| token 无效 | 401 | 无效的访问令牌 |
| SQL 执行失败 | 500 | 内部服务器错误 |
| 数据库查不到文件记录 | 404 | 文件不存在 |
| 本地磁盘文件不存在 | 404 | 文件不存在 |

---

## 十六、`CryptoUtil` 详解

`CryptoUtil.h` 中的 `User`：

```cpp
struct User {
    int id;
    std::string username;
    std::string password;
    std::string salt;
    std::string createdAt;
};
```

不同场景下 `User` 字段使用不同：

```text
注册
  不需要 User 对象，只生成 salt 和 password_hash。

登录
  从 MySQL 查出 id、username、password、salt、created_at，填入 User。
  然后校验密码并生成 token。

后续请求
  定义空的 User user。
  verify_token(token, user) 只填入 id、username、createdAt。
  password 和 salt 不会从 token 中恢复，因为 token 不应该保存敏感数据。
```

### 1. `generate_salt()`

```cpp
static std::string generate_salt(int length = 8);
```

从数字、小写字母、大写字母中随机取字符，生成 salt。

`main.cc` 中调用：

```cpp
srand(time(NULL));
```

用于初始化 `rand()` 随机数种子。

### 2. `hash_password()`

```cpp
static std::string hash_password(const std::string& password,
                                 const std::string& salt,
                                 const EVP_MD* md = EVP_sha256());
```

默认使用 SHA-256。输入是：

```text
salt + password
```

输出是十六进制字符串。

### 3. `generate_hashcode()`

```cpp
static std::string generate_hashcode(const char* data, size_t n,
                                     const EVP_MD* md = EVP_sha256());
```

根据文件内容计算 SHA-256 哈希。上传时使用：

```cpp
string hashcode = CryptoUtil::generate_hashcode(content.data(), content.size());
```

这个 hashcode 用于本地文件名，不是用户看到的文件名。

### 4. `generate_token()`

```cpp
static std::string generate_token(const User& user,
                                  jwt_alg_t algorithm = JWT_ALG_HS256);
```

写入 JWT payload：

```text
sub        = LoginToken
id         = user.id
username   = user.username
created_at = user.createdAt
exp        = time(NULL) + 3600
```

`exp` 表示过期时间，当前是 1 小时。

### 5. `verify_token()`

```cpp
static bool verify_token(const std::string& token, User& user);
```

校验过程：

```text
1. jwt_decode()
   使用 SECRET_KEY 校验签名。

2. 检查 sub 是否是 LoginToken。

3. 检查 exp 是否已经过期。

4. 从 token 取出 id、username、created_at，写入 user。
```

如果任何一步失败，返回 false。

---

## 十七、前端如何调用后端

前端 API 基础路径：

```js
const API_BASE_URL = '/api/v1';
```

### 1. JSON 请求

`apiRequest()` 会设置：

```js
const headers = {
  'Content-Type': 'application/json',
};
```

如果本地有 token：

```js
headers['Authorization'] = `Bearer ${token}`;
```

注册和登录走 JSON 请求。

### 2. 文件上传请求

上传使用：

```js
const formData = new FormData();
formData.append('file', file);
```

注意前端没有手动设置 `Content-Type`：

```js
// Do NOT set Content-Type header; browser will set it with boundary
```

浏览器会自动生成：

```http
Content-Type: multipart/form-data; boundary=...
```

如果手动设置 `multipart/form-data` 却没有 boundary，后端可能无法正确解析。

### 3. 文件下载请求

下载使用：

```js
fetch(`${API_BASE_URL}/file/${fileId}`, {
  method: 'GET',
  headers,
});
```

成功后前端读取：

```js
const contentDisposition = response.headers.get('Content-Disposition');
const blob = await response.blob();
```

然后触发浏览器下载。

---

## 十八、编译和运行

### 1. 编译

进入项目目录：

```bash
cd /home/lws/my_project/Cpp_Advanced/课件/05_Web网盘项目/CloudDisk_V1
```

方式一：使用脚本：

```bash
./build.sh
```

方式二：手动执行：

```bash
cmake -S . -B build
cmake --build build -j2
```

编译成功后会生成：

```text
CloudDisk_V1/server
```

### 2. 运行

```bash
./server
```

默认监听端口：

```text
8888
```

浏览器访问：

```text
http://127.0.0.1:8888
```

登录页：

```text
http://127.0.0.1:8888/static/login.html
```

注册页：

```text
http://127.0.0.1:8888/static/register.html
```

### 3. MySQL 连接配置

代码中数据库 URL：

```cpp
static const string DatabaseURL = "mysql://root:123456@localhost/CloudDisk";
```

含义：

```text
用户名：root
密码：123456
主机：localhost
数据库：CloudDisk
```

如果你的 MySQL 用户、密码或数据库名不同，需要修改这行。

### 4. 依赖库

`CMakeLists.txt` 链接：

```cmake
target_link_libraries(server PRIVATE 
    crypto
    ssl
    jwt
    wfrest
)
```

依赖含义：

```text
crypto / ssl
  OpenSSL，提供哈希能力。

jwt
  libjwt，生成和解析 JWT。

wfrest
  HTTP 服务器、路由、MySQL 异步任务、文件响应等。
```

---

## 十九、curl 测试示例

### 1. 注册

```bash
curl -i -X POST http://127.0.0.1:8888/api/v1/auth/register \
  -H 'Content-Type: application/json' \
  -d '{"username":"alice","password":"1234","confirm":"1234"}'
```

### 2. 登录

```bash
curl -i -X POST http://127.0.0.1:8888/api/v1/auth/login \
  -H 'Content-Type: application/json' \
  -d '{"username":"alice","password":"1234"}'
```

从响应中复制 `data.accessToken`。

### 3. 获取当前用户

```bash
curl -i http://127.0.0.1:8888/api/v1/user/me \
  -H 'Authorization: Bearer 你的token'
```

### 4. 文件列表

```bash
curl -i http://127.0.0.1:8888/api/v1/files \
  -H 'Authorization: Bearer 你的token'
```

### 5. 上传文件

```bash
curl -i -X POST http://127.0.0.1:8888/api/v1/files \
  -H 'Authorization: Bearer 你的token' \
  -F 'file=@CMakeLists.txt'
```

注意：使用 `-F` 时 curl 会自动生成 multipart/form-data 请求。

### 6. 下载文件

假设上传后返回：

```json
{
  "fileId": 1
}
```

下载：

```bash
curl -i http://127.0.0.1:8888/api/v1/file/1 \
  -H 'Authorization: Bearer 你的token' \
  -o downloaded_file
```

---

## 二十、常见问题排查

### 1. 服务启动失败

现象：

```text
Error: Server start FAILED!
```

常见原因：

```text
8888 端口已经被其它进程占用。
当前环境不允许监听端口。
```

可以检查：

```bash
pgrep -af server
```

### 2. 注册或登录返回 500

常见原因：

```text
MySQL 没启动。
数据库 CloudDisk 不存在。
tbl_user 表不存在。
数据库用户名或密码不对。
DatabaseURL 配置不匹配。
```

检查：

```bash
mysql -h127.0.0.1 -uroot -p123456 CloudDisk
```

### 3. 注册返回“用户名已存在”

原因：

```text
tbl_user.username 有 UNIQUE 约束。
相同用户名不能重复注册。
```

### 4. 登录返回“用户名或密码错误”

可能原因：

```text
用户名不存在。
用户 tomb 不等于 0。
密码错误。
数据库中的 salt/password 与当前算法不匹配。
```

接口不会告诉前端具体是哪一种原因，这是登录接口常见做法。

### 5. 获取文件列表返回“无效的访问令牌”

常见原因：

```text
没有 Authorization 请求头。
Authorization 不是 Bearer 类型。
token 复制错了。
token 过期了。
SECRET_KEY 改过，旧 token 无法校验。
```

请求头必须是：

```http
Authorization: Bearer xxxxx
```

### 6. 上传返回“请求格式有误”

常见原因：

```text
没有使用 multipart/form-data。
multipart 中没有 file 字段。
file 字段没有 filename。
手动设置 Content-Type 导致缺少 boundary。
```

推荐用：

```bash
curl -F 'file=@a.txt' ...
```

或者前端使用：

```js
const formData = new FormData();
formData.append('file', file);
```

### 7. 上传返回 500

可能原因：

```text
storage 目录无法创建。
当前进程没有写入权限。
磁盘空间不足。
tbl_file 表不存在。
同一用户上传同名文件，触发 UNIQUE KEY (uid, filename)。
MySQL 连接失败。
```

### 8. 下载返回“文件不存在”

可能原因：

```text
fileId 不存在。
fileId 存在但不属于当前登录用户。
数据库记录存在，但 ./storage/{uid}/{hashcode} 文件被删除。
```

可以根据数据库记录检查本地路径：

```text
./storage/{uid}/{hashcode}
```

---

## 二十一、第一期的边界和后续演进

第一期为了教学清晰，做了很多简化：

```text
文件保存在本地磁盘，不适合多机部署。
上传时先写本地文件，再写 MySQL，没有事务补偿。
SQL 使用字符串拼接，只做了简单 escape_sql。
同名文件直接依赖 UNIQUE KEY 拒绝。
JWT SECRET_KEY 写在代码中。
没有文件删除接口。
没有分页，文件列表一次返回当前用户所有文件。
没有对超大文件做流式处理，multipart 内容会进入内存。
```

这些简化是为了让第一期重点集中在：

```text
HTTP API 设计
请求参数校验
统一响应格式
MySQL 异步回调
JWT 登录态
文件上传下载流程
权限控制
```

第二期会把真实文件内容迁移到 OSS，对应变化是：

```text
第一期：
  ./storage/{uid}/{hashcode}

第二期：
  OSS ObjectName = users/{uid}/{hashcode}
```

MySQL 表结构和大部分 API 格式保持不变。

---

## 二十二、学习建议

阅读代码时建议按下面顺序：

```text
1. main.cc
   看服务如何启动。

2. CloudDiskServer.h
   看服务器类有哪些模块。

3. CloudDiskServer.cc 顶部工具函数
   理解统一响应、JSON 解析、token 校验、本地路径生成。

4. register_auth_module()
   先看注册，再看登录。

5. CryptoUtil.cc
   理解 salt、密码 hash、JWT。

6. register_user_module()
   理解 token 如何还原当前用户。

7. register_file_module()
   理解文件列表、上传、下载。

8. www/static/api.js
   对照前端如何调用这些接口。
```

如果只记一条主线，就是：

```text
前端请求
  -> wfrest 路由
  -> 校验请求格式
  -> 校验 token
  -> 执行 MySQL 或文件操作
  -> 按固定 JSON/文件格式响应
```

这就是第一期后端的核心。
