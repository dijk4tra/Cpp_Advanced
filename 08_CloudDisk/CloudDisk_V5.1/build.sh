#!/bin/bash

# 遇到任何命令执行失败时，立即退出脚本。
set -e

# 如果执行 ./build.sh clean，则删除旧的 build 目录，进行全量干净编译。
if [ "$1" = "clean" ]; then
    echo "Clean build: removing build directory..."
    rm -rf build
fi

# 第四期新增 protobuf/srpc。
# 每次构建前重新生成代码，避免 proto/cloud_disk.proto 修改后 C++ 代码没有同步。
#
# 生成目录命名为 rpc_gen：
# - rpc 表示这些文件服务于 RPC 接口。
# - gen 表示它们是工具生成代码，不是手写业务代码。
mkdir -p rpc_gen
protoc -I ./proto --cpp_out=./rpc_gen proto/cloud_disk.proto
srpc_generator protobuf proto/cloud_disk.proto ./rpc_gen

# 如果 build 目录不存在，则创建 build 目录。
if [ ! -d build ]; then
    mkdir build
fi

# 进入 build 目录，使用 CMake 生成 Makefile，然后编译项目。
cd build
cmake ..
make
cd ..

# 自动生成 run.sh 启动脚本。
cat > run.sh << 'EOF'
#!/bin/bash

# 遇到任何命令执行失败时，立即退出脚本。
set -e

# 检查 .env 文件是否存在。
if [ ! -f .env ]; then
    echo "Error: .env file not found."
    exit 1
fi

# 将 .env 文件中的变量自动导出为环境变量。
set -a
source .env
set +a

# RabbitMQ 容器名称。
RABBITMQ_CONTAINER=${RABBITMQ_CONTAINER:-rabbit}

# 启动服务端前，先确保 RabbitMQ 容器正在运行。
if ! command -v docker >/dev/null 2>&1; then
    echo "Error: docker command not found. Please start RabbitMQ manually."
    exit 1
fi

# docker inspect 可以判断容器是否存在。
if ! docker inspect "$RABBITMQ_CONTAINER" >/dev/null 2>&1; then
    echo "Error: RabbitMQ container '$RABBITMQ_CONTAINER' not found."
    echo "Please create it first, or set RABBITMQ_CONTAINER in .env."
    exit 1
fi

# 读取容器运行状态。
RABBITMQ_RUNNING=$(docker inspect -f '{{.State.Running}}' "$RABBITMQ_CONTAINER")

if [ "$RABBITMQ_RUNNING" != "true" ]; then
    echo "Starting RabbitMQ container: $RABBITMQ_CONTAINER"
    docker start "$RABBITMQ_CONTAINER" >/dev/null
else
    echo "RabbitMQ container is already running: $RABBITMQ_CONTAINER"
fi

# 等待 RabbitMQ 服务真正 ready。
echo "Waiting for RabbitMQ service to be ready..."
for i in {1..30}; do
    if docker exec "$RABBITMQ_CONTAINER" rabbitmq-diagnostics -q check_running >/dev/null 2>&1 \
        && docker exec "$RABBITMQ_CONTAINER" rabbitmq-diagnostics -q check_port_connectivity >/dev/null 2>&1; then
        echo "RabbitMQ is ready."
        break
    fi

    if [ "$i" -eq 30 ]; then
        echo "Error: RabbitMQ service is not ready after 30 seconds."
        exit 1
    fi

    sleep 1
done

# Consul 容器名称。
CONSUL_CONTAINER=${CONSUL_CONTAINER:-consul1}

# 第五期要求必须使用 Consul 注册中心。
# 启动业务服务前，先确认 Consul HTTP API 可用。
echo "Checking Consul service..."

if ! command -v curl >/dev/null 2>&1; then
    echo "Error: curl command not found, cannot check Consul."
    exit 1
fi

# 如果 Consul 容器存在但没有运行，尝试启动它。
if docker inspect "$CONSUL_CONTAINER" >/dev/null 2>&1; then
    CONSUL_CMD=$(docker inspect "$CONSUL_CONTAINER" --format '{{join .Config.Cmd " "}}')
    CONSUL_RUNNING=$(docker inspect -f '{{.State.Running}}' "$CONSUL_CONTAINER")

    if [ "$CONSUL_RUNNING" != "true" ]; then
        if [[ "$CONSUL_CMD" == *"-bootstrap-expect 2"* ]]; then
            echo "Error: Consul container '$CONSUL_CONTAINER' was created as a multi-node server (-bootstrap-expect 2)."
            echo "Error: This run.sh starts only one Consul container, so this old container cannot be used as a single-node registry."
            echo "Error: Recreate a single-node Consul container with -bootstrap-expect 1, or start the full Consul cluster manually."
            exit 1
        fi

        echo "Starting Consul container: $CONSUL_CONTAINER"
        docker start "$CONSUL_CONTAINER" >/dev/null
    else
        echo "Consul container is already running: $CONSUL_CONTAINER"
    fi
else
    echo "Error: Consul container '$CONSUL_CONTAINER' not found."
    echo "Please create a single-node Consul container first, or set CONSUL_CONTAINER in .env."
    exit 1
fi

# 等待 Consul HTTP API ready。
for i in {1..30}; do
    if curl -fsS "${CONSUL_HTTP_ADDR:-http://127.0.0.1:8500}/v1/status/leader" >/dev/null 2>&1; then
        echo "Consul is ready."
        break
    fi

    if [ "$i" -eq 30 ]; then
        echo "Error: Consul service is not ready after 30 seconds."
        exit 1
    fi

    sleep 1
done

# 保存后台进程 pid。
PIDS=()

# 退出时清理后台微服务和 worker。
cleanup()
{
    for pid in "${PIDS[@]}"; do
        kill "$pid" >/dev/null 2>&1 || true
    done
}

# 脚本退出、Ctrl+C、终止信号都会执行 cleanup。
trap cleanup EXIT INT TERM

# 启动三个 srpc 微服务。
./bin/auth_service &
PIDS+=($!)

./bin/user_service &
PIDS+=($!)

./bin/filemeta_service &
PIDS+=($!)

# 启动 OSS 上传 worker。
./bin/oss_upload_worker &
PIDS+=($!)

# 给后台服务一点启动时间，避免网关刚启动就立刻调用尚未监听的端口。
sleep 1

# API Gateway 前台运行。
# 用户按 Ctrl+C 时，trap 会清理后台服务。
./bin/server
EOF

# 给 run.sh 可执行权限。
chmod +x run.sh

echo "Build complete."
echo "Use ./run.sh to start all CloudDisk microservice processes."
