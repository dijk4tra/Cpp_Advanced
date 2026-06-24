cmake_minimum_required(VERSION 3.16)

project(SearchEngineV1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include_directories(include)
include_directories(/usr/local/include)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/bin)

add_executable(offline_builder
    src/offline/offline_main.cc
    src/common/Config.cc
    src/common/DirectoryScanner.cc
    src/common/TextUtils.cc
    src/offline/KeywordProcessor.cc
    src/offline/PageProcessor.cc
)

add_executable(search_server
    src/online/online_main.cc
    src/common/Config.cc
    src/common/TextUtils.cc
    src/online/ProtocolCodec.cc
    src/online/SearchServer.cc
    src/online/KeywordRecommender.cc
    src/online/PageLibrary.cc
    src/online/DynamicAbstract.cc
    src/online/WebSearcher.cc
    src/online/WebHttpServer.cc
)

find_library(TINYXML2_LIB tinyxml2)
find_library(MUDUO_NET_LIB muduo_net)
find_library(MUDUO_BASE_LIB muduo_base)


if (TINYXML2_LIB)
    target_link_libraries(offline_builder ${TINYXML2_LIB})
    target_link_libraries(search_server PRIVATE ${TINYXML2_LIB})
else()
    message(FATAL_ERROR "tinyxml2 library not found. Please install libtinyxml2-dev or add tinyxml2.cpp manually.")
endif()

if (MUDUO_NET_LIB AND MUDUO_BASE_LIB)
    target_link_libraries(search_server PRIVATE ${MUDUO_NET_LIB} ${MUDUO_BASE_LIB} pthread)
else()
    message(FATAL_ERROR "muduo library not found. Please install muduo_net and muduo_base.")
endif())
