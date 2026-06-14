# CloudDisk 第三期技术文档：引入 RabbitMQ 实现异步 OSS 上传

本文档说明 `CloudDisk` 第三期的后端实现。

第三期的核心目标是：在第二期 OSS 存储的基础上，引入 RabbitMQ 消息队列，把“上传文件到 OSS”从 HTTP 上传请求中拆出来，改成后台异步执行。

读完本文档后，你应该能掌握：

1. 为什么第三期要引入 RabbitMQ。
2. RabbitMQ 在本项目中的角色是什么。
3. 上传接口从同步 OSS 上传改成异步 OSS 上传后，完整链路如何流转。
4. `CloudDiskServer`、`OssStorage`、`RabbitMqOssUploader` 三个后端模块分别负责什么。
5. RabbitMQ 的 exchange、queue、routing key 在代码中如何声明和使用。
6. 消息体为什么使用 JSON + tempPath。
7. 后台消费者如何等待推送消息、上传 OSS、确认消息或重新入队。
8. `build.sh` / `run.sh` 如何自动检查并启动 RabbitMQ 容器。
9. 常见报错应该从哪里排查。

本文档主要讲后端。前端页面仍然沿用前两期的登录、注册、文件列表、上传、下载逻辑。

---

## 一、第三期解决什么问题

第二期中，用户上传文件时，后端流程大致是：

```text
浏览器
  -> POST /api/v1/files
  -> CloudDisk 后端解析 multipart/form-data
  -> 计算文件 hash
  -> 直接调用 OSS PutObject
  -> 写入 MySQL 文件元数据
  -> 返回上传成功
```

这个流程的问题是：HTTP 请求线程必须等待 OSS 上传完成。

如果文件比较大，或者 OSS 网络请求比较慢，用户会明显感觉上传接口响应时间变长。更重要的是，HTTP 服务和 OSS 上传操作耦合在一起：

- OSS 慢，上传接口就慢。
- OSS 临时异常，上传接口更容易失败。
- 上传高峰期，所有请求都直接压到 OSS 上传逻辑上。

第三期引入 RabbitMQ 后，上传流程改成：

```text
浏览器
  -> POST /api/v1/files
  -> CloudDisk 后端解析 multipart/form-data
  -> 计算文件 hash
  -> 写入 MySQL 文件元数据
  -> 发布一条 RabbitMQ 上传任务
  -> 返回上传成功

后台消费者线程
  -> 从 RabbitMQ 队列拉取上传任务
  -> 按 tempPath 读取临时文件
  -> 调用 OSS PutObject
  -> 上传成功后 ack 消息
```

变化的关键点是：HTTP 上传接口不再直接执行 OSS PutObject，而是把“需要上传 OSS”这件事变成一条消息，交给后台线程处理。

这就是消息队列最典型的用途：异步解耦。

---

## 二、第三期整体架构

当前后端可以分成四部分：

```text
浏览器前端
  |
  | HTTP
  v
CloudDiskServer
  |
  | 读写用户、文件元数据
  v
MySQL

CloudDiskServer
  |
  | 发布上传任务
  v
RabbitMQ
  |
  | 后台消费者线程拉取任务
  v
RabbitMqOssUploader
  |
  | 调用上传接口
  v
OssStorage
  |
  | PutObject / GetObject
  v
阿里云 OSS
```

更具体一点：

```text
POST /api/v1/files
  |
  | 1. 校验 JWT
  | 2. 解析上传文件
  | 3. 计算 hashcode
  | 4. 保存到本地临时目录
  | 5. INSERT tbl_file
  | 6. oss_uploader_.publish(uid, hashcode, temp_path)
  v
RabbitMQ: oss.direct -> oss.queue
  |
  | 后台线程 RabbitMqOssUploader::worker_loop()
  | 1. BasicConsume 订阅队列
  | 2. BasicConsumeMessage 阻塞等待推送消息
  | 3. JSON 解析
  | 4. 按 tempPath 读取临时文件
  | 5. oss_storage_.upload_object(...)
  | 6. 成功后删除临时文件
  | 7. BasicAck 或 BasicReject
  v
OSS: users/{uid}/{hashcode}
```

下载流程没有走 RabbitMQ。下载仍然是同步读取 OSS：

```text
GET /api/v1/file/{id}
  |
  | 1. 校验 JWT
  | 2. 查询 tbl_file
  | 3. oss_storage_.download_object(uid, hashcode, content)
  | 4. 返回文件内容
```

原因很简单：下载是用户立即需要文件内容的操作，不能异步返回。上传后的 OSS 写入可以稍后完成，但下载接口必须拿到内容才能响应浏览器。

---

## 三、当前目录结构

第三期后端代码经过职责拆分后，主要文件如下：

```text
CloudDisk/
├── CMakeLists.txt
├── main.cc
├── CloudDiskServer.h
├── CloudDiskServer.cc
├── CryptoUtil.h
├── CryptoUtil.cc
├── OssStorage.h
├── OssStorage.cc
├── RabbitMqOssUploader.h
├── RabbitMqOssUploader.cc
├── build.sh
├── run.sh
├── .env
└── www/
```

各文件职责：

```text
main.cc
  程序入口，创建 CloudDiskServer，注册路由，监听 8888 端口。

CloudDiskServer.h / CloudDiskServer.cc
  HTTP 服务层。
  负责注册路由、解析请求、校验登录、读写 MySQL、调用 OssStorage / RabbitMqOssUploader。

CryptoUtil.h / CryptoUtil.cc
  加密工具层。
  负责生成 salt、密码 hash、文件 hash、JWT 生成和校验。

OssStorage.h / OssStorage.cc
  OSS 存储层。
  负责 OSS SDK 生命周期、创建 OssClient、上传对象、下载对象。

RabbitMqOssUploader.h / RabbitMqOssUploader.cc
  RabbitMQ 异步 OSS 上传层。
  负责发布上传任务、声明交换机和队列、后台线程消费消息、调用 OssStorage 上传。

build.sh
  编译项目，并自动生成 run.sh。

run.sh
  加载 .env，检查 RabbitMQ 容器，等待 RabbitMQ 服务就绪，然后启动 server。
```

这次拆分的原则是“按职责边界拆”，不是为了拆而拆。

`CloudDiskServer.cc` 原来同时包含 HTTP、MySQL、OSS、RabbitMQ、线程、消息编码等逻辑，阅读成本太高。现在把 OSS 和 RabbitMQ 相关代码移出去后，`CloudDiskServer.cc` 主要保留接口流程，更适合作为学习 Web 后端业务逻辑的入口。

