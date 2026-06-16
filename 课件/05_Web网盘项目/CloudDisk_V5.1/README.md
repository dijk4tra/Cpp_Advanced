# CloudDisk 第五期技术文档：基于 Consul 的服务注册与发现

本文档说明当前 `CloudDisk` 第五期代码的后端实现。

第五期是在第四期 srpc 微服务架构基础上，引入 Consul 注册中心。API Gateway 不再保存后端服务的固定地址，而是在每次调用 RPC 前按服务名从 Consul 查询健康实例。

当前版本要求 Consul 必须可用：

```text
1. 微服务启动后必须成功注册到 Consul。
2. API Gateway 必须从 Consul 查询到 passing 状态实例。
3. API Gateway 只通过 Consul 获取后端服务地址。
```

前端页面和 HTTP API 地址保持不变。浏览器仍然访问：

```text
http://127.0.0.1:8888
```

如果在 Windows 浏览器访问 Linux 虚拟机或服务器上的项目，则访问：

```text
http://<Linux服务器IP>:8888
```

---

## 一、第五期解决什么问题

第四期已经把单体服务拆成：

```text
API Gateway
AuthService
UserService
FileMetaService
OssUploadWorker
```

但第四期的 API Gateway 仍然通过固定 host/port 调用后端微服务。这样会有几个问题：

```text
1. 服务地址变化时，网关也要改配置。
2. 后端服务实例宕机后，网关不会自动切换。
3. 同一个服务即使启动多个实例，网关也无法按健康实例分配请求。
4. 网关和后端服务之间仍然没有彻底解耦。
```

第五期通过 Consul 解决这个问题：

```text
1. 服务提供者启动后注册到 Consul。
2. 服务提供者周期性发送 TTL 心跳。
3. Consul 根据心跳维护实例健康状态。
4. API Gateway 从 Consul 查询 passing 状态实例。
5. API Gateway 使用发现到的 host/port 创建 srpc client 并发送 RPC。
```

---

## 二、整体架构

```text
                         Consul :8500
                      service registry
                    /       |        \
                   /        |         \
                  v         v          v
AuthService     UserService FileMetaService
register + TTL  register    register
  :9001          :9002       :9003
  :9011          :9012       :9013

Browser
  |
  | HTTP
  v
API Gateway :8888
  |
  | 1. discover healthy instance from Consul
  | 2. create srpc client by discovered host/port
  v
AuthService / UserService / FileMetaService

API Gateway
  |
  | publish upload task
  v
RabbitMQ -> OssUploadWorker -> Aliyun OSS
```

进程说明：

```text
bin/server             API Gateway，监听 8888
bin/auth_service       认证微服务，默认监听 9001
bin/user_service       用户资料微服务，默认监听 9002
bin/filemeta_service   文件元数据微服务，默认监听 9003
bin/oss_upload_worker  OSS 上传后台 worker，不监听端口
```

Consul 中注册的服务名：

```text
AuthService
UserService
FileMetaService
```

`OssUploadWorker` 不注册到 Consul，因为它不提供 RPC 服务，只消费 RabbitMQ 队列。

---

## 三、目录结构

```text
CloudDisk/
├── CMakeLists.txt
├── build.sh
├── run.sh
├── .env
├── README.md
├── docs/
├── proto/
│   └── cloud_disk.proto
├── rpc_gen/
├── src/
│   ├── common/
│   │   ├── CryptoUtil.*
│   │   ├── OssStorage.*
│   │   ├── RabbitMqOssUploader.*
│   │   ├── ServiceCommon.*
│   │   └── ServiceRegistry.*
│   ├── gateway/
│   │   ├── CloudDiskServer.*
│   │   └── main.cc
│   ├── services/
│   │   ├── AuthServiceMain.cc
│   │   ├── UserServiceMain.cc
│   │   └── FileMetaServiceMain.cc
│   └── worker/
│       └── OssUploadWorkerMain.cc
└── www/
```

第五期新增的核心文件：

```text
src/common/ServiceRegistry.h
src/common/ServiceRegistry.cc
```

它提供两个能力：

```text
ServiceRegistrar  服务注册、TTL 心跳、服务注销
ServiceDiscovery  健康实例查询、简单轮询选择
```

---

## 四、模块职责

### 1. API Gateway

代码位置：

```text
src/gateway/main.cc
src/gateway/CloudDiskServer.h
src/gateway/CloudDiskServer.cc
```

职责：

