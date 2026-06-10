# CloudDisk 第二期：接入阿里云 OSS

本文档说明当前 `CloudDisk` 项目第二期的实现。第二期的核心变化是：用户上传的文件内容不再保存到服务器本地磁盘，而是保存到阿里云对象存储 OSS；MySQL 仍然保存用户、文件名、文件大小、文件 hash 等元数据。

## 一、项目当前职责

当前项目是一个基于 `wfrest` 的 Web 网盘服务，主要包含三层：

1. 前端页面
   - 路径：`www/index.html`、`www/static/*`
   - 负责登录、注册、文件列表、上传、下载等浏览器交互。

2. 后端 HTTP 服务
   - 核心文件：`CloudDiskServer.cc`、`CloudDiskServer.h`
   - 负责注册路由、校验登录态、读写 MySQL、调用 OSS SDK。

3. 存储服务
   - MySQL：保存用户信息和文件元数据。
   - 阿里云 OSS：保存真实文件内容。

第一期中，文件内容保存到类似 `./storage/{uid}/{hashcode}` 的本地路径。第二期中，这部分已经替换为 OSS Object：

```text
users/{uid}/{hashcode}
```

例如用户 `uid = 3` 上传了一个 hash 为 `abc123...` 的文件，那么真实文件内容会保存为：

```text
OSS Bucket: ubuntu-cloud-disk-oss
ObjectName: users/3/abc123...
```

数据库仍然保存原始文件名 `filename`，所以用户下载时看到的文件名不会变。

## 二、第二期改了哪些代码

主要改动集中在以下文件：

```text
CloudDiskServer.cc
CloudDiskServer.h
CMakeLists.txt
```

### 1. `CloudDiskServer.cc`

新增了 OSS 相关逻辑：

- OSS 配置：endpoint、region、bucket、AccessKey。
- `getEnvOrThrow()`：从进程环境变量中读取 OSS 配置，缺少任意必需配置时直接抛异常。
- `create_oss_client()`：创建 OSS SDK 的 `OssClient` 对象。
- `oss_object_name()`：生成 `users/{uid}/{hashcode}` 形式的 ObjectName。
- `oss_upload_object()`：把上传文件内容写入 OSS。
- `oss_download_object()`：从 OSS 读取文件内容。
- `CloudDiskServer::CloudDiskServer()`：启动时初始化 OSS SDK。
- `CloudDiskServer::~CloudDiskServer()`：退出时释放 OSS SDK 全局资源。

### 2. `CloudDiskServer.h`

原来类里只有一个空的默认构造函数：

```cpp
CloudDiskServer() { }
```

现在改成：

```cpp
CloudDiskServer();
~CloudDiskServer();
```

因为第二期需要在构造函数中调用 `InitializeSdk()`，在析构函数中调用 `ShutdownSdk()`。

### 3. `CMakeLists.txt`

OSS C++ SDK 需要额外编译和链接配置：

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

`-fno-rtti` 是 OSS C++ SDK 文档要求的编译选项。`alibabacloud-oss-cpp-sdk` 是 OSS SDK 本身，`curl`、`crypto`、`pthread` 是它依赖的库。

## 三、几个容易混淆的“客户端”

这里最容易混淆的是“客户端”这个词。当前项目里至少有三种不同含义：

### 1. 浏览器客户端

这是用户打开网页时运行的前端代码。

例如：

```text
浏览器 -> POST /api/v1/files -> CloudDisk 后端
```

它不是 C++ 里的 `OssClient`，也不是 `CloudDiskServer`。

### 2. `CloudDiskServer server`

这是后端服务器对象，在 `main.cc` 中创建：

```cpp
CloudDiskServer server;
server.register_routes();
server.start(8888);
```

这个对象代表当前 Web 服务。它内部组合了一个 `wfrest::HttpServer server_`，负责监听端口、注册路由、处理 HTTP 请求。

它的生命周期大致是：