---

## 四、第三期核心对象关系

`CloudDiskServer` 内部组合了三个核心对象：

```cpp
wfrest::HttpServer server_;
OssStorage oss_storage_;
RabbitMqOssUploader oss_uploader_;
```

含义如下：

```text
server_
  负责 HTTP 监听和路由分发。

oss_storage_
  负责 OSS SDK 生命周期和 OSS 上传/下载。

oss_uploader_
  负责 RabbitMQ 生产者、消费者和后台线程。
```

构造函数：

```cpp
CloudDiskServer::CloudDiskServer()
    : oss_storage_()
    , oss_uploader_(oss_storage_)
{
    oss_uploader_.start();
}
```

逐步理解：

1. `oss_storage_()` 创建 OSS 存储对象。
2. `OssStorage` 构造函数中调用 `oss::InitializeSdk()`。
3. `oss_uploader_(oss_storage_)` 创建 RabbitMQ OSS 上传对象，并把 OSS 存储对象借给它。
4. `oss_uploader_.start()` 启动后台消费者线程。

析构函数：

```cpp
CloudDiskServer::~CloudDiskServer()
{
    oss_uploader_.stop();
}
```

含义是：服务器退出时，先停止 RabbitMQ 后台线程。等后台线程结束后，`OssStorage` 再析构并调用 `oss::ShutdownSdk()`。

这个顺序很重要：

```text
正确顺序：
  1. 停止 RabbitMQ 后台线程
  2. 确认后台线程不再上传 OSS
  3. 释放 OSS SDK

错误顺序：
  1. 先释放 OSS SDK
  2. 后台线程还在调用 OSS PutObject
  3. 可能产生未定义行为或崩溃
```

### 路由回调为什么要捕获 this

`CloudDiskServer.cc` 中有几处 lambda 写了 `[this]` 或者在捕获列表中包含 `this`。

这里的 `this` 是当前 `CloudDiskServer` 对象的指针。捕获 `this` 后，lambda 内部才能访问当前对象的成员变量，例如：

```cpp
oss_uploader_
oss_storage_
```

如果 lambda 没有捕获 `this`，那么它只能访问自己参数、全局变量、静态函数、以及显式捕获的局部变量，不能直接访问 `CloudDiskServer` 的成员对象。

上传接口外层路由：

```cpp
server_.POST("/api/v1/files", [this](const HttpReq* req, HttpResp* resp) {
    ...
});
```

这里捕获 `this`，是为了让上传接口内部后续的 MySQL 回调也能继续捕获 `this`。

上传接口的 MySQL 回调：

```cpp
resp->MySQL(DatabaseURL, sql, [resp,
                               this,
                               uid = user.id,
                               filename,
                               hashcode,
                               temp_path](MySQLResultCursor* cursor) {
    ...
    oss_uploader_.publish(uid, hashcode, temp_path);
});
```

这里必须捕获 `this`，因为 `oss_uploader_` 是 `CloudDiskServer` 的成员变量。MySQL 回调不是普通顺序代码，它会在 SQL 执行完成后异步触发。回调触发时，原来的路由函数已经返回，所以回调要靠捕获列表保存自己后续需要的东西。

捕获列表里每个值的作用不同：

```text
resp
  HTTP 响应对象指针，用来返回 JSON。

this
  当前 CloudDiskServer 对象指针，用来访问成员 oss_uploader_。

uid = user.id
  把当前用户 id 保存下来，避免外层 user 局部变量失效。

filename / hashcode / temp_path
  按值保存上传文件信息，供 SQL 完成后继续使用。
```

下载接口外层路由：

```cpp
server_.GET("/api/v1/file/{id}", [this](const HttpReq* req, HttpResp* resp) {
    ...
});
```

这里捕获 `this`，是为了让下载接口内部的 MySQL 回调能够访问成员 `oss_storage_`。

下载接口的 MySQL 回调：

```cpp
resp->MySQL(DatabaseURL, sql, [resp, uid = user.id, this](MySQLResultCursor* cursor) {
    ...
    oss_storage_.download_object(uid, hashcode, content);
});
```

这里必须捕获 `this`，因为 `oss_storage_` 是 `CloudDiskServer` 的成员变量，不是局部变量。

一句话总结：

```text
捕获 this，是为了在 HTTP 路由 lambda 和异步 MySQL 回调 lambda 中访问
CloudDiskServer 的成员对象 oss_uploader_ 和 oss_storage_。
```

当前项目中 `CloudDiskServer server;` 在 `main()` 中创建，并且程序退出前会先停止 HTTP 服务和后台线程，所以这些回调访问 `this` 的生命周期是可控的。生产项目如果对象生命周期更复杂，还需要用更严格的生命周期管理方式。

---

## 五、上传接口完整流程

上传接口在 `CloudDiskServer::register_file_module()` 中注册：

```text
POST /api/v1/files
```

它的完整流程是：

```text
1. 校验登录态
2. 检查请求是否为 multipart/form-data
3. 从 form 中取出字段 file
4. 读取 filename 和 content
5. 计算 hashcode
6. INSERT tbl_file 写入 MySQL 元数据
7. MySQL 写入成功后，调用 oss_uploader_.publish(...)
8. RabbitMQ 发布成功后，返回上传成功
```

### 1. 校验登录态

```cpp
User user;
if (!check_login(req, user)) {
    response_error(resp, HttpStatusUnauthorized, "无效的访问令牌");
    return;
}
```

`check_login()` 做三件事：

```text
1. 从 Authorization 请求头中取 Bearer Token
2. 调用 CryptoUtil::verify_token 校验 JWT
3. 校验成功后，把 token 中的用户 id、username、createdAt 写入 user
```

后续上传文件时，`user.id` 就是当前登录用户的用户 id。

### 2. 检查请求类型

```cpp
if (req->content_type() != MULTIPART_FORM_DATA) {
    response_error(resp, HttpStatusBadRequest, "请求格式有误");
    return;
}
```

浏览器上传文件时，前端使用 `FormData`：

```js
const formData = new FormData();
formData.append('file', file);
```

所以后端必须按 `multipart/form-data` 解析。普通 JSON 请求不能用来上传文件。

### 3. 读取上传文件

```cpp
Form& form = req->form();
if (!form.count("file")) {
    response_error(resp, HttpStatusBadRequest, "请求格式有误");
    return;
}

string filename = form["file"].first;
string content = form["file"].second;
```

