# CloudDisk 第四期技术文档：基于 srpc + Protobuf 的微服务改造

本文档说明当前 `CloudDisk` 第四期代码的后端实现。

第四期的核心目标是：在第三期 RabbitMQ 异步上传 OSS 的基础上，把单体 Web 网盘按职责拆成 API Gateway、三个 srpc 微服务和一个独立 OSS 上传 worker。

读完本文档后，你应该能掌握：

1. 第四期为什么要从单体进程改造成多进程微服务。
2. API Gateway、AuthService、UserService、FileMetaService、OssUploadWorker 分别负责什么。
3. HTTP 请求如何通过 `resp->add_task()` 接入 srpc 异步调用。
4. Protobuf IDL 如何定义认证、用户资料、文件元数据 RPC。
5. 上传、下载、登录、文件列表在第四期中的完整链路。
6. RabbitMQ 和 OSS 在第四期中如何从网关进程拆到 worker。
7. `build.sh` / `run.sh` 如何生成代码、编译并启动所有进程。
8. 当前代码的配置项、目录结构、数据库表依赖和常见排查点。

前端页面和 HTTP API 地址继续沿用前三期，浏览器仍然只访问 `http://127.0.0.1:8888`。

---

## 一、第四期解决什么问题

第三期已经把 OSS 上传改成 RabbitMQ 异步任务，但整体仍然是一个单体后端：

```text
Browser
  -> CloudDiskServer
      -> HTTP 路由、静态资源、JSON、multipart
      -> 注册、登录、JWT
      -> tbl_user / tbl_file SQL
      -> RabbitMQ 发布和后台消费线程
      -> OSS 上传下载
```

这个结构可以运行，但职责仍然集中在一个进程和一个核心类中。第四期把它改造成：

```text
Browser
  |
  | HTTP
  v
API Gateway :8888
  |
  | srpc
  +--> AuthService     :9001 -> tbl_user
  |
  | srpc
  +--> UserService     :9002 -> tbl_user
  |
  | srpc
  +--> FileMetaService :9003 -> tbl_file
  |
  | RabbitMQ publish
  v
RabbitMQ: oss.direct -> oss.queue
  |
  | consume
  v
OssUploadWorker -> Aliyun OSS
```

拆分后，网关只面向浏览器，业务数据读写交给 RPC 服务，后台 OSS 上传交给独立 worker。这样代码边界更清楚，也为后续服务注册、配置中心、横向扩容打基础。

---

## 二、当前进程和端口

当前 CMake 会生成 5 个可执行程序，全部输出到 `bin/`：

```text
bin/server             API Gateway，监听 8888
bin/auth_service       认证微服务，默认监听 9001
bin/user_service       用户资料微服务，默认监听 9002
bin/filemeta_service   文件元数据微服务，默认监听 9003
bin/oss_upload_worker  OSS 上传后台 worker，不监听端口
```

`run.sh` 会按顺序启动：

```text
1. 检查并启动 RabbitMQ Docker 容器
2. 等待 RabbitMQ ready
3. 后台启动 auth_service
4. 后台启动 user_service
5. 后台启动 filemeta_service
6. 后台启动 oss_upload_worker
7. 前台启动 server
```

按 `Ctrl+C` 退出时，`run.sh` 会清理后台微服务和 worker。

---

## 三、目录结构

第四期代码已经按职责拆分：

```text
CloudDisk/
├── CMakeLists.txt
├── build.sh
├── run.sh
├── .env
├── README.md
├── docs/
│   ├── 项目目录结构说明.md
│   └── 第四期：微服务架构设计文档&改造方案.md
├── proto/
│   └── cloud_disk.proto
├── rpc_gen/
│   ├── cloud_disk.pb.h
│   ├── cloud_disk.pb.cc
│   ├── cloud_disk.srpc.h
│   ├── client.pb_skeleton.cc
│   └── server.pb_skeleton.cc
├── src/
│   ├── common/
│   │   ├── CryptoUtil.*
│   │   ├── OssStorage.*
│   │   ├── RabbitMqOssUploader.*
│   │   └── ServiceCommon.*
│   ├── gateway/
│   │   ├── main.cc
│   │   ├── CloudDiskServer.h
│   │   └── CloudDiskServer.cc
│   ├── services/
│   │   ├── AuthServiceMain.cc
│   │   ├── UserServiceMain.cc
│   │   └── FileMetaServiceMain.cc
│   └── worker/
│       └── OssUploadWorkerMain.cc
├── www/
├── build/
└── bin/
```

