# CloudDisk 第二期技术文档：接入阿里云 OSS 对象存储

本文档说明 `CloudDisk_V2` 网盘项目第二期的后端实现。

第二期的核心目标是：把第一期“用户文件保存到服务器本地磁盘”的方案，替换为“用户文件保存到阿里云对象存储 OSS”。MySQL 仍然保存用户信息和文件元数据，OSS 保存真实文件内容。

读完本文档后，你应该能掌握：

1. 第二期为什么要接入 OSS。
2. 当前后端由哪些模块组成，每个模块负责什么。
3. `.env`、`run.sh`、`getEnvOrThrow()` 如何共同完成 OSS 配置加载。
4. OSS SDK 的 `InitializeSdk()` / `ShutdownSdk()` 为什么放在 `CloudDiskServer` 构造和析构函数中。
5. 每次上传/下载为什么临时创建一个 `OssClient`。
6. 文件上传接口如何从 multipart 中取文件、生成 hash、上传 OSS、写入 MySQL。
7. 文件下载接口如何校验权限、查询 MySQL、从 OSS 拉取内容并返回给浏览器。
8. MySQL 和 OSS 分别保存什么数据。
9. `stringstream`、`seekg(0)`、`rdbuf()`、`resp->String(move(content))` 这些关键代码在做什么。
10. 编译运行和常见错误应该如何排查。

本文档主要讲后端。前端页面仍然负责登录、注册、文件列表、上传、下载等浏览器交互。

---

## 一、第二期解决什么问题

第一期中，上传文件会保存到服务器本地磁盘，例如：

```text
./storage/{uid}/{hashcode}
```

这种方式适合教学入门，但有明显限制：

- 文件和后端服务部署在同一台机器上，服务器磁盘压力会越来越大。
- 服务迁移、扩容、备份都不方便。
- 如果后端部署多台机器，不同机器上的本地文件不共享。
- 本地磁盘损坏会直接影响用户文件。

第二期接入阿里云 OSS 后，真实文件内容保存到 OSS：

```text
Bucket: ubuntu-cloud-disk-oss
ObjectName: users/{uid}/{hashcode}
```

后端服务器只负责：

- 接收 HTTP 请求。
- 校验用户登录态。
- 读写 MySQL 元数据。
- 调用 OSS SDK 上传或下载文件内容。

文件内容不再依赖后端服务器本地磁盘。

---

## 二、整体架构

当前后端核心架构如下：

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
  | PutObject / GetObject
  v
阿里云 OSS
```

更具体一点：

```text
注册 / 登录 / 获取个人信息
  -> CloudDiskServer
  -> MySQL

上传文件
  -> CloudDiskServer
  -> 解析 multipart/form-data
  -> 计算文件 hashcode
  -> OSS PutObject 保存文件内容
  -> MySQL INSERT 保存文件元数据

下载文件
  -> CloudDiskServer
  -> MySQL SELECT 确认文件属于当前用户
  -> OSS GetObject 读取文件内容
  -> HTTP 响应体返回给浏览器
```

第二期是同步 OSS 版本：上传接口会等待 OSS `PutObject` 成功后，再写 MySQL 并返回结果。第三期才会引入 RabbitMQ，把 OSS 上传拆到后台异步执行。

---

## 三、目录结构

`CloudDisk_V2` 主要文件如下：

```text
CloudDisk_V2/
├── .env
├── CMakeLists.txt
├── CloudDiskServer.cc
├── CloudDiskServer.h
├── CryptoUtil.cc
├── CryptoUtil.h
├── build.sh
├── run.sh
├── main.cc
├── server
└── www/
    ├── index.html
    └── static/
```

后端重点文件：

```text
CloudDiskServer.cc
  HTTP 路由、MySQL 查询、OSS 上传下载、统一 JSON 响应。

CloudDiskServer.h
  CloudDiskServer 类声明，组合 wfrest::HttpServer。

CryptoUtil.cc / CryptoUtil.h
  密码加盐哈希、文件 hashcode、JWT 生成和校验。

main.cc
  创建 CloudDiskServer，注册路由，启动 8888 端口。

CMakeLists.txt
  编译 server，并链接 wfrest、jwt、openssl、OSS SDK 等依赖。

build.sh
  编译项目，并生成 run.sh。

run.sh
  加载 .env 中的 OSS 配置，然后启动 ./server。