在 wfrest 中，上传字段可以理解为：

```text
form["file"].first
  上传文件的原始文件名，例如 a.txt。

form["file"].second
  上传文件的真实内容，也就是二进制字节。
```

当前教学项目直接把文件内容放在 `std::string content` 中。`std::string` 可以保存二进制数据，不只是普通文本。

### 4. 计算文件 hash

```cpp
string hashcode = CryptoUtil::generate_hashcode(content.data(), content.size());
```

`hashcode` 的作用：

```text
1. 作为 OSS ObjectName 的一部分。
2. 下载时根据数据库中的 hashcode 定位 OSS 对象。
3. 避免直接使用原始文件名作为后端存储对象名。
```

OSS 中最终保存路径是：

```text
users/{uid}/{hashcode}
```

例如：

```text
uid = 3
hashcode = abc123...
OSS ObjectName = users/3/abc123...
```

### 5. 写入 MySQL 元数据

```cpp
INSERT INTO tbl_file (uid, filename, hashcode, size)
VALUES (...);
```

MySQL 保存的是文件元数据：

```text
uid
  文件属于哪个用户。

filename
  用户上传时的原始文件名，用于列表展示和下载文件名。

hashcode
  文件内容 hash，用于定位 OSS 对象。

size
  文件大小。
```

注意：MySQL 不保存文件内容。真实内容最终保存到 OSS。

### 6. 保存临时文件并发布 RabbitMQ 上传任务

上传接口会先把文件内容保存到本地临时目录：

```cpp
oss_uploader_.save_temp_file(uid, hashcode, content, temp_path)
```

MySQL 插入成功后，再执行：

```cpp
oss_uploader_.publish(uid, hashcode, temp_path)
```

这里的 `oss_uploader_` 是 `RabbitMqOssUploader` 对象。

`publish()` 会把当前上传文件变成一条 RabbitMQ 任务消息。消息体只保存临时文件路径，不保存真实文件内容。消息被成功投递到 RabbitMQ 后，HTTP 接口就可以返回上传成功。

这一步是第三期最核心的变化。

第二期：

```text
HTTP 请求线程 -> 直接 PutObject -> 等 OSS 返回 -> 再响应浏览器
```

第三期：

```text
HTTP 请求线程 -> 发布 RabbitMQ 消息 -> 响应浏览器
后台线程 -> 从 RabbitMQ 拉消息 -> PutObject
```

---

## 六、RabbitMQ 基础模型在项目中的对应关系

RabbitMQ 的基本模型是：

```text
Producer -> Exchange -> Queue -> Consumer
```

在本项目中对应为：

```text
Producer
  RabbitMqOssUploader::publish()

Exchange
  oss.direct

Queue
  oss.queue

Routing Key
  oss

Consumer
  RabbitMqOssUploader::worker_loop()
```

### 1. Exchange

代码中声明交换机：

```cpp
channel->DeclareExchange(RabbitMqExchange,
                         amqp::Channel::EXCHANGE_TYPE_DIRECT,
                         false,
                         true,
                         false);
```

含义：

```text
RabbitMqExchange = "oss.direct"
交换机类型 = direct
passive = false
durable = true
auto_delete = false
```

逐项解释：

```text
direct
  直接交换机。routing key 必须精确匹配，消息才会被路由到对应队列。

passive = false
  如果交换机不存在，就自动创建。

durable = true
  RabbitMQ 重启后交换机仍然存在。

auto_delete = false
  没有队列绑定时也不自动删除。
```

### 2. Queue

代码中声明队列：

```cpp
channel->DeclareQueue(RabbitMqQueue,
                      false,
                      true,
                      false,
                      false);
```

含义：

```text
RabbitMqQueue = "oss.queue"
passive = false
durable = true
exclusive = false
auto_delete = false
```

逐项解释：

```text
passive = false
  队列不存在时自动创建。

durable = true
  RabbitMQ 重启后队列仍然存在。

exclusive = false
  队列不是当前连接独占，后续可以多个消费者使用。

auto_delete = false
  连接断开后队列不会自动删除。
```

### 3. Binding

代码中绑定队列和交换机：

```cpp
channel->BindQueue(RabbitMqQueue, RabbitMqExchange, RabbitMqRoutingKey);
```

含义：

```text
把 oss.queue 绑定到 oss.direct。
绑定使用 routing key = oss。
```

之后生产者发布消息：

```cpp
channel->BasicPublish("oss.direct", "oss", message);
```

RabbitMQ 就会把消息路由到：

```text
oss.queue
```

---

## 七、RabbitMQ 配置

`RabbitMqOssUploader.cc` 中读取 RabbitMQ 配置：

```cpp
static const string RabbitMqUri =
    getEnvOrDefault("RABBITMQ_URI", "amqp://guest:guest@localhost:5672/%2f");

static const string RabbitMqExchange =
    getEnvOrDefault("RABBITMQ_EXCHANGE", "oss.direct");

static const string RabbitMqQueue =
    getEnvOrDefault("RABBITMQ_QUEUE", "oss.queue");

static const string RabbitMqRoutingKey =
    getEnvOrDefault("RABBITMQ_ROUTING_KEY", "oss");
```

默认配置对应本地 Docker RabbitMQ：

```text
RABBITMQ_URI=amqp://guest:guest@localhost:5672/%2f
RABBITMQ_EXCHANGE=oss.direct
RABBITMQ_QUEUE=oss.queue
RABBITMQ_ROUTING_KEY=oss
```

其中 `%2f` 表示 RabbitMQ 默认虚拟主机 `/`。

如果你的 RabbitMQ 用户名、密码、端口不同，可以在 `.env` 中覆盖：

```env
RABBITMQ_URI=amqp://guest:guest@localhost:5672/%2f
RABBITMQ_EXCHANGE=oss.direct
RABBITMQ_QUEUE=oss.queue
RABBITMQ_ROUTING_KEY=oss
```

---

## 八、消息体设计：JSON + tempPath

第三期的 RabbitMQ 消息体是 JSON：

```json
{
  "uid": 3,
  "hashcode": "abc123...",
  "tempPath": "./tmp/uploads/3-abc123-1790000000000000000.tmp"
}
```

字段含义：

```text
uid
  当前用户 id。消费者上传 OSS 时要生成 users/{uid}/{hashcode}。

hashcode
  文件内容 hash。消费者上传 OSS 时要生成 ObjectName。

tempPath
  上传接口写入的本地临时文件路径。
  消费者根据这个路径读取真实文件内容，再上传 OSS。
```