```text
程序启动
  -> 创建 CloudDiskServer server
  -> CloudDiskServer 构造函数执行
  -> 初始化 OSS SDK 全局资源
  -> 注册路由
  -> start(8888) 开始监听
  -> 用户不断访问网页、上传、下载
  -> Ctrl+C 停止服务
  -> server.stop()
  -> main() 结束
  -> CloudDiskServer 析构函数执行
  -> 释放 OSS SDK 全局资源
程序退出
```

所以 `CloudDiskServer` 不是每个用户都会创建一个，也不是每次上传都会创建一个。正常情况下，整个服务进程里只有一个。

### 3. OSS SDK 的 `OssClient`

`OssClient` 是阿里云 OSS C++ SDK 提供的对象，用来向 OSS 发请求。

当前代码中，每次上传或下载时会调用：

```cpp
auto client = create_oss_client();
```

这会创建一个 OSS SDK 的客户端对象，然后用它执行：

```cpp
client->PutObject(...); // 上传
client->GetObject(...); // 下载
```

请求结束后，局部变量 `client` 离开作用域，`unique_ptr` 会自动销毁这个 `OssClient` 对象。

这里说的“轻量客户端对象”，不是浏览器客户端，也不是 Web 服务器大对象，而是 OSS SDK 的请求操作对象。它主要保存 endpoint、AccessKey、region、连接配置等信息，然后发起一次 OSS 请求。

## 四、为什么要加 `~CloudDiskServer()`

阿里云 OSS C++ SDK 有两个全局生命周期函数：

```cpp
InitializeSdk();
ShutdownSdk();
```

PDF 第 15-16 页也强调了：

```text
InitializeSdk() 和 ShutdownSdk() 应该在整个程序生命周期中各只执行一次。
```

所以不能在每次上传、每次下载时这样写：

```cpp
InitializeSdk();
PutObject(...);
ShutdownSdk();
```

这样做的问题是：

- 每个请求都初始化/释放 SDK，成本不必要。
- 多个用户并发上传下载时，某个请求可能把 SDK 释放掉，影响另一个请求。
- SDK 全局资源生命周期会变得混乱。

现在的写法是：

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

含义是：

- `CloudDiskServer` 创建时，初始化 OSS SDK。
- `CloudDiskServer` 销毁时，释放 OSS SDK。
- 因为 `main.cc` 中 `CloudDiskServer server;` 只创建一次，所以 SDK 也只初始化一次、释放一次。

这是一种典型的 RAII 写法：对象创建时申请资源，对象销毁时释放资源。

## 五、为什么上传/下载时还要创建 `OssClient`

需要区分两类资源：

1. OSS SDK 全局资源
   - 由 `InitializeSdk()` 创建。
   - 由 `ShutdownSdk()` 释放。
   - 整个程序生命周期只做一次。

2. OSS 请求客户端对象 `OssClient`
   - 由 `create_oss_client()` 创建。
   - 上传或下载时临时使用。
   - 请求结束后自动销毁。

也就是说，启动服务器时不会创建一个“很大的 OSS 客户端对象”来一直服务所有请求。启动时只是初始化 OSS SDK 的全局环境。真正执行上传/下载时，才创建一个 `OssClient` 对象去发请求。

当前这样设计的原因是：

- 代码简单，容易理解。
- 不需要处理全局静态对象的析构顺序问题。
- 不需要担心 `OssClient` 是否可以被多个线程安全共享。
- 当前教学项目请求量较小，每次创建一个 `OssClient` 的成本可以接受。

如果是高并发生产项目，可以进一步优化成：

```text
CloudDiskServer 内部持有一个 OssClient 成员
或
封装一个 OssStorage 类统一管理 OssClient
```

但那样需要更仔细地确认 SDK 客户端的线程安全、生命周期、错误恢复策略。当前阶段先选择更直观、更稳妥的写法。

## 六、为什么不做成全局静态 `OssClient`

例如不要写成这样：

```cpp
static oss::OssClient client(...);
```

原因是全局静态对象的初始化和析构顺序不容易控制。

`OssClient` 必须在 `InitializeSdk()` 之后使用，并且最好在 `ShutdownSdk()` 之前析构。如果写成全局静态对象，可能出现：

```text
程序退出
  -> ShutdownSdk() 先执行
  -> 全局静态 OssClient 后析构
```