`rpc_gen/` 是生成代码目录，不手动修改。需要改 RPC 接口时，先改 `proto/cloud_disk.proto`，再执行 `./build.sh` 重新生成。

---

## 四、模块职责

### 1. API Gateway

代码位置：

```text
src/gateway/main.cc
src/gateway/CloudDiskServer.h
src/gateway/CloudDiskServer.cc
```

监听端口：`8888`。

保留职责：

```text
1. 静态资源服务
   GET /
   GET /static/*

2. 对浏览器暴露 HTTP API
   POST /api/v1/auth/register
   POST /api/v1/auth/login
   GET  /api/v1/user/me
   GET  /api/v1/files
   POST /api/v1/files
   GET  /api/v1/file/{id}

3. HTTP 请求解析
   JSON body
   multipart/form-data
   Authorization: Bearer ...
   path 参数

4. HTTP 响应封装
   成功 JSON
   失败 JSON
   文件下载响应头

5. 文件字节流处理
   上传时接收文件内容、计算 hashcode、保存临时文件
   下载时从 OSS 读取内容并写回 HTTP 响应

6. 调用后端 srpc 服务
   AuthService
   UserService
   FileMetaService

7. 发布 RabbitMQ OSS 上传任务
```

网关不再直接做：

```text
1. tbl_user 注册、登录 SQL
2. 密码 salt/hash 业务
3. JWT 生成和校验核心逻辑
4. tbl_file 元数据 SQL
5. RabbitMQ 后台消费
6. OSS 异步上传
```

### 2. AuthService

代码位置：`src/services/AuthServiceMain.cc`

默认端口：`9001`，可用 `AUTH_SERVICE_PORT` 覆盖。

职责：

```text
1. Register：注册用户
2. Login：登录用户
3. VerifyToken：校验 JWT
4. 生成 salt、密码 hash、JWT
5. 访问 tbl_user
```

对应 Protobuf 服务：

```protobuf
service AuthService {
  rpc Register(RegisterRequest) returns (RegisterResponse);
  rpc Login(LoginRequest) returns (LoginResponse);
  rpc VerifyToken(VerifyTokenRequest) returns (VerifyTokenResponse);
}
```

### 3. UserService

代码位置：`src/services/UserServiceMain.cc`

默认端口：`9002`，可用 `USER_SERVICE_PORT` 覆盖。

职责：

```text
1. GetUserProfile：按 user_id 查询当前用户资料
2. 访问 tbl_user
3. 过滤 tomb=0 的有效用户
```

对应 Protobuf 服务：

```protobuf
service UserService {
  rpc GetUserProfile(GetUserProfileRequest) returns (GetUserProfileResponse);
}
```

第三期 `/api/v1/user/me` 直接从 JWT claims 还原用户信息。第四期改成先通过 `AuthService.VerifyToken` 得到 `user_id`，再调用 `UserService.GetUserProfile` 查询数据库中的最新用户资料。

### 4. FileMetaService

代码位置：`src/services/FileMetaServiceMain.cc`

默认端口：`9003`，可用 `FILEMETA_SERVICE_PORT` 覆盖。

职责：

```text
1. ListFiles：查询当前用户文件列表
2. CreateFile：创建文件元数据
3. GetFileForDownload：下载前查询 filename/hashcode
4. 访问 tbl_file
```

它不负责：

```text
1. HTTP
2. multipart/form-data
3. 临时文件
4. RabbitMQ
5. OSS
6. 文件内容字节流
```

对应 Protobuf 服务：