```text
1. 提供 HTTP 服务，监听 8888。
2. 提供静态资源访问。
3. 解析 JSON、multipart/form-data、Authorization header。
4. 把 HTTP 请求转换成后端 srpc 请求。
5. 调用 ServiceDiscovery 按服务名发现后端实例。
6. 使用发现到的 host/port 创建 srpc client。
7. 使用 resp->add_task(task) 把 srpc task 接入 wfrest 当前 HTTP 序列。
8. 上传文件时保存临时文件，并发布 RabbitMQ 上传任务。
9. 下载文件时查询文件元数据，再从 OSS 下载文件内容。
```

API Gateway 不负责：

```text
1. 注册、登录 SQL。
2. 密码 hash 和 JWT 生成。
3. 用户资料 SQL。
4. 文件元数据 SQL。
5. RabbitMQ 后台消费。
6. OSS 异步上传。
```

### 2. AuthService

代码位置：

```text
src/services/AuthServiceMain.cc
```

默认端口：

```text
9001
```

可通过环境变量修改：

```text
AUTH_SERVICE_PORT
```

职责：

```text
1. Register：注册用户。
2. Login：登录用户。
3. VerifyToken：校验 JWT。
4. 生成 salt。
5. 计算密码 hash。
6. 生成 JWT。
7. 访问 tbl_user。
8. 启动成功后注册 AuthService 到 Consul。
9. 定时发送 TTL 心跳。
10. 退出时注销 Consul 实例。
```

### 3. UserService

代码位置：

```text
src/services/UserServiceMain.cc
```

默认端口：

```text
9002
```

可通过环境变量修改：

```text
USER_SERVICE_PORT
```

职责：

```text
1. GetUserProfile：按 user_id 查询用户资料。
2. 访问 tbl_user。
3. 启动成功后注册 UserService 到 Consul。
4. 定时发送 TTL 心跳。
5. 退出时注销 Consul 实例。
```

### 4. FileMetaService

代码位置：

```text
src/services/FileMetaServiceMain.cc
```

默认端口：

```text
9003
```

可通过环境变量修改：

```text
FILEMETA_SERVICE_PORT
```

职责：

```text
1. ListFiles：查询用户文件列表。
2. CreateFile：创建文件元数据。
3. GetFileForDownload：下载前查询 filename/hashcode。
4. 访问 tbl_file。
5. 启动成功后注册 FileMetaService 到 Consul。
6. 定时发送 TTL 心跳。
7. 退出时注销 Consul 实例。
```

### 5. OssUploadWorker

代码位置：

```text
src/worker/OssUploadWorkerMain.cc
```

职责：

```text
1. 创建 OssStorage。
2. 创建 RabbitMqOssUploader。
3. 消费 RabbitMQ 队列 oss.queue。
4. 读取消息中的 uid/hashcode/tempPath。
5. 从 tempPath 读取本地临时文件。
6. 上传文件内容到 OSS。
7. 上传成功后 ack 消息并删除临时文件。
8. 上传失败时 reject 并重新入队。
```

它不是 srpc 服务，不监听端口，也不注册到 Consul。

---

## 五、注册中心实现

### 1. ServiceEndpoint

`ServiceEndpoint` 表示一个可连接的服务实例地址：

```cpp
struct ServiceEndpoint {
    std::string host;
    unsigned short port;
};
```

API Gateway 最终只需要这两个值来创建 srpc client。

### 2. ServiceRegistrar

`ServiceRegistrar` 给三个 srpc 服务使用。

启动流程：

```text
1. srpc server 先 start(port)。
2. 创建 ServiceRegistrar(service_name, host, port)。
3. 调用 registrar.start()。
4. start() 调用 Consul agent.registerService()。
5. 注册成功后启动心跳线程。
6. 心跳线程定期调用 agent.servicePass(service_id)。
7. 进程退出时调用 registrar.stop()。
8. stop() 停止心跳，并调用 deregisterService(service_id)。
```

实例 ID 格式：

```text
{service_name}-{host}-{port}
```

示例：

```text
AuthService-127.0.0.1-9001
UserService-127.0.0.1-9002
FileMetaService-127.0.0.1-9003
```

健康检查：

```text
TTL:       CONSUL_TTL_SECONDS，默认 10 秒
Heartbeat: CONSUL_HEARTBEAT_SECONDS，默认 5 秒
```

如果服务超过 TTL 没有心跳，Consul 会把实例标记为 `critical`。API Gateway 查询 `passing=true` 时不会拿到这个实例。

### 3. ServiceDiscovery