### 为什么不再把文件内容放进 RabbitMQ

之前为了教学直观，RabbitMQ 消息体可以直接放文件内容。那样做能快速理解：

```text
Producer -> Exchange -> Queue -> Consumer
```

但生产上通常不建议把大文件内容直接塞进 RabbitMQ，原因包括：

```text
1. RabbitMQ 更适合传递任务消息，不适合承载大文件内容。
2. 文件内容进入消息体后，消息会变大，占用 RabbitMQ 内存和磁盘。
3. 如果用 Base64 包装二进制，体积还会再增加约 1/3。
4. 大消息会影响队列吞吐，也会拖慢生产者和消费者。
```

所以当前实现改成：

```text
1. 上传接口先把文件内容保存到本地临时目录。
2. RabbitMQ 消息只保存 uid、hashcode、tempPath。
3. 消费者根据 tempPath 读取临时文件，再上传 OSS。
4. OSS 上传成功后，消费者删除临时文件。
```

这更接近真实项目中的做法：消息队列只传任务描述，不直接传大文件。

### 临时文件目录

临时文件默认保存到：

```text
./tmp/uploads
```

对应代码：

```cpp
static const string TempUploadDir =
    getEnvOrDefault("CLOUDDISK_TEMP_DIR", "./tmp/uploads");
```

如果想修改目录，可以在 `.env` 中配置：

```env
CLOUDDISK_TEMP_DIR=./tmp/uploads
```

临时文件名格式类似：

```text
{uid}-{hashcode}-{timestamp}.tmp
```

例如：

```text
3-abc123-1790000000000000000.tmp
```

加时间戳是为了避免同一个用户连续上传同一个文件时，临时文件互相覆盖。

### 临时文件清理时机

当前项目有三处清理逻辑：

```text
1. MySQL 元数据写入失败
   说明这个上传任务不会进入 RabbitMQ，立即删除临时文件。

2. RabbitMQ 消息发布失败
   说明后台消费者不会知道这个临时文件，立即删除临时文件。

3. 消费者上传 OSS 成功
   说明临时文件已经完成使命，立即删除临时文件。
```

如果 OSS 上传失败：

```text
消费者不会删除临时文件。
消息会重新入队。
下一次重试还需要根据 tempPath 读取这个临时文件。
```

```cpp
channel->BasicReject(envelope, true);
```

所以 OSS 临时失败时，临时文件必须保留。

项目中还增加了：

```text
.gitignore
```

并写入：

```text
tmp/
```

这样本地临时上传文件不会被 Git 跟踪。

---

## 九、生产者：保存临时文件并发布 tempPath

当前上传接口不是直接调用 `publish(uid, hashcode, content)`。

现在流程拆成两步：

```text
1. oss_uploader_.save_temp_file(uid, hashcode, content, temp_path)
2. oss_uploader_.publish(uid, hashcode, temp_path)
```

### 1. save_temp_file()

`save_temp_file()` 负责把 HTTP 请求中的文件内容写到本地临时目录。

伪代码：

```cpp
bool RabbitMqOssUploader::save_temp_file(int uid,
                                         const string& hashcode,
                                         const string& content,
                                         string& temp_path)
{
    fs::create_directories(TempUploadDir);

    fs::path path = make_temp_file_path(uid, hashcode);

    ofstream ofs(path, ios::binary);
    ofs.write(content.data(), content.size());

    temp_path = path.string();
    return true;
}
```

关键点：

```text
fs::create_directories()
  确保临时目录存在。

ios::binary
  按二进制写文件，避免文本模式影响文件字节。

temp_path
  输出参数。保存成功后，调用方拿到临时文件路径。
```

### 2. publish()

`publish()` 是 RabbitMQ 生产者逻辑。

核心流程：

```text
1. 创建 Channel
2. 声明 exchange / queue / binding
3. 构造 JSON 消息体
4. 创建 BasicMessage
5. 设置 ContentType
6. 设置 DeliveryMode 为持久化
7. BasicPublish 发布消息
```

伪代码：

```cpp
bool RabbitMqOssUploader::publish(int uid,
                                  const string& hashcode,
                                  const string& temp_path)
{
    Channel::ptr_t channel = create_rabbitmq_channel();
    declare_rabbitmq_topology(channel);

    json task;
    task["uid"] = uid;
    task["hashcode"] = hashcode;
    task["tempPath"] = temp_path;

    BasicMessage::ptr_t message = BasicMessage::Create(task.dump());
    message->ContentType("application/json");
    message->DeliveryMode(BasicMessage::dm_persistent);

    channel->BasicPublish(RabbitMqExchange, RabbitMqRoutingKey, message);
    return true;
}
```

### 为什么每次 publish 都声明拓扑

代码中每次发布消息前都会调用：

```cpp
declare_rabbitmq_topology(channel);
```

原因是简单可靠：

```text
1. 如果 exchange / queue / binding 已经存在，声明不会重复创建一份。
2. 如果 RabbitMQ 是刚启动的，第一次 publish 可以自动把需要的结构建好。
3. 不需要手动提前执行初始化脚本。
```

对学习项目来说，这种写法最直观。

### DeliveryMode 持久化

```cpp
message->DeliveryMode(amqp::BasicMessage::dm_persistent);
```

这表示消息希望持久化。

注意：消息持久化要配合 durable 队列才有意义。当前队列声明使用：

```cpp
durable = true
```

所以组合起来是：

```text
durable queue + persistent message
```

这可以提高 RabbitMQ 重启后消息保留的概率。

---

## 十、消费者：RabbitMqOssUploader::worker_loop()

`worker_loop()` 是后台消费者线程执行的函数。

它由：

```cpp
oss_uploader_.start();
```

启动。

### 1. 后台线程生命周期

`RabbitMqOssUploader::start()`：

```cpp
worker_ = thread(&RabbitMqOssUploader::worker_loop, this);
```

含义：

```text
创建一个后台线程。
后台线程在当前 RabbitMqOssUploader 对象上执行 worker_loop()。
```

`RabbitMqOssUploader::stop()`：

```cpp
stopping_ = true;
if (worker_.joinable()) {
    worker_.join();
}
```

含义：

```text
1. 设置停止标志。
2. 等待后台线程退出。
3. 确保程序退出时没有后台线程继续访问 OSS 或 RabbitMQ。
```