```protobuf
service FileMetaService {
  rpc ListFiles(ListFilesRequest) returns (ListFilesResponse);
  rpc CreateFile(CreateFileRequest) returns (CreateFileResponse);
  rpc GetFileForDownload(GetFileForDownloadRequest) returns (GetFileForDownloadResponse);
}
```

### 5. OssUploadWorker

代码位置：`src/worker/OssUploadWorkerMain.cc`

这是后台 worker，不是 srpc 服务，不监听端口。

职责：

```text
1. 创建 OssStorage，初始化 OSS SDK
2. 创建 RabbitMqOssUploader 并传入 OssStorage
3. 消费 RabbitMQ 队列 oss.queue
4. 读取消息中的 uid/hashcode/tempPath
5. 从 tempPath 读取临时文件
6. 调用 OssStorage::upload_object()
7. 上传成功后 BasicAck 并删除临时文件
8. 上传失败时 BasicReject(..., true) 重新入队
```

第四期开始，网关使用 `RabbitMqOssUploader` 的默认构造函数，只发布任务；worker 使用传入 `OssStorage&` 的构造函数，负责消费和上传。

---

## 五、Protobuf 和 srpc 接口

IDL 文件：`proto/cloud_disk.proto`

生成命令在 `build.sh` 中：

```bash
mkdir -p rpc_gen
protoc -I ./proto --cpp_out=./rpc_gen proto/cloud_disk.proto
srpc_generator protobuf proto/cloud_disk.proto ./rpc_gen
```

主要消息：

```text
CommonResult
  code: 0 表示业务成功
  message: 业务提示

UserIdentity
  user_id
  username
  created_at

RegisterRequest / RegisterResponse
LoginRequest / LoginResponse
VerifyTokenRequest / VerifyTokenResponse

GetUserProfileRequest / GetUserProfileResponse

ListFilesRequest / ListFilesResponse
CreateFileRequest / CreateFileResponse
GetFileForDownloadRequest / GetFileForDownloadResponse
```

这里区分两类错误：

```text
RPC 通信层错误：
  服务不可达、网络失败、序列化失败。
  网关通过 ctx->success() 判断，统一返回 500。

业务层错误：
  用户名已存在、密码错误、token 无效、文件不存在。
  微服务通过 CommonResult.code/message 返回。
  网关把 400/401/404/409/500 转成 HTTP 状态码。
```

---

## 六、HTTP 与 srpc 如何接起来

第四期网关调用 srpc 时，不直接 `task->start()`，而是使用：

```cpp
resp->add_task(task);
```

原因是 wfrest 的 HTTP 回调返回后，如果异步任务没有挂到当前 HTTP 响应所在的 Workflow 序列上，框架可能认为响应已经结束，浏览器会收到空响应。

当前代码中的典型流程：

```text
HTTP 回调
  -> 创建 srpc task
  -> task->serialize_input(&rpc_req)
  -> resp->add_task(task)
  -> srpc 回调中写 HttpResp
  -> HTTP 响应真正结束
```

需要登录态的接口先调用：

```text
verify_request_async()
  -> 从 Authorization 解析 Bearer Token
  -> AuthService.VerifyToken
  -> token 有效后执行 next(identity)
```

部分接口会串联两个 RPC，例如 `/api/v1/user/me`：

```text
GET /api/v1/user/me
  -> AuthService.VerifyToken
  -> UserService.GetUserProfile
  -> response_success()
```

上传接口也会串联两个 RPC：

```text
POST /api/v1/files
  -> AuthService.VerifyToken
  -> 保存临时文件
  -> FileMetaService.CreateFile
  -> RabbitMQ publish
  -> response_success()
```

---

## 七、核心业务链路

### 1. 注册

```text
Browser
  -> POST /api/v1/auth/register
  -> API Gateway 解析 JSON，检查 username/password/confirm
  -> AuthService.Register
      -> generate_salt()
      -> hash_password()
      -> INSERT tbl_user
  -> API Gateway 返回 201 JSON
```

网关只做 HTTP 表单级校验，真正写用户表由 `AuthService` 完成。

### 2. 登录

