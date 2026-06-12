#!/bin/bash

# 遇到任何命令执行失败时，立即退出脚本。
set -e

# 检查 .env 文件是否存在。
# .env 用来保存环境变量，例如 OSS 的 AccessKey、Endpoint、Bucket 等。
if [ ! -f .env ]; then
    echo "Error: .env file not found."
    exit 1
fi

# 将 .env 文件中的变量自动导出为环境变量。
# set -a 表示之后定义的变量都会自动 export。
set -a
source .env
set +a

# RabbitMQ 容器名称。
# 如果 .env 中没有配置 RABBITMQ_CONTAINER，就默认使用当前项目里的容器名 rabbit。
RABBITMQ_CONTAINER=${RABBITMQ_CONTAINER:-rabbit}

# 启动服务端前，先确保 RabbitMQ 容器正在运行。
# 否则 C++ 后台消费者连接 127.0.0.1:5672 时，会报 socket error。
if ! command -v docker >/dev/null 2>&1; then
    echo "Error: docker command not found. Please start RabbitMQ manually."
    exit 1
fi

# docker inspect 可以判断容器是否存在。
# 如果容器不存在，说明本机还没有创建名为 $RABBITMQ_CONTAINER 的 RabbitMQ 容器。
if ! docker inspect "$RABBITMQ_CONTAINER" >/dev/null 2>&1; then
    echo "Error: RabbitMQ container '$RABBITMQ_CONTAINER' not found."
    echo "Please create it first, or set RABBITMQ_CONTAINER in .env."
    exit 1
fi

# 读取容器运行状态。
# 输出 true 表示容器正在运行；false 表示容器存在但当前停止。
RABBITMQ_RUNNING=$(docker inspect -f '{{.State.Running}}' "$RABBITMQ_CONTAINER")

if [ "$RABBITMQ_RUNNING" != "true" ]; then
    echo "Starting RabbitMQ container: $RABBITMQ_CONTAINER"
    docker start "$RABBITMQ_CONTAINER" >/dev/null
else
    echo "RabbitMQ container is already running: $RABBITMQ_CONTAINER"
fi

# docker start 只代表容器进程启动了，不代表 RabbitMQ 服务已经能接收 AMQP 连接。
# RabbitMQ 从容器启动到 5672 端口真正可用通常还需要几秒。
# check_running 确认 RabbitMQ 应用已经启动。
# check_port_connectivity 确认 5672/15672 等端口已经可以连接。
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

# 启动服务端程序
./server
