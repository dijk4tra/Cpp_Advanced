#!/bin/bash

# 遇到任何命令执行失败时，立即退出脚本。
# 这样可以避免 cmake/make 失败后，脚本还继续生成 run.sh。
set -e

# 如果执行 ./build.sh clean，则删除旧的 build 目录，进行全量干净编译。
# 普通执行 ./build.sh 时，不会删除 build 目录，会进行增量编译。
if [ "$1" = "clean" ]; then
    echo "Clean build: removing build directory..."
    rm -rf build
fi

# 如果 build 目录不存在，则创建 build 目录。
# 如果 build 目录已经存在，则直接复用它。
if [ ! -d build ]; then
    mkdir build
fi

# 进入 build 目录，使用 CMake 生成 Makefile，然后编译项目。
cd build

# 使用 CMake 编译
cmake ..
make

# 回到项目根目录。
cd ..

# 自动生成 run.sh 启动脚本。
cat > run.sh << 'EOF'
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
EOF

# 给 run.sh 可执行权限
chmod +x run.sh

echo "Build complete."
echo "Use ./run.sh to start the server."