`stopping_` 使用 `std::atomic<bool>`，是因为它会被两个线程同时访问：

```text
主线程
  设置 stopping_ = true

后台线程
  while (!stopping_) 判断是否继续运行
```

### 2. 外层循环：断线重连

消费者外层是：

```cpp
while (!stopping_) {
    try {
        Channel::ptr_t channel = create_rabbitmq_channel();
        declare_rabbitmq_topology(channel);
        ...
    } catch (const exception& ex) {
        if (!stopping_) {
            cerr << "[RabbitMQ consumer ERROR] " << ex.what() << endl;
            this_thread::sleep_for(chrono::seconds(3));
        }
    }
}
```

这段逻辑的作用：

```text
1. RabbitMQ 正常时，建立连接并消费消息。
2. RabbitMQ 暂时不可用时，捕获异常。
3. 打印错误日志。
4. 等待 3 秒后重试连接。
```

所以即使你先启动 CloudDisk，再启动 RabbitMQ，后台线程也会不断重试，直到 RabbitMQ 可用。

### 3. 内层循环：BasicConsume 推送消息

当前代码使用：

```cpp
string consumer_tag = channel->BasicConsume(RabbitMqQueue,
                                            "",
                                            true,
                                            false,
                                            false,
                                            1);

channel->BasicConsumeMessage(consumer_tag, envelope, 1000)
```

参数解释：

```text
RabbitMqQueue
  要订阅哪个队列，这里是 oss.queue。

""
  consumer tag 为空，表示让 RabbitMQ 自动生成消费者标识。

true
  no_local。RabbitMQ 通常会忽略这个参数。

false
  no_ack=false，表示关闭自动确认。
  消费者处理成功后，必须手动 BasicAck。

false
  exclusive=false，表示不独占队列。

1
  prefetch count。一次只向这个消费者推送 1 条未确认消息。

1000
  最多阻塞等待 1000ms。
  超时还没有消息时返回 false，让后台线程有机会检查 stopping_ 并退出。
```

为什么使用带超时的阻塞等待？

```cpp
if (!channel->BasicConsumeMessage(consumer_tag, envelope, 1000)) {
    continue;
}
```

原因：

```text
1. 它不是 BasicGet 轮询，而是等待 RabbitMQ 推送消息。
2. 没有消息时线程会阻塞，不会空转占用 CPU。
3. 1000ms 超时后会回到循环顶部，检查 stopping_。
4. 如果使用无限阻塞版本，stop() 时后台线程可能无法及时退出。
```

### SimpleAmqpClient 的 global_qos 兼容问题

新版本 RabbitMQ 不推荐旧式 `global_qos`。

如果 SimpleAmqpClient 源码中仍然是：

```cpp
qos.global = m_impl->BrokerHasNewQosBehavior();
```

在新版本 RabbitMQ 上可能导致连接被服务端关闭。

当前项目使用推送模式的前提是已经把 SimpleAmqpClient 源码中两处 `qos.global` 改为：

```cpp
qos.global = false;
```

这样 `basic.qos` 使用的是 per-consumer QoS，和 RabbitMQ 新版本的推荐行为一致。

当前消费者日志：

```text
[RabbitMQ consumer] waiting for pushed messages from oss.queue
```

### 4. 消息格式校验

消费者取到消息后，先解析 JSON：

```cpp
json task = json::parse(body, nullptr, false);
```

这里使用 `parse(..., false)`，表示解析失败时不抛异常，而是返回 `discarded` 状态。

随后检查字段：

```cpp
task.contains("uid")
task["uid"].is_number_integer()
task.contains("hashcode")
task["hashcode"].is_string()
task.contains("tempPath")
task["tempPath"].is_string()
```

如果消息格式错误：

```cpp
channel->BasicReject(envelope, false);
```

第二个参数 `false` 表示不重新入队。

为什么不重新入队？

```text
消息格式已经坏了。
重新入队后，下次消费者拿到它还是坏的。
如果一直重新入队，会形成死循环。
```

### 5. 读取临时文件

```cpp
if (!read_temp_file(temp_path, content)) {
    channel->BasicReject(envelope, false);
    continue;
}
```

如果临时文件不存在或读取失败，说明这条任务已经无法继续完成。重新入队也找不回文件，所以当前项目直接丢弃，不重新入队。

### 6. 上传 OSS

```cpp
if (oss_storage_.upload_object(uid, hashcode, content)) {
    channel->BasicAck(envelope);
    remove_temp_file(temp_path);
} else {
    channel->BasicReject(envelope, true);
}
```

成功时：

```text
BasicAck
  告诉 RabbitMQ：这条消息已经处理完成，可以从队列删除。
```

失败时：

```text
BasicReject(envelope, true)
  告诉 RabbitMQ：这条消息处理失败，请重新放回队列，后续再试。
```

OSS 上传成功后，会删除本地临时文件，避免磁盘长期堆积。

OSS 上传失败时，消费者不会删除临时文件。因为消息重新入队后，下一次重试还要根据 `tempPath` 读取同一个临时文件。

这里体现了消息队列的重要价值：如果 OSS 临时失败，任务不会直接丢失，而是可以重新尝试。

---

## 十一、OssStorage 模块

`OssStorage` 只负责 OSS 存储，不关心 HTTP 和 RabbitMQ。

### 1. 配置读取

`OssStorage.cc` 从环境变量读取 OSS 配置：

```cpp
static const string OssEndpoint =
    getEnvOrThrow("ALIBABA_CLOUD_OSS_ENDPOINT");

static const string OssAccessKeyId =
    getEnvOrThrow("ALIBABA_CLOUD_ACCESS_KEY_ID");

static const string OssAccessKeySecret =
    getEnvOrThrow("ALIBABA_CLOUD_ACCESS_KEY_SECRET");

static const string OssBucketName =
    getEnvOrThrow("ALIBABA_CLOUD_OSS_BUCKET");

static const string OssRegion =
    getEnvOrThrow("ALIBABA_CLOUD_OSS_REGION");
```

这些变量应该写在 `.env` 中：

```env
ALIBABA_CLOUD_ACCESS_KEY_ID=...
ALIBABA_CLOUD_ACCESS_KEY_SECRET=...
ALIBABA_CLOUD_OSS_BUCKET=...
ALIBABA_CLOUD_OSS_ENDPOINT=...
ALIBABA_CLOUD_OSS_REGION=...
```

如果缺少任意变量，程序启动时会抛出异常：