这样 `OssClient` 析构时 SDK 全局资源已经释放了，存在隐藏风险。

当前代码避免了这个问题：

```cpp
static unique_ptr<oss::OssClient> create_oss_client()
{
    ...
    return make_unique<oss::OssClient>(...);
}
```

`OssClient` 是函数里的局部对象，请求结束就销毁；而 OSS SDK 全局资源在整个服务器退出时才释放。

## 七、上传流程

接口：

```text
POST /api/v1/files
```

当前上传流程：

```text
1. 校验 Authorization: Bearer token
2. 检查 Content-Type 是否为 multipart/form-data
3. 从表单字段 file 中取出 filename 和 content
4. 使用文件内容生成 hashcode
5. 调用 oss_upload_object(uid, hashcode, content)
6. OSS 保存对象 users/{uid}/{hashcode}
7. MySQL 写入 tbl_file 元数据
8. 返回上传成功 JSON
```

关键点：

- `filename` 是用户看到的原始文件名。
- `content` 是文件真实内容。
- `hashcode` 是根据文件内容算出来的后端存储名。
- OSS ObjectName 不使用原始文件名，而使用 `users/{uid}/{hashcode}`。

这样做的好处：

- 不同用户的文件按 `uid` 隔离。
- 文件名中即使有空格、中文或特殊字符，也不会影响 OSS 对象定位。
- 下载时仍然可以用数据库里的 `filename` 还原原始文件名。

## 八、下载流程

接口：

```text
GET /api/v1/file/{id}
```

当前下载流程：

```text
1. 校验 Authorization: Bearer token
2. 根据 fileId 和当前 uid 查询 tbl_file
3. 如果查不到，返回 404 文件不存在
4. 查到 filename 和 hashcode
5. 调用 oss_download_object(uid, hashcode, content)
6. OSS 读取对象 users/{uid}/{hashcode}
7. 设置 Content-Disposition，让浏览器按原始文件名下载
8. 返回文件内容
```

SQL 查询中带了当前登录用户的 `uid`：

```sql
WHERE id = file_id AND uid = 当前登录用户id
```

这可以防止用户通过猜测别人的 `fileId` 下载不属于自己的文件。

## 九、MySQL 和 OSS 分别保存什么

MySQL 保存的是元数据：

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

OSS 保存的是文件内容：

```text
users/{uid}/{hashcode} -> 文件二进制内容
```

所以下载时必须两步都成功：

1. MySQL 查到这条文件记录。
2. OSS 查到对应 Object。

如果 MySQL 有记录但 OSS 没有对象，当前接口会返回：

```text
404 文件不存在
```

## 十、当前实现的一个细节：先上传 OSS，再写 MySQL

当前上传顺序是：

```text
1. PutObject 上传 OSS
2. INSERT tbl_file 写 MySQL
```

这样做的原因是：只有文件内容真正上传成功了，才写数据库记录。

但这也有一个边界情况：

```text
OSS 上传成功
MySQL 写入失败
```

这时 OSS 中会留下一个没有数据库记录的对象，也就是“孤儿对象”。当前教学项目先不处理这个问题。生产项目通常会补一层补偿逻辑：

```text
如果 MySQL INSERT 失败，则 DeleteObject 删除刚上传的 OSS 对象
```

或者通过定时任务清理数据库中不存在的 OSS 对象。

## 十一、配置方式

当前版本已经不再把 OSS AccessKey、Bucket、Endpoint 等敏感配置写死在代码里，而是通过 `.env` 文件集中配置。

`.env` 文件位于项目根目录：

```text
课件/05_Web网盘项目/CloudDisk/.env
```

内容格式如下：

```text
ALIBABA_CLOUD_ACCESS_KEY_ID=你的 AccessKey ID
ALIBABA_CLOUD_ACCESS_KEY_SECRET=你的 AccessKey Secret
ALIBABA_CLOUD_OSS_BUCKET=ubuntu-cloud-disk-oss
ALIBABA_CLOUD_OSS_ENDPOINT=oss-cn-wuhan-lr.aliyuncs.com
ALIBABA_CLOUD_OSS_REGION=cn-wuhan
```

