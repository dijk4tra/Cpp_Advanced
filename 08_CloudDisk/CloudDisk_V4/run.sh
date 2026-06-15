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