```text
Missing environment variable: xxx
```

### 2. OSS SDK 生命周期

构造函数：

```cpp
OssStorage::OssStorage()
{
    oss::InitializeSdk();
}
```

析构函数：

```cpp
OssStorage::~OssStorage()
{
    oss::ShutdownSdk();
}
```

这是一种 RAII 思路：

```text
对象创建 -> 初始化资源
对象销毁 -> 释放资源
```

为什么不在每次上传时初始化 SDK？

```text
1. InitializeSdk / ShutdownSdk 是全局生命周期操作。
2. 每个请求都调用会增加不必要开销。
3. 并发时，一个请求 ShutdownSdk 可能影响另一个请求。
```

所以当前项目只在 `OssStorage` 生命周期内初始化一次、释放一次。

### 3. 创建 OssClient

```cpp
static unique_ptr<oss::OssClient> create_oss_client()
```

每次上传/下载都创建一个临时 `OssClient`。

这样做的原因：

```text
1. 生命周期非常清楚。
2. 不需要考虑全局静态对象析构顺序。
3. 不需要确认 OssClient 是否能安全被多个线程共享。
4. 教学项目请求量不大，按次创建可以接受。
```

### 4. ObjectName 规则

```cpp
return "users/" + to_string(uid) + "/" + hashcode;
```

例如：

```text
uid = 3
hashcode = abc123
ObjectName = users/3/abc123
```

优点：

```text
1. 不同用户互相隔离。
2. 同名文件不会覆盖。
3. ObjectName 不受原始文件名特殊字符影响。
4. 后续可以按 users/{uid}/ 前缀清理用户文件。
```

### 5. 上传对象

```cpp
bool OssStorage::upload_object(int uid,
                               const string& hashcode,
                               const string& content)
```

流程：

```text
1. 创建 OssClient
2. 生成 bucket_name
3. 生成 object name
4. 把 string content 包装成 stringstream
5. 创建 PutObjectRequest
6. client->PutObject(request)
7. 成功返回 true，失败打印日志并返回 false
```

为什么要用 `stringstream`？

OSS SDK 的 `PutObjectRequest` 需要的是流对象：

```cpp
shared_ptr<iostream>
```

而 wfrest 解析出的上传内容在内存字符串中：

```cpp
string content
```

所以用 `stringstream` 把内存中的字符串包装成一个“像文件一样可读取”的流。

### 6. 下载对象

```cpp
OssDownloadStatus OssStorage::download_object(int uid,
                                              const string& hashcode,
                                              string& content)
```

返回值不是 bool，而是：

```cpp
enum class OssDownloadStatus {
    Ok,
    NotFound,
    Failed
};
```

原因：

```text
Ok
  下载成功。

NotFound
  OSS 中对象不存在。HTTP 接口应该返回 404。

Failed
  OSS 服务异常、权限错误、网络错误等。HTTP 接口应该返回 500。
```

下载接口根据不同状态返回不同 HTTP 响应。

---

## 十二、下载接口为什么仍然同步访问 OSS

第三期只把“上传后的 OSS 写入”改成 RabbitMQ 异步处理，但没有把“OSS 下载”改成 RabbitMQ。

这不是遗漏，而是因为上传和下载的 HTTP 语义不同。

下载接口：

```text
GET /api/v1/file/{id}
```

流程：

```text
1. 校验登录态
2. 根据 fileId 和 uid 查询 tbl_file
3. 拿到 filename 和 hashcode
4. 调用 oss_storage_.download_object(...)
5. 设置响应头
6. 返回文件内容
```

下载不能异步的根本原因是：浏览器这一次 HTTP 请求的目标就是立刻拿到文件内容。

普通下载接口返回的不是 JSON，而是文件字节流：

```text
HTTP/1.1 200 OK
Content-Type: application/octet-stream
Content-Disposition: attachment; filename="a.txt"

<这里是真正的文件内容>
```

浏览器只有收到这个响应体，才能弹出下载或保存文件。

如果下载也改成 RabbitMQ：

```text
浏览器请求下载
  -> 后端发布下载任务
  -> 后端立即返回“任务已提交”
  -> 后台线程稍后从 OSS 下载文件
```

这里会出现一个关键问题：

```text
浏览器当前这次 GET 请求应该返回什么？
```

如果返回 JSON：

```json
{
  "status": "success",
  "message": "下载任务已提交"
}
```

那浏览器并没有拿到文件内容，普通下载流程就断了。

如果要让下载也异步，就必须把产品形态改成“离线下载”或“异步导出”：

```text
1. 用户请求下载
2. 后端返回一个 taskId
3. 后台任务从 OSS 下载、压缩或准备文件
4. 前端轮询 taskId 状态
5. 准备完成后，后端提供临时下载链接
6. 用户再发起第二次请求下载文件
```

这种模式适合：

```text
1. 批量打包多个文件
2. 导出大型报表
3. 准备超大文件
4. 生成压缩包
5. 需要长时间处理的离线任务
```

但当前项目的接口是普通单文件下载：

```text
GET /api/v1/file/{id}
```

它的语义就是：

```text
这次请求直接返回这个文件。
```

所以当前下载接口必须同步执行：

```text
查 MySQL -> 从 OSS GetObject -> 把文件内容写入 HTTP 响应
```

对比来看：

```text
上传接口
  用户把文件交给后端后，可以先返回“上传请求已接收”。
  OSS 写入可以稍后完成，所以适合 RabbitMQ 异步处理。

下载接口
  用户这次请求就是为了拿到文件内容。
  后端必须在这次响应中返回文件字节，所以不适合当前这种 RabbitMQ 异步处理。
```

所以当前项目只把“上传后的 OSS 写入”异步化，下载仍然同步读取 OSS。

---

## 十三、数据一致性说明

第三期上传接口的顺序是：

```text
1. 写 MySQL 文件元数据
2. 发布 RabbitMQ 上传任务
3. 后台消费者上传 OSS
```

这带来一个现象：上传接口返回成功时，OSS 对象可能还没有真正上传完成。

也就是说，系统变成了“最终一致性”：

```text
短时间内：
  MySQL 已有记录
  OSS 对象可能还在上传中

最终：
  RabbitMQ 消息被消费者处理
  OSS 对象上传完成
```

如果用户刚上传完立刻下载，理论上可能出现：

```text
MySQL 查到文件记录
OSS 还没有对象
下载接口返回 404 文件不存在
```