```

---

## 四、核心类：`CloudDiskServer`

`CloudDiskServer.h` 中的类声明：

```cpp
class CloudDiskServer {
public:
    CloudDiskServer();
    ~CloudDiskServer();

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

`main.cc` 中创建对象：

```cpp
CloudDiskServer server;
server.register_routes();

if (server.start(8888) == 0) {
    waitGroup.wait();
    server.stop();
}
```

生命周期大致如下：

```text
程序启动
  -> 创建 CloudDiskServer server
  -> CloudDiskServer 构造函数执行
  -> InitializeSdk() 初始化 OSS SDK
  -> register_routes() 注册路由
  -> start(8888) 监听 HTTP 请求
  -> 用户访问、上传、下载
  -> Ctrl+C 触发 waitGroup.done()
  -> server.stop()
  -> main() 结束
  -> CloudDiskServer 析构函数执行
  -> ShutdownSdk() 释放 OSS SDK
程序退出
```

注意：`CloudDiskServer server` 是后端服务器进程里的 C++ 对象，不是浏览器客户端。正常情况下，一个服务进程只创建一个 `CloudDiskServer`。

---

## 五、OSS SDK 生命周期

OSS C++ SDK 要求在使用任何 OSS API 前调用：

```cpp
oss::InitializeSdk();
```

在程序结束、后续不再使用 OSS SDK 时调用：

```cpp
oss::ShutdownSdk();
```

当前代码放在 `CloudDiskServer` 构造和析构函数中：

```cpp
CloudDiskServer::CloudDiskServer()
{
    oss::InitializeSdk();
}

CloudDiskServer::~CloudDiskServer()
{
    oss::ShutdownSdk();
}
```

这样做的原因：

- `CloudDiskServer` 在 `main.cc` 中只创建一次。
- SDK 初始化和释放也就各执行一次。
- 不会在每个上传/下载请求里反复初始化和释放 SDK。
- 避免并发请求中某个请求释放 SDK，影响其它请求。

这是一种 RAII 思想：

```text
对象构造 -> 申请或初始化资源
对象析构 -> 释放资源
```

错误写法示例：

```cpp
InitializeSdk();
PutObject(...);
ShutdownSdk();
```

如果每个请求都这么做，在并发场景下很容易出问题。

---

## 六、OSS 配置和 `.env`

第二期需要的 OSS 配置包括：

```text
ALIBABA_CLOUD_ACCESS_KEY_ID
ALIBABA_CLOUD_ACCESS_KEY_SECRET
ALIBABA_CLOUD_OSS_BUCKET
ALIBABA_CLOUD_OSS_ENDPOINT
ALIBABA_CLOUD_OSS_REGION
```

这些配置放在项目根目录的 `.env` 文件中：

```text
CloudDisk_V2/.env
```

格式如下：

```text
ALIBABA_CLOUD_ACCESS_KEY_ID=你的 AccessKey ID
ALIBABA_CLOUD_ACCESS_KEY_SECRET=你的 AccessKey Secret
ALIBABA_CLOUD_OSS_BUCKET=ubuntu-cloud-disk-oss
ALIBABA_CLOUD_OSS_ENDPOINT=oss-cn-wuhan-lr.aliyuncs.com
ALIBABA_CLOUD_OSS_REGION=cn-wuhan
```

`CloudDiskServer.cc` 中读取配置：

```cpp
static string getEnvOrThrow(const char* name) {
    const char* value = getenv(name);
    if (value == nullptr || string(value).empty()) {
        throw runtime_error(string("Missing environment variable: ") + name);
    }
    return string(value);
}

static const string OssEndpoint = getEnvOrThrow("ALIBABA_CLOUD_OSS_ENDPOINT");
static const string OssAccessKeyId = getEnvOrThrow("ALIBABA_CLOUD_ACCESS_KEY_ID");
static const string OssAccessKeySecret = getEnvOrThrow("ALIBABA_CLOUD_ACCESS_KEY_SECRET");
static const string OssBucketName = getEnvOrThrow("ALIBABA_CLOUD_OSS_BUCKET");
static const string OssRegion = getEnvOrThrow("ALIBABA_CLOUD_OSS_REGION");
```

这里有一个关键点：`getenv()` 不会自动读取 `.env` 文件。它读取的是当前进程已经拥有的环境变量。

本项目通过 `run.sh` 完成 `.env` 加载：

```bash
set -a
source .env
set +a

./server
```

含义：

- `set -a`：之后定义的 shell 变量自动 export。
- `source .env`：读取 `.env` 文件内容。
- `set +a`：关闭自动 export。
- `./server`：启动后端服务，此时进程可以通过 `getenv()` 读到这些变量。

因此推荐启动方式是：

```bash
./run.sh
```

不要直接执行：

```bash
./server
```

如果直接执行 `./server`，当前 shell 又没有提前加载 `.env`，程序会在静态初始化阶段抛异常：

```text
Missing environment variable: ALIBABA_CLOUD_ACCESS_KEY_ID
```

AccessKey 是敏感信息，实际项目中不应提交到公开仓库。更合理的做法是提交 `.env.example` 模板，把 `.env` 加入 `.gitignore`。

---

## 七、OSS 客户端对象 `OssClient`

创建 OSS 客户端的函数：

```cpp
static unique_ptr<oss::OssClient> create_oss_client()
{
    oss::ClientConfiguration conf;
    auto client = make_unique<oss::OssClient>(
        OssEndpoint,
        OssAccessKeyId,
        OssAccessKeySecret,
        conf);
    client->SetRegion(OssRegion);
    return client;
}
```

几个概念要分清：

```text
CloudDiskServer
  后端 Web 服务器对象，main.cc 中只创建一次。

OssClient
  OSS SDK 提供的 C++ 客户端对象，用来发 PutObject / GetObject 请求。

浏览器客户端
  用户打开网页后运行的前端代码，不是 C++ OssClient。
```

当前代码每次上传或下载都会调用：

```cpp
auto client = create_oss_client();
```

也就是每次 OSS 操作创建一个临时 `OssClient`。请求结束后，`unique_ptr` 离开作用域，自动销毁这个客户端对象。

为什么不做全局静态 `OssClient`？

```cpp
static oss::OssClient client(...);
```

全局静态对象存在析构顺序问题。`OssClient` 应该在 `InitializeSdk()` 之后使用，并且最好在 `ShutdownSdk()` 之前析构。如果做成全局静态对象，程序退出时可能出现：

```text
ShutdownSdk() 已经释放 SDK 全局资源
全局静态 OssClient 才开始析构
```

这类问题比较隐蔽。当前按请求创建临时 `OssClient`，代码更直观，也足够满足教学项目需求。

为什么返回 `unique_ptr<oss::OssClient>`？

- 表示这个客户端对象由当前调用者独占。
- 函数结束或请求结束后自动释放。
- 不需要手写 `new/delete`。
- 避免直接值返回 SDK 对象带来的拷贝/移动语义疑问。

`make_unique<oss::OssClient>(...)` 可以理解为安全版：

```cpp
new oss::OssClient(endpoint, accessKeyId, accessKeySecret, conf)
```

但它返回 `unique_ptr`，对象会自动释放。

---

## 八、OSS ObjectName 设计

当前代码生成 OSS 对象名：

```cpp
static string oss_object_name(int uid, const string& hashcode)
{
    return "users/" + to_string(uid) + "/" + hashcode;
}
```

示例：

```text
uid = 3
hashcode = a948904f2f0f479...

ObjectName = users/3/a948904f2f0f479...
```

为什么不直接用用户上传的原始文件名？

- 原始文件名可能包含空格、中文、特殊字符。
- 同名文件容易覆盖。
- 后端更适合用稳定的 hashcode 定位内容。
- 原始文件名仍然保存在 MySQL，下载时可以恢复给浏览器。

OSS 中没有真正的目录。`users/3/xxx` 只是一个对象名字符串。OSS 控制台把 `/` 展示成类似目录，是为了方便查看。

---

## 九、MySQL 和 OSS 分工

MySQL 保存元数据。

用户表大致字段：

```text
tbl_user
  id
  username
  password
  salt
  created_at
  tomb
```

文件表大致字段：

```text
tbl_file
  id
  uid
  filename
  hashcode
  size
  created_at
  last_update
```

OSS 保存真实文件内容：

```text
users/{uid}/{hashcode} -> 文件二进制内容
```

例如用户上传：

```text
filename = 毕业设计.docx
content  = 文件二进制内容
uid      = 3
hashcode = a948904f2f0f479...
```

MySQL 中保存：

```text
uid      = 3
filename = 毕业设计.docx
hashcode = a948904f2f0f479...
size     = 文件大小
```

OSS 中保存：

```text
users/3/a948904f2f0f479... -> 文件二进制内容
```

下载时：

1. 先通过 MySQL 判断当前用户是否有权限下载这个 fileId。
2. 再通过 `uid + hashcode` 去 OSS 读取真实内容。
3. 最后用 MySQL 中的 `filename` 设置下载文件名。

---

## 十、统一响应格式

后端对 API 接口统一返回 JSON。

成功响应：

```json
{
  "status": "success",
  "message": "登录成功",
  "data": {}
}
```

失败响应：

```json
{
  "status": "error",
  "message": "无效的访问令牌"
}
```

对应工具函数：

```cpp
static void response_json(HttpResp* resp, int status_code, const json& body)
static void response_success(HttpResp* resp, int status_code, const string& message, const json& data)
static void response_error(HttpResp* resp, int status_code, const string& message)
```

这样前端只需要按统一结构读取：

```text
status
message
data
```

下载文件接口是例外：成功时返回的是二进制文件内容，不是 JSON；失败时仍然返回 JSON 错误。

---

## 十一、认证和 JWT

登录成功后，后端使用 `CryptoUtil::generate_token(user)` 生成 JWT。

JWT 中保存：

```text
sub        = LoginToken
id         = 用户 id
username   = 用户名
created_at = 注册时间
exp        = 过期时间，当前为 1 小时
```

前端后续请求带上：

```http
Authorization: Bearer xxxxxx
```

后端通过：

```cpp
static bool get_bearer_token(const HttpReq* req, string& token)
static bool check_login(const HttpReq* req, User& user)
```

完成登录态校验。

`check_login()` 做三件事：

1. 从请求头取出 Bearer Token。
2. 调用 `CryptoUtil::verify_token(token, user)` 校验签名、主题和过期时间。
3. 校验成功后，把 JWT 里的用户信息填入 `User user`。

因此文件列表、上传、下载都不相信前端传来的 uid，而是从 JWT 还原当前用户 id。

---

## 十二、密码和文件 hash

`CryptoUtil` 负责密码、文件 hash、JWT。

### 1. 密码加盐哈希

注册时：

```cpp
string salt = CryptoUtil::generate_salt();
string password_hash = CryptoUtil::hash_password(password, salt);
```

数据库保存：

```text
password = hash_password(password, salt)
salt     = 随机 salt
```

登录时：

```cpp
string input_hash = CryptoUtil::hash_password(password, user.salt);
```

然后比较：

```cpp
input_hash == user.password
```

后端不保存明文密码。

### 2. 文件 hashcode

上传时：

```cpp
string hashcode = CryptoUtil::generate_hashcode(content.data(), content.size());
```

这个 hashcode 用于 OSS ObjectName，而不是用户展示名。

---

## 十三、路由总览

### 1. 静态资源

```text
GET /
GET /static/*
```

注册位置：

```cpp
server_.Static("/", "./www/index.html");
server_.Static("/static", "./www/static");
```

### 2. 注册

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

流程：

```text
1. 检查 Content-Type 是否是 application/json
2. 解析 username/password/confirm
3. 检查用户名密码非空
4. 检查两次密码一致
5. 生成 salt 和 password_hash
6. INSERT tbl_user
7. 返回 userId 和 username
```

成功响应：

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

### 3. 登录

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

流程：

```text
1. 检查 JSON 请求体
2. 按 username 查询 tbl_user
3. 读取 salt 和 password hash
4. 对输入密码重新计算 hash
5. 比较 hash
6. 生成 JWT
7. 返回 accessToken
```

成功响应：

```json
{
  "status": "success",
  "message": "登录成功",
  "data": {
    "accessToken": "xxxxx",
    "tokenType": "Bearer",
    "user": {
      "userId": 1,
      "username": "alice"
    }
  }
}
```

### 4. 当前用户信息

```text
GET /api/v1/user/me
```

请求头：

```http
Authorization: Bearer xxxxx
```

流程：

```text
1. 校验 token
2. 从 JWT 还原 user.id/user.username/user.createdAt
3. 返回用户信息
```

### 5. 文件列表

```text
GET /api/v1/files
```

请求头：

```http
Authorization: Bearer xxxxx
```

SQL：

```sql
SELECT id, filename, size, created_at, last_update
FROM tbl_file
WHERE uid = 当前登录用户id
ORDER BY last_update DESC, id DESC;
```

返回字段：

```json
{
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
```

### 6. 上传文件

```text
POST /api/v1/files
```

请求头：

```http
Authorization: Bearer xxxxx
Content-Type: multipart/form-data; boundary=...
```

表单字段：

```text
file = 用户选择的文件
```

完整流程：

```text
1. 校验 token，得到当前登录用户 uid
2. 检查请求类型必须是 multipart/form-data
3. 从 req->form() 取出 form["file"]
4. form["file"].first 是原始文件名 filename
5. form["file"].second 是文件内容 content
6. 检查 filename 非空
7. 对 content 计算 hashcode
8. 调用 oss_upload_object(uid, hashcode, content)
9. OSS 保存对象 users/{uid}/{hashcode}
10. INSERT tbl_file 保存 uid/filename/hashcode/size
11. 返回上传成功
```

关键代码：

```cpp
Form& form = req->form();
string filename = form["file"].first;
string content = form["file"].second;
string hashcode = CryptoUtil::generate_hashcode(content.data(), content.size());

if (!oss_upload_object(user.id, hashcode, content)) {
    response_error(resp, HttpStatusInternalServerError, "内部服务器错误");
    return;
}
```

注意：当前第二期是先上传 OSS，再写 MySQL。如果 OSS 成功但 MySQL 失败，OSS 中会留下没有数据库记录的对象，也就是孤儿对象。教学项目暂不处理这个补偿逻辑；生产项目通常会在 MySQL 写入失败时调用 OSS `DeleteObject` 清理。

### 7. 下载文件

```text
GET /api/v1/file/{id}
```

请求头：

```http
Authorization: Bearer xxxxx
```

完整流程：

```text
1. 校验 token，得到当前登录用户 uid
2. 从路径参数中取 file_id
3. 查询 tbl_file，条件必须同时包含 file_id 和 uid
4. 如果查不到，返回 404 文件不存在
5. 取出 filename 和 hashcode
6. 调用 oss_download_object(uid, hashcode, content)
7. 如果 OSS 返回 NoSuchKey，返回 404 文件不存在
8. 设置下载响应头
9. 把 content 写入 HTTP 响应体
```

权限控制 SQL：

```sql
SELECT filename, hashcode
FROM tbl_file
WHERE id = file_id AND uid = 当前登录用户id
LIMIT 1;
```

这个 SQL 很关键。它保证用户只能下载自己的文件，不能通过猜测其它用户的 fileId 下载文件。

下载成功时响应不是 JSON，而是文件内容：

```cpp
resp->set_status(HttpStatusOK);
resp->add_header("Content-Type", "application/octet-stream");
resp->add_header("Content-Disposition",
                 "attachment; filename=\"" + escape_header_filename(filename) + "\"");
resp->String(move(content));
```

---

## 十四、上传到 OSS 的实现细节

上传函数：

```cpp
static bool oss_upload_object(int uid, const string& hashcode, const string& content)
{
    auto client = create_oss_client();
    string bucket_name = OssBucketName;
    string object_name = oss_object_name(uid, hashcode);

    auto stream = make_shared<stringstream>(ios::in | ios::out | ios::binary);
    stream->write(content.data(), content.size());
    stream->seekg(0);

    oss::PutObjectRequest request(bucket_name, object_name, stream);
    auto outcome = client->PutObject(request);
    if (!outcome.isSuccess()) {
        log_oss_error("PutObject", outcome.error());
        return false;
    }
    return true;
}
```

### 1. 为什么要用 `stringstream`

`wfrest` 已经把 multipart 中的文件内容解析成：

```cpp
string content;
```

但 OSS SDK 的 `PutObjectRequest` 需要：

```cpp
shared_ptr<iostream>
```

所以需要把内存中的 `string` 包装成一个“像文件一样可以读取的流”：

```cpp
auto stream = make_shared<stringstream>(ios::in | ios::out | ios::binary);
```

含义：

- `ios::out`：允许把 content 写入流。
- `ios::in`：允许 OSS SDK 后续从流中读取。
- `ios::binary`：按二进制处理，不做文本转换。

### 2. 为什么要 `seekg(0)`

写入流：

```cpp
stream->write(content.data(), content.size());
```

写完后，流的位置在末尾。OSS SDK 需要从开头读取完整内容，因此：

```cpp
stream->seekg(0);
```

`seekg` 的 `g` 是 get pointer，也就是读取位置。`seekg(0)` 表示把读取位置移动到开头。

如果没有这一步，SDK 可能从错误位置开始读，导致上传内容为空或不完整。

### 3. 错误处理

OSS SDK 返回 `outcome`：

```cpp
auto outcome = client->PutObject(request);
```

判断是否成功：

```cpp
if (!outcome.isSuccess()) {
    log_oss_error("PutObject", outcome.error());
    return false;
}
```

HTTP 接口不会把 AccessKey、Bucket、RequestId 等内部细节返回给浏览器，只返回统一错误：

```json
{
  "status": "error",
  "message": "内部服务器错误"
}
```

详细错误打印到服务端日志，便于排查。

---

## 十五、从 OSS 下载的实现细节

下载函数：

```cpp
static OssDownloadStatus oss_download_object(int uid,
                                             const string& hashcode,
                                             string& content)
{
    auto client = create_oss_client();
    string bucket_name = OssBucketName;
    string object_name = oss_object_name(uid, hashcode);

    auto outcome = client->GetObject(bucket_name, object_name);
    if (!outcome.isSuccess()) {
        if (outcome.error().Code() == "NoSuchKey") {
            return OssDownloadStatus::NotFound;
        }
        log_oss_error("GetObject", outcome.error());
        return OssDownloadStatus::Failed;
    }

    auto stream = outcome.result().Content();
    if (!stream) {
        cerr << "[OSS GetObject FAILED] empty content stream" << endl;
        return OssDownloadStatus::Failed;
    }

    ostringstream oss_content;
    oss_content << stream->rdbuf();
    content = oss_content.str();
    return OssDownloadStatus::Ok;
}
```

### 1. 为什么返回枚举

```cpp
enum class OssDownloadStatus {
    Ok,
    NotFound,
    Failed
};
```

下载失败分两类：

```text
NotFound
  OSS 对象不存在，例如 NoSuchKey。
  HTTP 层应该返回 404 文件不存在。

Failed
  OSS 权限、网络、Bucket、Endpoint 等服务端问题。
  HTTP 层应该返回 500 内部服务器错误。
```

如果只返回 `bool`，就无法区分 404 和 500。

### 2. `rdbuf()` 在做什么

OSS SDK 返回的是输入流：

```cpp
auto stream = outcome.result().Content();
```

这行代码：

```cpp
oss_content << stream->rdbuf();
```

表示把 `stream` 中剩余的所有字节复制到 `oss_content` 中。

随后：

```cpp
content = oss_content.str();
```

得到完整文件内容字符串，用作 HTTP 响应体。

### 3. `content` 为什么是输出参数

函数签名：

```cpp
static OssDownloadStatus oss_download_object(int uid,
                                             const string& hashcode,
                                             string& content)
```

`content` 是引用参数。函数成功时，把 OSS 下载到的文件内容写入它；函数失败时，通过返回的 `OssDownloadStatus` 告诉调用者失败原因。

调用处：

```cpp
string content;
OssDownloadStatus status = oss_download_object(uid, hashcode, content);
```

如果 `status == Ok`，此时 `content` 中就是文件内容。

---

## 十六、下载响应头

下载成功时后端设置：

```cpp
resp->add_header("Content-Type", "application/octet-stream");
resp->add_header("Content-Disposition",
                 "attachment; filename=\"" + escape_header_filename(filename) + "\"");
```

这两个响应头是返回给浏览器的，不是 OSS 要求的。

`Content-Type: application/octet-stream` 表示响应体是通用二进制文件。

`Content-Disposition: attachment; filename="..."` 表示浏览器应该下载响应体，并使用指定文件名。

文件名来自 MySQL 中保存的原始 `filename`。

为什么要转义文件名？

```cpp
static string escape_header_filename(const string& filename)
```

HTTP 响应头中 `filename` 放在双引号里。如果文件名中包含 `"` 或 `\`，会破坏响应头格式。因此需要做简单转义。

---

## 十七、`resp->String(move(content))`

下载时文件内容已经从 OSS 读入内存：

```cpp
string content;
```

返回给浏览器：

```cpp
resp->String(move(content));
```

`resp->String(...)` 是 wfrest 的接口，用来设置 HTTP 响应体。

`move(content)` 表示把 `content` 内部管理的字符串内存尽量转交给响应对象，减少一次大字符串复制。

执行 `move(content)` 后，本作用域不要再使用 `content`。当前代码在这行之后没有再访问 `content`，所以是正确的。

第一期本地文件下载可以用：

```cpp
resp->File(real_path);
```

第二期文件在 OSS 上，服务器本地没有这个真实路径，所以先 `GetObject` 读到内存，再 `resp->String(...)` 返回。

---

## 十八、MySQL 异步回调和捕获

项目使用 `resp->MySQL(DatabaseURL, sql, callback)` 发起 MySQL 查询。

MySQL 查询是异步的。也就是说，注册路由的 lambda 执行完后，SQL 可能还没有执行完成；等结果回来后，才执行回调。

因此回调中使用外层局部变量时，要注意捕获方式。

例如注册接口：

```cpp
resp->MySQL(DatabaseURL, sql, [resp, username](MySQLResultCursor* cursor) {
    ...
});
```

`username` 按值捕获，因为外层函数返回后，局部变量 `username` 会销毁。如果按引用捕获，回调执行时引用可能已经悬空。

登录接口中：

```cpp
resp->MySQL(DatabaseURL, sql, [resp, password](MySQLResultCursor* cursor) {
    ...
});
```

`password` 也必须按值捕获，因为回调中要用它重新计算密码 hash。

下载接口中：

```cpp
resp->MySQL(DatabaseURL, sql, [resp, uid = user.id](MySQLResultCursor* cursor) {
    ...
});
```

这里把 `user.id` 保存成回调中的 `uid`，避免回调执行时访问已经销毁的 `user` 局部变量。

---

## 十九、SQL 字符串转义

当前项目用字符串拼接 SQL，因此提供了：

```cpp
static string escape_sql(const string& s)
```

它处理：

```text
'  -> \'
\  -> \\
```

目的是避免用户名、文件名中包含单引号或反斜线时破坏 SQL 字符串。

例如：

```text
abc'def
```

如果不转义，拼到 SQL 里会破坏引号结构。

注意：这只是教学项目中的简化处理。生产项目应该使用参数化查询或预处理语句，而不是手工拼接 SQL。

---

## 二十、编译和运行

### 1. 编译

推荐使用脚本：

```bash
cd 课件/05_Web网盘项目/CloudDisk_V2
./build.sh
```

干净编译：

```bash
./build.sh clean
```

手动编译也可以：

```bash
cd 课件/05_Web网盘项目/CloudDisk_V2
mkdir -p build
cd build
cmake ..
make
```

### 2. 运行

推荐使用：

```bash
cd 课件/05_Web网盘项目/CloudDisk_V2
./run.sh
```

服务默认监听：

```text
http://localhost:8888
```

`run.sh` 会检查 `.env` 是否存在，并把 `.env` 中的变量导出后再启动 `./server`。

### 3. CMake 依赖

`CMakeLists.txt` 中 OSS 相关配置：

```cmake
target_compile_options(server PRIVATE
    -g
    -fno-rtti
)

target_link_libraries(server PRIVATE
    alibabacloud-oss-cpp-sdk
    curl
    crypto
    ssl
    jwt
    wfrest
    pthread
)
```

`-fno-rtti` 是 OSS C++ SDK 文档要求的编译选项。

`alibabacloud-oss-cpp-sdk` 是 OSS C++ SDK 库。

`curl`、`crypto`、`pthread` 是 OSS SDK 依赖。

`ssl`、`jwt`、`wfrest` 是当前网盘项目依赖。

---

## 二十一、常见问题和排查

### 1. 启动时报 `Missing environment variable`

原因：没有通过 `run.sh` 启动，或者 `.env` 中缺少变量。

检查：

```bash
cd 课件/05_Web网盘项目/CloudDisk_V2
ls -la .env
cat .env
./run.sh
```

不要直接执行：

```bash
./server
```

除非你已经手动 `source .env`。

### 2. OSS 上传失败

服务端日志会出现：

```text
[OSS PutObject FAILED] code:..., message:..., requestId:...
```

重点检查：

- AccessKey ID 是否正确。
- AccessKey Secret 是否正确。
- Bucket 名称是否正确。
- Endpoint 是否和 Bucket 地域一致。
- Region 是否正确。
- 当前账号是否有 PutObject 权限。
- 服务器是否能访问 OSS endpoint。

### 3. OSS 下载失败或文件不存在

如果 OSS 返回 `NoSuchKey`，接口会返回：

```json
{
  "status": "error",
  "message": "文件不存在"
}
```

可能原因：

- MySQL 中有文件元数据，但 OSS 对象不存在。
- 曾经上传 OSS 成功后手动删除了对象。
- ObjectName 生成规则改过，导致新旧对象路径不一致。
- uid 或 hashcode 不匹配。

当前 ObjectName 规则固定是：

```text
users/{uid}/{hashcode}
```

### 4. 上传成功但列表没有文件

上传流程是：

```text
OSS PutObject 成功
MySQL INSERT 成功
返回上传成功
```

如果 OSS 成功但 MySQL 失败，接口会返回 500，列表不会出现文件。服务端日志中应检查 `[upload SQL]` 和 MySQL 状态。

### 5. 文件列表能看到，但下载失败

说明 MySQL 中有元数据，但 OSS 读取失败。

排查：

- OSS 对象是否存在：`users/{uid}/{hashcode}`。
- Bucket、Endpoint、Region 是否正确。
- AccessKey 是否有 GetObject 权限。
- 是否手动清理过 Bucket 中对象。

### 6. 用户能否下载别人的文件

正常不能。

下载 SQL 同时检查：

```sql
WHERE id = file_id AND uid = 当前登录用户id
```

`uid` 来自 JWT，不来自前端参数。用户即使猜到别人的 fileId，也查不到记录。

### 7. 为什么不是只按 hashcode 存 OSS

如果 ObjectName 只用：

```text
{hashcode}
```

不同用户上传相同内容会指向同一个对象。这样可以节省空间，但删除、权限、引用计数会复杂很多。

当前教学项目选择：

```text
users/{uid}/{hashcode}
```

优点是用户隔离清楚，逻辑简单。

### 8. 当前上传方式适合大文件吗

当前实现会把上传文件内容放入内存：

```cpp
string content = form["file"].second;
```

然后再用 `stringstream` 传给 OSS。对教学项目和小文件可以接受。

如果要支持大文件，需要考虑：

- 分片上传。
- 流式转发，避免完整文件常驻内存。
- 上传进度。
- 上传失败重试。
- 文件大小限制。

OSS C++ SDK 支持更丰富的上传方式，当前第二期只使用简单上传。

---

## 二十二、第二期和第三期的区别

第二期：

```text
HTTP 上传请求
  -> 直接 PutObject 上传 OSS
  -> 写 MySQL
  -> 返回
```

特点：

- 实现简单。
- 上传成功代表 OSS 已经有文件。
- HTTP 请求需要等待 OSS 上传完成。

第三期：

```text
HTTP 上传请求
  -> 写 MySQL
  -> 投递 RabbitMQ 备份任务
  -> 立即返回

后台消费者
  -> 从 RabbitMQ 拉任务
  -> PutObject 上传 OSS
```

特点：

- 上传接口响应更快。
- HTTP 服务和 OSS 上传解耦。
- 需要处理消息队列、失败重试、数据一致性等问题。

`CloudDisk_V2` 是第二期，同步 OSS 版本，不包含 RabbitMQ。

---

## 二十三、关键代码索引

```text
CloudDiskServer.cc
  getEnvOrThrow()
    读取 OSS 配置，缺少变量则抛异常。