`ServiceDiscovery` 给 API Gateway 使用。

发现流程：

```text
1. API Gateway 准备调用某个 RPC。
2. 按服务名调用 discovery_.select("AuthService", endpoint)。
3. ServiceDiscovery 调用 Consul Health API。
4. 查询 passing=true 的健康实例。
5. 如果有多个实例，使用简单 round-robin。
6. 返回选中的 host/port。
7. API Gateway 用 host/port 创建 srpc client。
```

当前没有固定地址回退逻辑。

如果 Consul 查询失败，或者没有 passing 实例：

```text
ServiceDiscovery::select() 返回 false
API Gateway 返回 HTTP 500
```

---

## 六、HTTP API 与 RPC 调用链路

### 1. 注册

```text
Browser
  -> POST /api/v1/auth/register
  -> API Gateway 解析 JSON
  -> ServiceDiscovery 查询 AuthService
  -> AuthService.Register
  -> INSERT tbl_user
  -> API Gateway 返回 JSON
```

### 2. 登录

```text
Browser
  -> POST /api/v1/auth/login
  -> API Gateway 解析 JSON
  -> ServiceDiscovery 查询 AuthService
  -> AuthService.Login
  -> SELECT tbl_user
  -> 校验密码
  -> 生成 JWT
  -> API Gateway 返回 accessToken
```

### 3. 获取当前用户资料

```text
Browser
  -> GET /api/v1/user/me
  -> API Gateway 解析 Bearer Token
  -> ServiceDiscovery 查询 AuthService
  -> AuthService.VerifyToken
  -> ServiceDiscovery 查询 UserService
  -> UserService.GetUserProfile
  -> API Gateway 返回用户资料
```

### 4. 获取文件列表

```text
Browser
  -> GET /api/v1/files
  -> API Gateway 解析 Bearer Token
  -> ServiceDiscovery 查询 AuthService
  -> AuthService.VerifyToken
  -> ServiceDiscovery 查询 FileMetaService
  -> FileMetaService.ListFiles
  -> API Gateway 返回文件列表
```

### 5. 上传文件

```text
Browser
  -> POST /api/v1/files multipart/form-data
  -> API Gateway 解析 Bearer Token
  -> API Gateway 解析文件内容
  -> API Gateway 计算 hashcode
  -> ServiceDiscovery 查询 AuthService
  -> AuthService.VerifyToken
  -> API Gateway 保存本地临时文件
  -> ServiceDiscovery 查询 FileMetaService
  -> FileMetaService.CreateFile
  -> API Gateway 发布 RabbitMQ 上传任务
  -> API Gateway 返回上传成功

OssUploadWorker
  -> 消费 RabbitMQ 消息
  -> 读取 tempPath
  -> 上传 OSS
  -> ack 消息
  -> 删除临时文件
```

RabbitMQ 消息格式：

```json
{
  "uid": 1,
  "hashcode": "file-content-sha256",
  "tempPath": "./tmp/uploads/1-file-content-sha256-xxxx.tmp"
}
```

注意：

```text
当前上传链路仍使用本地 tempPath。
所以 API Gateway 和 OssUploadWorker 必须运行在同一台机器上，或者共享同一个临时目录。
```

### 6. 下载文件

```text
Browser
  -> GET /api/v1/file/{id}
  -> API Gateway 解析 Bearer Token
  -> ServiceDiscovery 查询 AuthService
  -> AuthService.VerifyToken
  -> ServiceDiscovery 查询 FileMetaService
  -> FileMetaService.GetFileForDownload
  -> API Gateway 从 OSS 下载文件内容
  -> API Gateway 返回文件字节流
```

---

## 七、Protobuf / srpc 接口

IDL 文件：

```text
proto/cloud_disk.proto
```

服务：

```protobuf
service AuthService {
  rpc Register(RegisterRequest) returns (RegisterResponse);
  rpc Login(LoginRequest) returns (LoginResponse);
  rpc VerifyToken(VerifyTokenRequest) returns (VerifyTokenResponse);
}

service UserService {
  rpc GetUserProfile(GetUserProfileRequest) returns (GetUserProfileResponse);
}

service FileMetaService {
  rpc ListFiles(ListFilesRequest) returns (ListFilesResponse);
  rpc CreateFile(CreateFileRequest) returns (CreateFileResponse);
  rpc GetFileForDownload(GetFileForDownloadRequest) returns (GetFileForDownloadResponse);
}
```