当前项目是学习项目，没有专门做“上传中”状态字段。生产项目通常会在 `tbl_file` 增加类似字段：

```text
status = pending / ready / failed
```

然后：

```text
上传接口写 status=pending
消费者上传 OSS 成功后更新 status=ready
下载接口只允许下载 ready 文件
```

当前项目为了保持简单，暂不引入这个状态机。

---

## 十四、run.sh 如何自动启动 RabbitMQ 容器

第三期依赖 RabbitMQ。如果 RabbitMQ 容器没启动，C++ 后台消费者连接 `127.0.0.1:5672` 会报：

```text
[RabbitMQ consumer ERROR] a socket error occurred
```

所以 `run.sh` 中加入了自动检查逻辑。

### 1. 加载 .env

```bash
set -a
source .env
set +a
```

这会把 `.env` 中的配置导出为环境变量，供 C++ 程序读取。

### 2. 确定容器名

```bash
RABBITMQ_CONTAINER=${RABBITMQ_CONTAINER:-rabbit}
```

含义：

```text
如果 .env 中配置了 RABBITMQ_CONTAINER，就使用配置值。
否则默认使用 rabbit。
```

如果你的容器不叫 `rabbit`，可以在 `.env` 中写：

```env
RABBITMQ_CONTAINER=my-rabbitmq
```

### 3. 检查 Docker 命令

```bash
if ! command -v docker >/dev/null 2>&1; then
    echo "Error: docker command not found. Please start RabbitMQ manually."
    exit 1
fi
```

如果系统没有 docker 命令，脚本直接报错。

### 4. 检查容器是否存在

```bash
docker inspect "$RABBITMQ_CONTAINER"
```

如果容器不存在，说明你还没有创建 RabbitMQ 容器，脚本会提示：

```text
Error: RabbitMQ container 'rabbit' not found.
Please create it first, or set RABBITMQ_CONTAINER in .env.
```

### 5. 容器存在但没运行时自动启动

```bash
RABBITMQ_RUNNING=$(docker inspect -f '{{.State.Running}}' "$RABBITMQ_CONTAINER")

if [ "$RABBITMQ_RUNNING" != "true" ]; then
    docker start "$RABBITMQ_CONTAINER" >/dev/null
fi
```

这一步只保证容器进程启动，不代表 RabbitMQ 应用已经准备好。

### 6. 等待 RabbitMQ 服务真正 ready

```bash
for i in {1..30}; do
    if docker exec "$RABBITMQ_CONTAINER" rabbitmq-diagnostics -q check_running >/dev/null 2>&1 \
        && docker exec "$RABBITMQ_CONTAINER" rabbitmq-diagnostics -q check_port_connectivity >/dev/null 2>&1; then
        echo "RabbitMQ is ready."
        break
    fi
    sleep 1
done
```

这里用了两个检查：

```text
check_running
  确认 RabbitMQ 应用已经启动。

check_port_connectivity
  确认 5672、15672 等端口已经可以连接。
```

为什么不用简单的 `docker start` 后立刻启动 server？

因为 RabbitMQ 容器启动后，还需要一段时间初始化 Erlang、加载插件、恢复队列、监听 5672 端口。

如果过早启动 C++ 服务，消费者可能会报 socket error。

---

## 十五、编译和运行

### 1. 编译

```bash
cd 课件/05_Web网盘项目/CloudDisk
./build.sh
```

`build.sh` 会执行：

```text
1. 创建 build 目录
2. cmake ..
3. make
4. 回到项目根目录
5. 重新生成 run.sh
```

注意：因为 `run.sh` 是由 `build.sh` 自动生成的，所以如果你改了 `run.sh` 的模板逻辑，应该改 `build.sh` 中的 heredoc 内容。

### 2. 运行

```bash
./run.sh
```

正常输出类似：

```text
RabbitMQ container is already running: rabbit
Waiting for RabbitMQ service to be ready...
RabbitMQ is ready.
[RabbitMQ consumer] waiting for pushed messages from oss.queue
```

如果容器停止，输出类似：

```text
Starting RabbitMQ container: rabbit
Waiting for RabbitMQ service to be ready...
RabbitMQ is ready.
[RabbitMQ consumer] waiting for pushed messages from oss.queue
```

看到 `waiting for pushed messages from oss.queue`，说明后台消费者已经订阅 RabbitMQ 队列，正在阻塞等待 RabbitMQ 推送消息。

---

## 十六、CMake 配置

第三期新增了两个源文件：

```cmake
OssStorage.cc
RabbitMqOssUploader.cc
```

所以 `CMakeLists.txt` 中：

```cmake
add_executable(server
    CryptoUtil.cc
    OssStorage.cc
    RabbitMqOssUploader.cc
    CloudDiskServer.cc
    main.cc
)
```

第三期新增 RabbitMQ 客户端库：

```cmake
target_link_libraries(server PRIVATE
    alibabacloud-oss-cpp-sdk
    SimpleAmqpClient
    rabbitmq
    curl
    crypto
    ssl
    jwt
    wfrest
    pthread
)
```

库含义：

```text
alibabacloud-oss-cpp-sdk
  阿里云 OSS C++ SDK。

SimpleAmqpClient
  C++ RabbitMQ/AMQP 客户端封装。

rabbitmq
  rabbitmq-c 底层 C 客户端库。

curl / crypto / ssl / pthread
  OSS SDK 和网络加密相关依赖。

jwt
  JWT 登录令牌依赖。

wfrest
  Web 后端框架。
```

---

## 十七、RabbitMQ 容器要求

当前脚本默认容器名是：

```text
rabbit
```

并且要求容器端口映射类似：

```text
5672  -> 5672
15672 -> 15672
```

可以用下面命令查看：

```bash
docker ps -a
docker inspect rabbit --format '{{json .HostConfig.PortBindings}}'
```

如果还没有创建容器，可以参考：

```bash
docker run -d \
  --name rabbit \
  -p 5672:5672 \
  -p 15672:15672 \
  rabbitmq:management
```

如果容器已经创建但停止：

```bash
docker start rabbit
```

不过正常情况下，现在直接执行：

```bash
./run.sh
```

脚本会自动启动 `rabbit` 容器。

---

## 十八、常见问题排查

### 1. `[RabbitMQ consumer ERROR] a socket error occurred`

常见原因：

```text
1. RabbitMQ 容器没有启动。
2. RabbitMQ 5672 端口没有映射到主机。
3. RabbitMQ 刚启动，还没有完成端口监听。
4. RabbitMQ 用户名、密码、vhost 配置不对。
```