```text
Browser
  -> POST /api/v1/auth/login
  -> API Gateway 解析 JSON
  -> AuthService.Login
      -> SELECT tbl_user WHERE username=? AND tomb=0
      -> 用数据库 salt 重新计算密码 hash
      -> generate_token()
  -> API Gateway 返回 accessToken/tokenType/user
```

前端拿到 `accessToken` 后，在后续请求中携带：

```text
Authorization: Bearer <token>
```

### 3. 获取当前用户资料

```text
Browser
  -> GET /api/v1/user/me
  -> API Gateway 解析 Bearer Token
  -> AuthService.VerifyToken
  -> UserService.GetUserProfile(user_id)
      -> SELECT id, username, created_at FROM tbl_user WHERE id=? AND tomb=0
  -> API Gateway 返回用户资料 JSON
```

### 4. 获取文件列表

```text
Browser
  -> GET /api/v1/files
  -> API Gateway 解析 Bearer Token
  -> AuthService.VerifyToken
  -> FileMetaService.ListFiles(user_id)
      -> SELECT id, filename, size, created_at, last_update
         FROM tbl_file
         WHERE uid=?
         ORDER BY last_update DESC, id DESC
  -> API Gateway 返回 files 数组
```

### 5. 上传文件

```text
Browser
  -> POST /api/v1/files multipart/form-data
  -> API Gateway 解析 Bearer Token
  -> API Gateway 校验 content_type 和 file 字段
  -> API Gateway 拷贝 filename/content
  -> API Gateway 计算 hashcode
  -> AuthService.VerifyToken
  -> API Gateway 保存本地临时文件
  -> FileMetaService.CreateFile
      -> INSERT tbl_file(uid, filename, hashcode, size)
  -> API Gateway 发布 RabbitMQ 消息
  -> API Gateway 返回 201 JSON

OssUploadWorker
  -> BasicConsume oss.queue
  -> 解析 JSON 消息 uid/hashcode/tempPath
  -> 读取 tempPath 文件内容
  -> OssStorage::upload_object(uid, hashcode, content)
  -> OSS ObjectName: users/{uid}/{hashcode}
  -> BasicAck
  -> 删除临时文件
```

RabbitMQ 消息体格式：

```json
{
  "uid": 1,
  "hashcode": "file-content-sha256",
  "tempPath": "./tmp/uploads/1-file-content-sha256-xxxx.tmp"
}
```

第四期仍然使用 `tempPath`，所以 API Gateway 和 OssUploadWorker 必须运行在同一台机器上，或者共享同一个临时目录挂载。多机部署时不能只传本地路径，应改成共享存储 key、OSS 临时对象 key 或浏览器直传 OSS。

### 6. 下载文件

```text
Browser
  -> GET /api/v1/file/{id}
  -> API Gateway 解析 Bearer Token
  -> AuthService.VerifyToken
  -> FileMetaService.GetFileForDownload(user_id, file_id)
      -> SELECT filename, hashcode
         FROM tbl_file
         WHERE id=? AND uid=?
  -> API Gateway 创建 OssStorage
  -> OssStorage::download_object(uid, hashcode, content)
  -> API Gateway 设置 Content-Type / Content-Disposition
  -> API Gateway 返回文件字节流
```

下载没有异步化，因为浏览器本次 HTTP 响应必须直接拿到文件内容。

---

## 八、MySQL、OSS、RabbitMQ 分别保存什么

MySQL 保存元数据：

```text
tbl_user:
  id
  username
  password
  salt
  created_at
  tomb

tbl_file:
  id
  uid
  filename
  hashcode
  size
  created_at
  last_update
```

OSS 保存文件内容：

```text
users/{uid}/{hashcode} -> 文件二进制内容
```

RabbitMQ 保存待上传任务：

```text
exchange:    oss.direct
queue:       oss.queue
routing key: oss
message:     {"uid": ..., "hashcode": "...", "tempPath": "..."}
```

当前数据库连接在 `src/common/ServiceCommon.cc` 中写死：

```text
mysql://root:123456@localhost/CloudDisk
```