第五期没有修改业务 RPC 协议。注册中心属于进程治理能力，放在 `src/common/ServiceRegistry.*` 中实现。

---

## 八、配置项

`run.sh` 会加载当前目录下的 `.env`：

```bash
set -a
source .env
set +a
```

当前配置：

```text
OSS：
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

Consul：
  CONSUL_HTTP_ADDR
  CONSUL_DC
  CONSUL_CONTAINER
  CLOUDDISK_SERVICE_HOST
  CONSUL_TTL_SECONDS
  CONSUL_HEARTBEAT_SECONDS

临时文件：
  CLOUDDISK_TEMP_DIR
```

当前 `.env` 示例：

```env
CONSUL_HTTP_ADDR=http://127.0.0.1:8500
CONSUL_DC=dc1
CONSUL_CONTAINER=consul1
CLOUDDISK_SERVICE_HOST=127.0.0.1
CONSUL_TTL_SECONDS=10
CONSUL_HEARTBEAT_SECONDS=5
```

说明：

```text
CONSUL_HTTP_ADDR
  API Gateway 和微服务访问 Consul HTTP API 的地址。

CONSUL_DC
  Consul 数据中心，默认 dc1。

CONSUL_CONTAINER
  run.sh 检查和启动的 Consul Docker 容器名。

CLOUDDISK_SERVICE_HOST
  微服务注册到 Consul 时暴露给 API Gateway 的地址。
  单机运行时通常是 127.0.0.1。
  如果 API Gateway 和微服务不在同一台机器上，必须改成网关能访问到的内网 IP。

CONSUL_TTL_SECONDS
  TTL 健康检查超时时间。

CONSUL_HEARTBEAT_SECONDS
  服务发送心跳的间隔，必须小于 TTL。
```

---

## 九、构建

执行：

```bash
./build.sh
```

`build.sh` 会：

```text
1. 重新生成 protobuf C++ 代码。
2. 重新生成 srpc 代码。
3. 运行 cmake。
4. 编译所有目标。
5. 重新生成 run.sh。
```

生成目标：

```text
bin/server
bin/auth_service
bin/user_service
bin/filemeta_service
bin/oss_upload_worker
```

第五期新增链接库：

```text
ppconsul
```

如果构建时报找不到 `ppconsul`，检查：

```text
/usr/local/include/ppconsul 是否存在
/usr/local/lib/libppconsul.so 是否存在
ldconfig 是否已经执行
```

---

## 十、运行

### 1. 准备 Consul

当前项目推荐本地单节点 Consul。

如果还没有单节点 Consul 容器，可以创建：

```bash
docker run --name consul1 -d \
  -p 8500:8500 \
  -p 8301:8301 \
  -p 8302:8302 \
  -p 8600:8600 \
  hashicorp/consul agent \
  -server \
  -bootstrap-expect 1 \
  -ui \
  -bind=0.0.0.0 \
  -client=0.0.0.0
```

访问 Consul UI：

```text
http://127.0.0.1:8500/ui
```

注意：

```text
如果已有 consul1 是按 -bootstrap-expect 2 创建的三节点容器，
当前 run.sh 不会把它当作单节点注册中心使用。
需要重建单节点容器，或者手动启动完整三节点 Consul 集群。
```

### 2. 启动 CloudDisk

```bash
./run.sh
```

`run.sh` 启动顺序：

```text
1. 加载 .env。
2. 检查并启动 RabbitMQ 容器。
3. 等待 RabbitMQ ready。
4. 检查并启动 Consul 容器。
5. 等待 Consul HTTP API ready。
6. 后台启动 auth_service。
7. 后台启动 user_service。
8. 后台启动 filemeta_service。
9. 后台启动 oss_upload_worker。
10. 前台启动 API Gateway。
```

Consul 是必需依赖：

```text
1. Consul 容器不存在，run.sh 退出。
2. Consul 是旧三节点容器但只启动了一个节点，run.sh 退出。
3. Consul HTTP API 30 秒内未 ready，run.sh 退出。
4. 微服务注册 Consul 失败，对应微服务退出。
5. API Gateway 发现不到服务实例时，请求返回 500。
```

---

## 十一、验证

### 1. 检查 Consul

```bash
curl -i http://127.0.0.1:8500/v1/status/leader
```

预期：

```text
HTTP/1.1 200 OK
```

查询服务列表：

```bash
curl -s http://127.0.0.1:8500/v1/catalog/services
```

预期包含：

```text
AuthService
UserService
FileMetaService
```

查询健康实例：