当前 `run.sh` 已经处理了前 3 类常见问题：

```text
1. 检查容器存在
2. 自动 docker start
3. 等待 check_running 和 check_port_connectivity
```

如果仍然报错，可以检查：

```bash
docker ps
docker logs --tail 100 rabbit
curl http://127.0.0.1:15672/
```

### 2. RabbitMQ 容器不存在

报错：

```text
Error: RabbitMQ container 'rabbit' not found.
```

解决方式：

```text
1. 创建名为 rabbit 的容器。
2. 或者在 .env 中设置 RABBITMQ_CONTAINER=实际容器名。
```

### 3. RabbitMQ 管理页面打不开

检查端口映射：

```bash
docker ps
```

应该能看到：

```text
0.0.0.0:15672->15672/tcp
```

然后访问：

```text
http://127.0.0.1:15672/
```

默认账号密码通常是：

```text
guest / guest
```

### 4. `Missing environment variable`

例如：

```text
Missing environment variable: ALIBABA_CLOUD_OSS_BUCKET
```

说明 `.env` 中缺少 OSS 配置，或者你没有通过 `./run.sh` 启动程序。

正确方式：

```bash
./run.sh
```

不要直接：

```bash
./server
```

因为直接执行 `./server` 不会自动加载 `.env`。

### 5. 上传成功后立刻下载返回文件不存在

第三期上传是异步 OSS 上传。

上传接口返回成功时：

```text
MySQL 元数据已经写入
RabbitMQ 任务已经发布
OSS 可能还在后台上传
```

如果立刻下载，有可能 OSS 对象还没生成。

当前学习项目没有加文件状态字段，所以这是最终一致性带来的正常现象。

### 6. Windows 浏览器下载中文文件名变成乱码

现象：

```text
上传文件：数据库表结构.md
下载后文件名：æ_°æ_®åº_è¡¨ç»_æ__.md
```

原因在 HTTP 下载响应头 `Content-Disposition`。

早期写法通常是：

```text
Content-Disposition: attachment; filename="数据库表结构.md"
```

问题是 `filename=` 对非 ASCII 文件名支持不稳定。中文文件名在 HTTP 头里实际是 UTF-8 字节，如果浏览器或下载逻辑按其它编码解释这些字节，就会看到 `æ...` 这种乱码。

第三期当前代码使用更兼容的写法：

```text
Content-Disposition:
  attachment;
  filename="________.md";
  filename*=UTF-8''%E6%95%B0%E6%8D%AE%E5%BA%93%E8%A1%A8%E7%BB%93%E6%9E%84.md
```

其中：

```text
filename
  ASCII 兜底文件名，给老浏览器使用。

filename*
  标准 UTF-8 文件名，使用 RFC 5987 百分号编码。
  现代浏览器会优先使用它，所以中文可以正常显示。
```

后端相关代码在 `CloudDiskServer.cc`：

```text
content_disposition_fallback_filename()
  生成 ASCII 兜底文件名。

encode_rfc5987_filename()
  把 UTF-8 文件名转成 filename*=UTF-8''... 格式。
```

前端因为使用的是 `fetch -> blob -> a.download`，不是浏览器原生直接下载，所以也要自己解析响应头。`www/static/api.js` 中会优先解析：

```text
filename*=UTF-8''...
```

然后再退回到：

```text
filename=...
```

这样 Windows 上下载中文文件名时，前端传给 `a.download` 的就是正确的中文文件名。

---

## 十九、为什么当前项目不继续过度拆分

当前拆分为：

```text
CloudDiskServer
OssStorage
RabbitMqOssUploader
CryptoUtil
```

这是比较适合教学项目的粒度。

没有继续拆成：

```text
ConfigManager
ResponseUtil
SqlBuilder
AuthMiddleware
FileRepository
MessageCodec
```

原因：

```text
1. 项目规模还不大。
2. 太多类会增加学习成本。
3. 当前最复杂的职责就是 OSS 和 RabbitMQ，把它们拆出去已经能明显降低 CloudDiskServer.cc 的负担。
4. 保持接口流程集中在 CloudDiskServer.cc 中，更方便初学者按 HTTP 路由阅读业务。
```

如果后续进入第四期微服务架构，再继续拆分会更自然。

---

## 二十、第三期你应该重点掌握什么

学习第三期时，建议按下面顺序读代码：

```text
1. main.cc
   看服务器如何创建、注册路由、启动。

2. CloudDiskServer.h
   看 CloudDiskServer 组合了哪些模块。

3. CloudDiskServer.cc
   重点看 POST /api/v1/files 和 GET /api/v1/file/{id}。

4. RabbitMqOssUploader.h / RabbitMqOssUploader.cc
   看 RabbitMQ 如何发布消息、如何后台消费消息。

5. OssStorage.h / OssStorage.cc
   看 OSS SDK 生命周期、上传和下载。

6. build.sh / run.sh
   看运行前如何确保 RabbitMQ 容器可用。
```

第三期最重要的思想不是某一行 API，而是这个架构变化：

```text
同步调用：
  HTTP 请求线程直接做耗时操作。

异步解耦：
  HTTP 请求线程只发布任务，后台消费者慢慢处理任务。
```

RabbitMQ 在这里的价值是：

```text
1. 把上传接口和 OSS 上传解耦。
2. 让 HTTP 响应不再等待 OSS PutObject。
3. 在 OSS 临时失败时，可以通过消息重新入队重试。
4. 为后续微服务拆分打基础。
```

---

## 二十一、当前第三期的局限

为了保持学习项目简单，当前实现没有处理所有生产级边界。

主要局限：

```text
1. 临时文件目录没有独立的定时清理任务，极端异常退出时可能残留文件。
2. tbl_file 没有 status 字段，无法区分 pending / ready / failed。
3. OSS 上传失败后会重新入队，但没有最大重试次数。
4. 没有死信队列。
5. 没有后台任务监控页面。
6. 没有对 RabbitMQ 消息体做版本号设计。
```

这些不是当前阶段的重点。

如果要继续增强，可以考虑：

```text
1. 增加 tbl_file.status。
2. 消费者上传成功后更新 status=ready。
3. 消费失败多次后进入 failed 状态。
4. RabbitMQ 增加死信交换机和死信队列。
5. 增加定时清理任务，删除过期临时文件。
```

当前第三期已经完整展示了消息队列引入后的核心后端流程。
