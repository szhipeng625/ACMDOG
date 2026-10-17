cmake_minimum_required(VERSION 3.16)
project(mq_middleware CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# === AddressSanitizer (debug build) ===
option(ENABLE_ASAN "Enable AddressSanitizer" ON)
if(ENABLE_ASAN)
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -g -fsanitize=address -fno-omit-frame-pointer")
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -fsanitize=address")
    message(STATUS "AddressSanitizer ENABLED")
endif()


set(JSON_INCLUDE "/tmp" CACHE PATH "nlohmann/json.hpp dir")
set(HTTPLIB_INCLUDE "/tmp" CACHE PATH "cpp-httplib dir")

find_package(Threads REQUIRED)
find_package(OpenSSL REQUIRED)

add_executable(middleware main.cpp business.cpp)
target_include_directories(middleware PRIVATE
    ${JSON_INCLUDE} ${HTTPLIB_INCLUDE} ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(middleware PRIVATE CPPHTTPLIB_OPENSSL_SUPPORT)
target_link_libraries(middleware PRIVATE
    Threads::Threads OpenSSL::SSL OpenSSL::Crypto
    -L/usr/lib64/mysql -lmysqlclient -lzstd -lssl -lcrypto -lresolv -lm)