这是课程项目的简化写法。真实项目应改成环境变量、配置文件或配置中心，并使用参数化 SQL。

---

## 九、配置项

`run.sh` 会加载当前目录下的 `.env`：

```bash
set -a
source .env
set +a
```

当前代码使用的环境变量：

```text
OSS 必填：
  ALIBABA_CLOUD_ACCESS_KEY_ID
  ALIBABA_CLOUD_ACCESS_KEY_SECRET
  ALIBABA_CLOUD_OSS_BUCKET
  ALIBABA_CLOUD_OSS_ENDPOINT
  ALIBABA_CLOUD_OSS_REGION

RabbitMQ：
  RABBITMQ_URI
  RABBITMQ_EXCHANGE
  RABBITMQ_QUEUE
  RABBITMQ_ROUTING_KEY
  RABBITMQ_CONTAINER

临时文件：
  CLOUDDISK_TEMP_DIR

微服务地址：
  AUTH_SERVICE_HOST
  AUTH_SERVICE_PORT
  USER_SERVICE_HOST
  USER_SERVICE_PORT
  FILEMETA_SERVICE_HOST
  FILEMETA_SERVICE_PORT
```

默认值：

```text
AUTH_SERVICE_HOST=127.0.0.1
AUTH_SERVICE_PORT=9001
USER_SERVICE_HOST=127.0.0.1
USER_SERVICE_PORT=9002
FILEMETA_SERVICE_HOST=127.0.0.1
FILEMETA_SERVICE_PORT=9003

RABBITMQ_URI=amqp://guest:guest@localhost:5672/%2f
RABBITMQ_EXCHANGE=oss.direct
RABBITMQ_QUEUE=oss.queue
RABBITMQ_ROUTING_KEY=oss
RABBITMQ_CONTAINER=rabbit

CLOUDDISK_TEMP_DIR=./tmp/uploads
```

`OssStorage.cc` 对 OSS 配置使用 `getEnvOrThrow()`，缺少任意 OSS 必填变量时，相关进程会启动失败并打印缺失变量名。

---

## 十、构建和运行

进入项目目录：

```bash
cd 课件/05_Web网盘项目/CloudDisk
```

普通构建：

```bash
./build.sh
```

干净构建：

```bash
./build.sh clean
```

构建脚本会做三件事：

```text
1. 根据 proto/cloud_disk.proto 重新生成 rpc_gen 代码
2. 使用 CMake + make 编译 5 个可执行程序
3. 重新生成 run.sh 并添加可执行权限
```

启动：

```bash
./run.sh
```

浏览器访问：

```text
http://127.0.0.1:8888
```

---

## 十一、CMake 目标和依赖

`CMakeLists.txt` 中的目标：

```text
server
  API Gateway
  依赖 wfrest、srpc、protobuf、workflow、OSS SDK、RabbitMQ、OpenSSL、jwt

auth_service
  认证微服务
  依赖 srpc、protobuf、workflow、OpenSSL、jwt

user_service
  用户资料微服务
  依赖 srpc、protobuf、workflow

filemeta_service
  文件元数据微服务
  依赖 srpc、protobuf、workflow

oss_upload_worker
  OSS 上传 worker
  依赖 OSS SDK、RabbitMQ、workflow、OpenSSL
```

编译 `OssStorage.cc` 的目标需要 `-fno-rtti`：

```text
server
oss_upload_worker
```

三个业务微服务不编译 OSS SDK 代码，所以不需要 `-fno-rtti`。

---

## 十二、常见问题排查

### 1. 网关返回 500，日志出现 RPC FAILED

通常是后端微服务没有启动、端口不对或端口被占用。

检查：

```text
auth_service 是否监听 9001
user_service 是否监听 9002
filemeta_service 是否监听 9003
AUTH_SERVICE_HOST / AUTH_SERVICE_PORT 等配置是否和实际一致
```

### 2. 注册或登录失败，日志出现 MySQL packet ERROR

检查：