`CloudDiskServer.cc` 中通过 `getEnvOrThrow()` 读取这些配置：

```cpp
static const string OssEndpoint = getEnvOrThrow("ALIBABA_CLOUD_OSS_ENDPOINT");
static const string OssAccessKeyId = getEnvOrThrow("ALIBABA_CLOUD_ACCESS_KEY_ID");
static const string OssAccessKeySecret = getEnvOrThrow("ALIBABA_CLOUD_ACCESS_KEY_SECRET");
static const string OssBucketName = getEnvOrThrow("ALIBABA_CLOUD_OSS_BUCKET");
static const string OssRegion = getEnvOrThrow("ALIBABA_CLOUD_OSS_REGION");
```

注意一个关键细节：`getEnvOrThrow()` 内部调用的是 `getenv()`，它读取的是当前 `server` 进程的环境变量；C++ 程序本身不会自动读取 `.env` 文件。

本项目通过 `run.sh` 解决这个问题。`run.sh` 会先执行：

```bash
set -a
source .env
set +a
```

这三行的含义是：把 `.env` 中的变量加载并导出为环境变量，然后再启动 `./server`。因此推荐使用：

```bash
./run.sh
```

而不是直接执行：

```bash
./server
```

如果直接运行 `./server`，并且当前 shell 没有提前加载 `.env`，`getEnvOrThrow()` 会因为找不到配置而抛出类似这样的异常：

```text
Missing environment variable: ALIBABA_CLOUD_ACCESS_KEY_ID
```

AccessKey 属于敏感信息，不适合长期写在源码中。当前 `.env` 方案比写死在 `.cc` 文件中更合理；后续如果提交到远程仓库，应考虑把 `.env` 加入 `.gitignore`，只提交 `.env.example` 模板。

## 十二、编译和运行

编译：

```bash
cd 课件/05_Web网盘项目/CloudDisk
cmake --build build
```

如果重新生成构建目录：

```bash
cd 课件/05_Web网盘项目/CloudDisk
rm -rf build
mkdir build
cd build
cmake ..
make
```

运行：

```bash
cd 课件/05_Web网盘项目/CloudDisk
./run.sh
```

服务默认监听：

```text
http://localhost:8888
```

## 十三、常见问题

### 1. 为什么下载时不再用 `resp->File(real_path)`

第一期文件在服务器本地磁盘上，所以可以直接：

```cpp
resp->File(real_path);
```

第二期文件在 OSS 上，服务器本地没有这个文件路径，所以要先：

```cpp
GetObject(...)
```

把 OSS 对象内容读回来，再：

```cpp
resp->String(move(content));
```

返回给浏览器。

### 2. OSS 的 `users/3/xxx` 是真实目录吗

不是。OSS 是对象存储，不是传统文件系统。

`users/3/xxx` 只是一个 ObjectName 字符串。OSS 控制台里看起来像目录，是因为控制台把 `/` 当作分隔符展示了。

### 3. 为什么数据库还要保存 `filename`

因为 `hashcode` 是后端存储用的名字，不适合展示给用户。

用户上传的是：

```text
毕业设计.docx
```

OSS 存的是：

```text
users/3/a948904f2f0f479...
```

下载时需要用数据库里的 `filename` 设置响应头：

```http
Content-Disposition: attachment; filename="毕业设计.docx"
```

浏览器才会按用户原始文件名下载。

### 4. 当前代码是不是每次请求都会连接 OSS

上传和下载请求会创建一个 `OssClient` 对象，然后发起一次 OSS 请求。

但这不等于每次都初始化 SDK。SDK 初始化只在 `CloudDiskServer` 构造函数里做一次。

可以理解为：

```text
InitializeSdk(): 打开 OSS SDK 的全局环境，只做一次
OssClient: 本次 OSS 操作使用的请求对象，上传/下载时临时创建
PutObject/GetObject: 真正访问 OSS 的网络请求
ShutdownSdk(): 关闭 OSS SDK 的全局环境，只做一次
```

### 5. 如果以后要做删除文件怎么办

删除接口应该同时处理两件事：