  create_oss_client()
    创建 OSS SDK 客户端。

  oss_object_name()
    生成 users/{uid}/{hashcode}。

  oss_upload_object()
    调用 PutObject 上传文件内容。

  oss_download_object()
    调用 GetObject 下载文件内容。

  CloudDiskServer::CloudDiskServer()
    InitializeSdk()。

  CloudDiskServer::~CloudDiskServer()
    ShutdownSdk()。

  register_auth_module()
    注册、登录接口。

  register_user_module()
    当前用户信息接口。

  register_file_module()
    文件列表、上传、下载接口。

CryptoUtil.cc
  generate_salt()
    生成密码 salt。

  hash_password()
    salt + password 计算哈希。

  generate_hashcode()
    根据文件内容计算 hashcode。

  generate_token()
    生成 JWT。

  verify_token()
    校验 JWT 并还原用户信息。

main.cc
  创建 CloudDiskServer，启动 8888 端口。

build.sh
  编译项目，生成 run.sh。

run.sh
  加载 .env，启动 server。
```

---

## 二十四、总结

`CloudDisk_V2` 的核心是把文件内容从服务器本地磁盘迁移到阿里云 OSS：

```text
第一期：MySQL 元数据 + 本地磁盘文件
第二期：MySQL 元数据 + OSS 对象内容
```

后端上传时：

```text
multipart 文件内容 -> hashcode -> OSS PutObject -> MySQL INSERT
```

后端下载时：

```text
fileId + JWT uid -> MySQL 查 filename/hashcode -> OSS GetObject -> HTTP 附件响应
```

配置通过 `.env` 管理，`run.sh` 负责把 `.env` 加载为进程环境变量，`getEnvOrThrow()` 负责读取并强制校验配置存在。

OSS SDK 生命周期由 `CloudDiskServer` 构造/析构函数管理，保证 `InitializeSdk()` 和 `ShutdownSdk()` 在整个服务进程中各执行一次。

当前第二期实现清晰、直接，适合理解 OSS 接入的完整链路。它的主要局限是上传请求同步等待 OSS、大文件会占用较多内存、OSS 成功但 MySQL 失败时缺少补偿清理。这些问题正是后续第三期引入 RabbitMQ 和更完整存储抽象时要继续改进的方向。
