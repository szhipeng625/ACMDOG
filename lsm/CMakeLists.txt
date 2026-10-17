cmake_minimum_required(VERSION 3.16)
project(tiny_lsm LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -pthread -fvisibility=hidden")

# 第三方依赖头文件路径
set(SPDLOG_DIR ${CMAKE_SOURCE_DIR}/third_party/spdlog/include)
set(TOML11_DIR /usr/include)

# LSM 核心库源文件
set(LSM_SOURCES
    src/block/block.cpp
    src/block/block_cache.cpp
    src/block/block_iterator.cpp
    src/block/blockmeta.cpp
    src/config/config.cpp
    src/iterator/iterator.cpp
    src/logger/logger.cpp
    src/lsm/engine.cpp
    src/lsm/level_iterator.cpp
    src/lsm/transation.cpp
    src/lsm/two_merge_iterator.cpp
    src/memtable/memtable.cpp
    src/skiplist/skipList.cpp
    src/sst/concact_iterator.cpp
    src/sst/sst.cpp
    src/sst/sst_iterator.cpp
    src/utils/bloom_filter.cpp
    src/utils/cursor.cpp
    src/utils/files.cpp
    src/utils/mmap_file.cpp
    src/utils/std_file.cpp
    src/vlog/vlog.cpp
    src/wal/record.cpp
    src/wal/wal.cpp
    src/redis_wrapper/redis_wrapper.cpp
)

# LSM 静态库
add_library(lsm STATIC ${LSM_SOURCES})
target_include_directories(lsm PUBLIC
    ${CMAKE_SOURCE_DIR}/include
    ${SPDLOG_DIR}
    ${TOML11_DIR}
)
target_link_libraries(lsm PUBLIC pthread)

# RESP 协议服务器
add_executable(lsm_server
    server/src/server.cpp
    server/src/handler.cpp
)
target_include_directories(lsm_server PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/server/include
    ${SPDLOG_DIR}
    ${TOML11_DIR}
)
target_link_libraries(lsm_server PRIVATE lsm pthread)

# 如果系统有 asio，链接它
find_path(ASIO_INCLUDE_DIR asio.hpp PATHS /usr/include)
if(ASIO_INCLUDE_DIR)
    target_include_directories(lsm_server PRIVATE ${ASIO_INCLUDE_DIR})
endif()
