# 设置 CMake 的最低版本。
cmake_minimum_required(VERSION 3.15)

# 项目名称。
project(CloudDisk)

# 使用 C++17。
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 生成 compile_commands.json。
# 很多编辑器/语言服务器会读取这个文件，从而正确识别 include 路径和编译选项。
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# 所有可执行程序都输出到 CloudDisk/bin 目录。
# 根目录只保留脚本、配置、源码目录和文档目录，避免构建产物混在源码中。
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ../bin)

# 生成的 protobuf 源文件。
# cloud_disk.srpc.h 是头文件，里面是 inline 实现，不需要放进 add_executable。
set(PROTO_SRC rpc_gen/cloud_disk.pb.cc)

# 让所有目标都能找到当前目录、公共源码目录、网关目录和 RPC 生成目录。
include_directories(${CMAKE_CURRENT_SOURCE_DIR})
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/src/common)
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/src/gateway)
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/rpc_gen)

# API Gateway。
# 这个目标继续叫 server，保持第三期 ./server 的启动习惯。
add_executable(server
    ${PROTO_SRC}
    src/common/CryptoUtil.cc
    src/common/OssStorage.cc
    src/common/RabbitMqOssUploader.cc
    src/gateway/CloudDiskServer.cc
    src/gateway/main.cc
)

# AuthService 认证微服务。
add_executable(auth_service
    ${PROTO_SRC}
    src/common/CryptoUtil.cc
    src/common/ServiceCommon.cc
    src/services/AuthServiceMain.cc
)

# UserService 用户资料微服务。
add_executable(user_service
    ${PROTO_SRC}
    src/common/ServiceCommon.cc
    src/services/UserServiceMain.cc
)

# FileMetaService 文件元数据微服务。
add_executable(filemeta_service
    ${PROTO_SRC}
    src/common/ServiceCommon.cc
    src/services/FileMetaServiceMain.cc
)

# OSS 上传后台 worker。
add_executable(oss_upload_worker
    src/common/OssStorage.cc
    src/common/RabbitMqOssUploader.cc
    src/worker/OssUploadWorkerMain.cc
)

# OSS C++ SDK 默认关闭 RTTI。
# 只要目标中编译了 OssStorage.cc，就按 SDK 要求加 -fno-rtti。
target_compile_options(server PRIVATE -g -fno-rtti)
target_compile_options(oss_upload_worker PRIVATE -g -fno-rtti)

# 业务微服务不编译 OSS SDK 代码，不需要 -fno-rtti。
target_compile_options(auth_service PRIVATE -g)
target_compile_options(user_service PRIVATE -g)
target_compile_options(filemeta_service PRIVATE -g)

# API Gateway 需要：
# - wfrest：HTTP 服务
# - srpc/protobuf/workflow：调用后端微服务
# - OSS SDK：下载文件
# - RabbitMQ：发布上传任务
# - crypto/ssl/jwt：计算文件 hash 等工具依赖
target_link_libraries(server PRIVATE
    alibabacloud-oss-cpp-sdk
    SimpleAmqpClient
    rabbitmq
    srpc
    lz4
    snappy
    protobuf
    workflow
    wfrest
    curl
    crypto
    ssl
    jwt
    pthread
)

# AuthService 需要：
# - srpc/protobuf/workflow：提供 RPC 服务和访问 MySQL
# - crypto/ssl/jwt：密码哈希和 JWT
target_link_libraries(auth_service PRIVATE
    srpc
    lz4
    snappy
    protobuf
    workflow
    crypto
    ssl
    jwt
    pthread
)

# UserService 需要 srpc/protobuf/workflow 访问 MySQL。
target_link_libraries(user_service PRIVATE
    srpc
    lz4
    snappy
    protobuf
    workflow
    pthread
)

# FileMetaService 需要 srpc/protobuf/workflow 访问 MySQL。
target_link_libraries(filemeta_service PRIVATE
    srpc
    lz4
    snappy
    protobuf
    workflow
    pthread
)

# OSS worker 需要：
# - RabbitMQ：消费上传任务
# - OSS SDK：上传对象
target_link_libraries(oss_upload_worker PRIVATE
    alibabacloud-oss-cpp-sdk
    SimpleAmqpClient
    rabbitmq
    workflow
    curl
    crypto
    ssl
    pthread
)