```text
1. 删除 MySQL 中的 tbl_file 记录，或设置 tomb
2. 删除 OSS 中的 users/{uid}/{hashcode}
```

如果多个数据库记录可能指向同一个 OSS 对象，还需要先确认是否还有其它记录引用该对象，再决定能不能删除 OSS 对象。

当前第二期只改造了上传和下载，未新增删除接口。

## 十四、代码细节问答

### 1. `.env`、`getEnvOrThrow()` 和 `getenv()` 是什么关系

当前代码中读取 OSS 配置的是 `getEnvOrThrow()`：

```cpp
static string getEnvOrThrow(const char* name) {
    const char* value = getenv(name);
    if (value == nullptr || string(value).empty()) {
        throw runtime_error(string("Missing environment variable: ") + name);
    }
    return string(value);
}
```

它的职责有两个：

- 调用 `getenv(name)` 读取当前进程环境变量。
- 如果变量不存在或为空，直接抛出异常，让程序启动失败。

这里容易误解：`getenv()` 不是读取 `.env` 文件，它只读取“已经进入当前进程环境”的变量。

`.env` 文件只是一个普通文本文件：

```text
ALIBABA_CLOUD_ACCESS_KEY_ID=...
ALIBABA_CLOUD_ACCESS_KEY_SECRET=...
ALIBABA_CLOUD_OSS_BUCKET=ubuntu-cloud-disk-oss
ALIBABA_CLOUD_OSS_ENDPOINT=oss-cn-wuhan-lr.aliyuncs.com
ALIBABA_CLOUD_OSS_REGION=cn-wuhan
```

要让 `getenv()` 读到 `.env` 中的变量，需要先把 `.env` 加载到 shell 环境里。本项目的 `run.sh` 已经做了这一步：

```bash
set -a
source .env
set +a
./server
```

所以正确启动方式是：

```bash
./run.sh
```

如果你手动启动，也可以这样：

```bash
set -a
source .env
set +a
./server
```

如果只是直接执行 `./server`，而当前 shell 没有这些环境变量，程序会在读取 OSS 配置时抛异常并退出。

### 2. `static unique_ptr<oss::OssClient>` 是什么意思

代码是：

```cpp
static unique_ptr<oss::OssClient> create_oss_client()
```

这里要拆开看：

```text
static
unique_ptr<oss::OssClient>
create_oss_client()
```

`static` 修饰的是函数，意思是这个函数只在当前 `CloudDiskServer.cc` 文件内部可见，别的 `.cc` 文件不能直接调用它。

`unique_ptr<oss::OssClient>` 才是返回值类型，表示函数返回一个由智能指针管理的 `OssClient` 对象。

所以这不是“返回一个 static 的 unique_ptr”，也不是“全局只创建一个 OssClient”。每次调用 `create_oss_client()` 都会创建一个新的 `OssClient`。

### 3. 能不能直接返回 `oss::OssClient`

理论上可以写成类似：

```cpp
static oss::OssClient create_oss_client()
{
    oss::ClientConfiguration conf;
    oss::OssClient client(endpoint, accessKeyId, accessKeySecret, conf);
    client.SetRegion(region);
    return client;
}
```

但当前项目没有这样写，主要是为了避免几个问题：

- 返回对象本身可能涉及拷贝或移动。
- `OssClient` 是 SDK 类型，内部持有实现对象和连接配置，直接值返回不如指针所有权清晰。
- `unique_ptr` 明确表达“这个客户端对象只属于当前调用者，用完自动销毁”。

PDF 示例：

```cpp
OssClient client(endpoint, accessKeyId, accessKeySecret, conf);
```

是在 `main()` 函数中直接创建局部变量，然后马上使用。这种写法完全没问题。

当前项目多封装了一层函数：

```cpp
auto client = create_oss_client();
```

因为上传和下载都需要创建客户端，封装成函数可以避免重复写 endpoint、AccessKey、region 这些配置代码。

### 4. 为什么用 `make_unique<oss::OssClient>`

代码：

```cpp
auto client = make_unique<oss::OssClient>(
    endpoint,
    accessKeyId,
    accessKeySecret,
    conf);
```

可以理解为安全版的：

