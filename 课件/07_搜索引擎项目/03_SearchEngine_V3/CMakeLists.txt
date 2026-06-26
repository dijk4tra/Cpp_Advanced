# 指定当前项目要求的 CMake 最低版本
cmake_minimum_required(VERSION 3.16)

# 项目名称和语言(其中 CXX 表示 C++)
project(SearchEngineV2 LANGUAGES CXX)

# 指定项目使用 C++17 标准
set(CMAKE_CXX_STANDARD 17)
# 要求必须使用上面指定的 C++ 标准
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# 禁止使用编译器私有扩展标准
set(CMAKE_CXX_EXTENSIONS OFF)

# 添加头文件搜索目录
# 编译源文件时，编译器会额外去当前项目的 include 目录中查找头文件
include_directories(include)

# 添加第三方库的头文件搜索目录
# /usr/local/include 通常用于存放手动安装的第三方库头文件
include_directories(/usr/local/include)

# 设置可执行文件最终生成的位置
# ${CMAKE_SOURCE_DIR} 表示项目源码根目录
# 也就是顶层 CMakeLists.txt 所在的目录
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/bin)

# 定义一个可执行程序目标
# add_executable 的第一个参数是目标名称：offline_builder
# 后面的参数是参与编译这个可执行程序的源文件列表
# CMake 会把下面这些 .cc 文件一起编译、链接
# 最终生成一个名为 offline_builder 的可执行程序
add_executable(offline_builder
    src/offline/offline_main.cc
    src/common/Config.cc
    src/common/DirectoryScanner.cc
    src/common/TextUtils.cc
    src/offline/KeywordProcessor.cc
    src/offline/PageProcessor.cc
)

# 定义第二期在线服务目标。它复用 common 模块，并加载第一期生成的数据文件。
add_executable(search_server
    src/online/online_main.cc
    src/common/Config.cc
    src/common/TextUtils.cc
    src/cache/ShardedLruCache.cc
    src/cache/RedisCache.cc
    src/cache/TwoLevelCache.cc
    src/cache/CachedSearchService.cc
    src/online/ProtocolCodec.cc
    src/online/SearchServer.cc
    src/online/KeywordRecommender.cc
    src/online/PageLibrary.cc
    src/online/DynamicAbstract.cc
    src/online/WebSearcher.cc
    src/online/WebHttpServer.cc
)

# 查找 tinyxml2 库
find_library(TINYXML2_LIB tinyxml2)

# 查找 muduo 网络库。muduo_net 依赖 muduo_base，最终还需要 pthread。
find_library(MUDUO_NET_LIB muduo_net)
find_library(MUDUO_BASE_LIB muduo_base)

# 判断 tinyxml2 库是否查找成功
if (TINYXML2_LIB)
    # 把 tinyxml2 链接到 offline_builder 目标上
    #
    # 编译一个 C++ 程序通常分为两个主要阶段：
    # 1. 编译阶段：
    #    把 .cc 文件编译成 .o 目标文件。
    # 2. 链接阶段：
    #    把多个 .o 文件和依赖库组合成最终的可执行文件。
    #
    # 如果代码中调用了 tinyxml2 的函数或类，
    # 仅仅包含头文件是不够的，还必须在链接阶段链接 tinyxml2 库。
    #
    # PRIVATE 的含义是：
    # - tinyxml2 只是 offline_builder 自己内部使用的依赖；
    # - 这个依赖不会继续传播给其他目标
    #
    # 对于可执行文件来说，通常使用 PRIVATE 即可。

    target_link_libraries(offline_builder PRIVATE ${TINYXML2_LIB})
    target_link_libraries(search_server PRIVATE ${TINYXML2_LIB})

    # 手写 g++ 命令里的 -lpthread、-ltinyxml2、-lmysqlclient、-lssl、-lcrypto
    # 在 CMakeLists.txt 中主要通过 target_link_libraries 来添加。

else()
    # 如果没有找到 tinyxml2 库，就直接让 CMake 配置失败
    # message(FATAL_ERROR "...") 表示输出错误信息并终止配置过程
    message(FATAL_ERROR "tinyxml2 library not found. Please install libtinyxml2-dev or add tinyxml2.cpp manually.")

endif()

if (MUDUO_NET_LIB AND MUDUO_BASE_LIB)
    target_link_libraries(search_server PRIVATE ${MUDUO_NET_LIB} ${MUDUO_BASE_LIB} pthread)
else()
    message(FATAL_ERROR "muduo library not found. Please install muduo_net and muduo_base.")
endif()