```bash
curl -s "http://127.0.0.1:8500/v1/health/service/AuthService?passing=true"
curl -s "http://127.0.0.1:8500/v1/health/service/UserService?passing=true"
curl -s "http://127.0.0.1:8500/v1/health/service/FileMetaService?passing=true"
```

### 2. 检查 HTTP 首页

```bash
curl -i http://127.0.0.1:8888/
```

### 3. 注册

```bash
curl -i -X POST http://127.0.0.1:8888/api/v1/auth/register \
  -H "Content-Type: application/json" \
  -d '{"username":"readme_user","password":"123456","confirm":"123456"}'
```

### 4. 登录

```bash
curl -i -X POST http://127.0.0.1:8888/api/v1/auth/login \
  -H "Content-Type: application/json" \
  -d '{"username":"readme_user","password":"123456"}'
```

### 5. 错误请求验证

密码错误：

```bash
curl -i -X POST http://127.0.0.1:8888/api/v1/auth/login \
  -H "Content-Type: application/json" \
  -d '{"username":"readme_user","password":"wrong"}'
```

无 token：

```bash
curl -i http://127.0.0.1:8888/api/v1/user/me
```

错误 token：

```bash
curl -i http://127.0.0.1:8888/api/v1/user/me \
  -H "Authorization: Bearer invalid-token"
```

更完整的接口测试命令见：

```text
docs/第五期：接口测试文档.md
```

---

## 十二、常见问题排查

### 1. run.sh 提示 Consul container not found

原因：

```text
CONSUL_CONTAINER 指定的容器不存在。
```

处理：

```bash
docker ps -a --filter name=consul
```

如果没有 `consul1`，创建单节点 Consul 容器。

### 2. run.sh 提示 bootstrap-expect 2

原因：

```text
已有 consul1 是按三节点集群方式创建的。
它要求至少两个 server 节点参与启动，不能作为单节点注册中心使用。
```

处理方式一：删除旧容器并重建单节点 Consul。

```bash
docker stop consul1 consul2 consul3
docker rm consul1 consul2 consul3
```

然后重新创建 `-bootstrap-expect 1` 的单节点容器。

处理方式二：手动启动完整三节点 Consul 集群。

### 3. 微服务日志出现 register FAILED

原因：

```text
微服务启动后向 Consul 注册失败。
```

检查：

```text
1. CONSUL_HTTP_ADDR 是否正确。
2. curl http://127.0.0.1:8500/v1/status/leader 是否成功。
3. Consul 容器是否正常运行。
4. 网络是否能访问 8500 端口。
```

### 4. 网关返回 500，日志出现 discovery FAILED

原因：

```text
API Gateway 从 Consul 查询服务实例失败。
```

检查：

```bash
curl -s "http://127.0.0.1:8500/v1/health/service/AuthService?passing=true"
curl -s "http://127.0.0.1:8500/v1/health/service/UserService?passing=true"
curl -s "http://127.0.0.1:8500/v1/health/service/FileMetaService?passing=true"
```

如果返回空数组，说明对应微服务没有注册成功，或 TTL 心跳已经失效。

### 5. 注册或登录失败，日志出现 MySQL packet ERROR

检查：

```text
1. MySQL 是否启动。
2. CloudDisk 数据库是否存在。
3. tbl_user / tbl_file 表结构是否匹配。
4. src/common/ServiceCommon.cc 中 DatabaseURL 是否正确。
```

### 6. 上传成功但下载不到文件

检查：

```text
1. RabbitMQ 是否 ready。
2. oss_upload_worker 是否启动。
3. RabbitMQ consumer 是否打印消费日志。
4. OSS 配置是否正确。
5. 临时文件是否能被 oss_upload_worker 读取。
```

### 7. Windows 浏览器无法访问 Linux 服务

检查：

```text
1. Linux 服务器 IP 是否正确。
2. API Gateway 是否监听 8888。
3. 防火墙是否放行 8888。
4. Windows 是否能 ping 通 Linux 服务器。
```

---

## 十三、当前边界

当前第五期仍然保留这些学习项目简化点：

```text
1. MySQL URL 写在 ServiceCommon.cc 中。
2. SQL 仍然使用字符串拼接，未改成参数化 SQL。
3. 上传链路仍然使用本地 tempPath。
4. API Gateway 没有注册到 Consul。
5. OssUploadWorker 没有注册到 Consul。
6. ServiceDiscovery 没有做复杂缓存，只做简单轮询。
```

这些点可以作为后续阶段继续演进。