```text
MySQL 是否启动
CloudDisk 数据库是否存在
tbl_user / tbl_file 表结构是否匹配
src/common/ServiceCommon.cc 中 DatabaseURL 是否正确
username 唯一键冲突时注册会返回“用户名已存在”
```

### 3. 上传接口返回成功，但 OSS 没有文件

上传 HTTP 成功只表示：

```text
1. 文件临时保存成功
2. tbl_file 元数据创建成功
3. RabbitMQ 任务发布成功
```

真正 OSS 上传由 `oss_upload_worker` 异步完成。继续检查：

```text
oss_upload_worker 是否正在运行
RabbitMQ 容器是否 ready
RABBITMQ_URI / exchange / queue / routing key 是否一致
CLOUDDISK_TEMP_DIR 中临时文件是否存在
OSS 环境变量是否正确
worker 日志是否有 PutObject 失败
```

### 4. worker 报 temp file missing or unreadable

说明 RabbitMQ 消息中的 `tempPath` 在 worker 进程中读不到。

常见原因：

```text
API Gateway 和 worker 不在同一台机器
CLOUDDISK_TEMP_DIR 没有共享挂载
临时文件被手动删除
文件目录权限不足
```

当前第四期代码要求网关和 worker 能访问同一个本地临时目录。

### 5. 下载返回“文件不存在”

分两种情况：

```text
FileMetaService 返回 404：
  tbl_file 中没有这个 file_id，或 uid 不匹配。

OSS 返回 NoSuchKey：
  tbl_file 有元数据，但 OSS 中没有 users/{uid}/{hashcode} 对象。
  可能是上传 worker 还没完成，或上传失败。
```

### 6. RabbitMQ 容器不存在

`run.sh` 默认查找容器名：

```text
rabbit
```

如果你的容器名不同，在 `.env` 中设置：

```text
RABBITMQ_CONTAINER=你的容器名
```

如果 RabbitMQ 没有创建，需要先手动创建并暴露 5672 端口。

---

## 十三、当前代码的教学取舍

第四期重点是微服务拆分、Protobuf IDL、srpc 调用和 Workflow 异步串联，所以代码保留了一些课程项目简化：

```text
1. MySQL URL 写在 ServiceCommon.cc 中。
2. SQL 仍然使用字符串拼接，只做了简单 escape_sql。
3. srpc 服务端内部用 WaitGroup 等待 MySQL 任务完成，便于按顺序流程理解。
4. API Gateway 和 OssUploadWorker 通过本地 tempPath 交接文件。
5. tbl_file 没有上传状态字段，无法区分 pending / ready / failed。
6. 没有服务注册发现，网关通过 host/port 环境变量连接微服务。
```

如果继续演进，可以改成：

```text
1. 数据库配置环境变量化。
2. SQL 改成参数化查询。
3. tbl_file 增加 status 字段。
4. 文件上传改为共享存储、OSS 临时对象或浏览器直传 OSS。
5. 引入服务注册发现，避免网关硬编码服务地址。
6. 给 worker 增加失败重试次数和死信队列。
```

---

## 十四、推荐阅读顺序

第一次阅读第四期代码，建议按这个顺序：

```text
1. proto/cloud_disk.proto
   先看服务边界和 RPC 请求/响应。

2. src/services/AuthServiceMain.cc
   看注册、登录、Token 校验如何从网关拆出。

3. src/services/UserServiceMain.cc
   看用户资料服务如何按 user_id 查询 tbl_user。

4. src/services/FileMetaServiceMain.cc
   看 tbl_file 元数据如何独立成服务。

5. src/gateway/CloudDiskServer.h
   看网关组合了哪些 srpc client。

6. src/gateway/CloudDiskServer.cc
   看 HTTP 路由如何创建 srpc task，并通过 resp->add_task() 串联异步流程。

7. src/common/RabbitMqOssUploader.cc
   看网关发布任务和 worker 消费任务如何复用同一个类。

8. src/worker/OssUploadWorkerMain.cc
   看后台 OSS 上传进程的启动和停止。

9. CMakeLists.txt / build.sh / run.sh
   看生成、编译和多进程启动方式。
```
