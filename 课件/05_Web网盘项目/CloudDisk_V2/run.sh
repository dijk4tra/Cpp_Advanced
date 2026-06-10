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

# 启动服务端程序
./server
