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

# Consul 容器名称列表。
# 三节点本地学习环境默认使用 consul1、consul2、consul3。
CONSUL_CONTAINERS=${CONSUL_CONTAINERS:-consul1,consul2,consul3}

# Consul HTTP API 地址列表。
# 这三个端口分别映射到三个 Consul server 容器内部的 8500 端口。
CONSUL_HTTP_ADDRS=${CONSUL_HTTP_ADDRS:-http://127.0.0.1:8500,http://127.0.0.1:8501,http://127.0.0.1:8502}

# 把默认值导出给后面启动的 auth_service、user_service、filemeta_service 和 API Gateway。
export CONSUL_HTTP_ADDRS

# 三节点 Consul 的多数派是 2。
# 至少 2 个节点可用时，Consul 集群才具备继续工作的基础条件。
CONSUL_READY_MIN=${CONSUL_READY_MIN:-2}

# 第五期要求必须使用 Consul 注册中心。
# 启动业务服务前，先确认三节点 Consul 集群可用。
echo "Checking Consul cluster..."

if ! command -v curl >/dev/null 2>&1; then
    echo "Error: curl command not found, cannot check Consul."
    exit 1
fi

# 按逗号把 CONSUL_CONTAINERS 拆成 Bash 数组。
IFS=',' read -r -a CONSUL_CONTAINER_LIST <<< "$CONSUL_CONTAINERS"

# 逐个检查并启动 Consul 容器。
for CONSUL_CONTAINER in "${CONSUL_CONTAINER_LIST[@]}"; do
    # docker inspect 可以判断容器是否存在。
    if ! docker inspect "$CONSUL_CONTAINER" >/dev/null 2>&1; then
        echo "Error: Consul container '$CONSUL_CONTAINER' not found."
        echo "Please create the three-node Consul cluster first, or set CONSUL_CONTAINERS in .env."
        exit 1
    fi

    # 读取容器运行状态。
    CONSUL_RUNNING=$(docker inspect -f '{{.State.Running}}' "$CONSUL_CONTAINER")

    # 如果容器没有运行，就尝试启动。
    if [ "$CONSUL_RUNNING" != "true" ]; then
        echo "Starting Consul container: $CONSUL_CONTAINER"
        docker start "$CONSUL_CONTAINER" >/dev/null
    else
        echo "Consul container is already running: $CONSUL_CONTAINER"
    fi
done

# 按逗号把 CONSUL_HTTP_ADDRS 拆成 Bash 数组。
IFS=',' read -r -a CONSUL_ADDR_LIST <<< "$CONSUL_HTTP_ADDRS"

# 等待 Consul 集群 ready。
for i in {1..30}; do
    # 每一轮重新统计可用 HTTP API 数量。
    CONSUL_READY_COUNT=0

    # 逐个检查 Consul HTTP API。
    for CONSUL_ADDR in "${CONSUL_ADDR_LIST[@]}"; do
        # /v1/status/leader 能返回非空 leader，说明当前节点已经加入有 leader 的集群。
        CONSUL_LEADER=$(curl -fsS "$CONSUL_ADDR/v1/status/leader" 2>/dev/null || true)

        # Consul 没有 leader 时通常返回空字符串 ""，不能算 ready。
        if [ -n "$CONSUL_LEADER" ] && [ "$CONSUL_LEADER" != '""' ]; then
            CONSUL_READY_COUNT=$((CONSUL_READY_COUNT + 1))
        fi
    done

    # 三节点默认要求至少 2 个 ready。
    if [ "$CONSUL_READY_COUNT" -ge "$CONSUL_READY_MIN" ]; then
        echo "Consul cluster is ready: $CONSUL_READY_COUNT node(s) ready."
        break
    fi

    # 30 秒后仍然不满足 ready 条件，就停止启动业务服务。
    if [ "$i" -eq 30 ]; then
        echo "Error: Consul cluster is not ready after 30 seconds."
        echo "Error: ready node(s): $CONSUL_READY_COUNT, required: $CONSUL_READY_MIN."
        exit 1
    fi

    # 等待 1 秒后重试。
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