```cpp
oss::OssClient* client = new oss::OssClient(endpoint, accessKeyId, accessKeySecret, conf);
```

区别是 `make_unique` 返回的是 `unique_ptr`，不需要手动 `delete`。

当 `client` 离开作用域时：

```cpp
auto client = create_oss_client();
...
// 函数结束
```

`unique_ptr` 会自动释放它管理的 `OssClient`。

所以：

```cpp
make_unique<T>(参数...)
```

含义就是：

```text
创建一个 T 对象，并把它交给 unique_ptr 管理。
```

### 5. `make_shared<stringstream>(ios::in | ios::out | ios::binary)` 在做什么

OSS SDK 上传内存内容时，接口需要的是：

```cpp
shared_ptr<iostream>
```

但当前上传接口拿到的是：

```cpp
string content;
```

也就是文件内容已经在内存字符串里。

所以代码创建了一个 `stringstream`：

```cpp
auto stream = make_shared<stringstream>(ios::in | ios::out | ios::binary);
```

`stringstream` 可以把内存当成“流”使用，类似一个内存文件：

- `ios::out`：允许向流里写入内容。
- `ios::in`：允许后续从流里读取内容。
- `ios::binary`：按二进制方式处理内容。

然后把文件内容写进去：

```cpp
stream->write(content.data(), content.size());
```

最后把这个流交给 OSS：

```cpp
oss::PutObjectRequest request(bucket_name, object_name, stream);
```

### 6. `stream->seekg(0)` 是什么意义

`stringstream` 内部有两个位置：

```text
写位置 put pointer
读位置 get pointer
```

执行：

```cpp
stream->write(content.data(), content.size());
```

之后，写位置已经到了末尾。为了让 OSS SDK 从头读取这个流，需要把读位置移动到开头：

```cpp
stream->seekg(0);
```

`seekg` 的 `g` 是 get，表示设置读取位置。

如果不调用 `seekg(0)`，SDK 读取这个流时可能从错误位置开始，导致上传内容为空或不完整。

### 7. `oss_content << stream->rdbuf()` 在做什么

下载时 OSS SDK 返回的是一个内容流：

```cpp
auto stream = outcome.result().Content();
```

这个 `stream` 可以理解为“从 OSS 下载回来的文件流”。

代码：

```cpp
ostringstream oss_content;
oss_content << stream->rdbuf();
content = oss_content.str();
```

含义是：

```text
把 OSS 返回流中的所有内容，复制到 oss_content 这个内存输出流中，
然后用 oss_content.str() 得到完整字符串。
```

`rdbuf()` 返回的是流背后的缓冲区。把一个流的缓冲区插入到另一个流中，是 C++ 中常见的“整流复制”写法。

### 8. 970-972 行的 header 是 OSS 要求的吗

不是。

这两个 header 是后端返回给浏览器的 HTTP 响应头，和 OSS 没有直接关系：

```cpp
resp->add_header("Content-Type", "application/octet-stream");
resp->add_header("Content-Disposition",
                 "attachment; filename=\"...\"");
```

`Content-Type: application/octet-stream` 表示响应体是普通二进制文件。

`Content-Disposition: attachment; filename="..."` 表示浏览器应该把响应体当附件下载，并使用指定文件名。

它们的作用对象是浏览器：

```text
CloudDisk 后端 -> 浏览器
```

不是：

```text
CloudDisk 后端 -> OSS
```

### 9. `resp->String(move(content))` 在做什么

`resp->String(...)` 是 wfrest 提供的接口，用来设置 HTTP 响应体。

第一期本地文件下载时可以这样：

```cpp
resp->File(real_path);
```

因为文件就在服务器磁盘上。

第二期中，文件内容来自 OSS，已经被读到了内存字符串：

```cpp
string content;
```

所以要把这个字符串作为响应体返回：

```cpp
resp->String(move(content));
```

`move(content)` 的作用是把 `content` 内部管理的那块内存尽量转移给响应对象，避免再复制一份文件内容。

执行后不要再使用 `content`，因为它已经被移动了。当前代码中 `resp->String(move(content));` 后面没有再访问 `content`，所以是正确的。
