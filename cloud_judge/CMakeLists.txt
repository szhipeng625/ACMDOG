cmake_minimum_required(VERSION 3.16)
project(eval_worker LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
find_package(PkgConfig REQUIRED)
pkg_check_modules(SECCOMP REQUIRED libseccomp)

# eval_worker: sandbox executor
add_executable(eval_worker
    src/main.cpp
    src/sandbox.cpp
    src/cgroup_manager.cpp
    src/thread_pool.cpp
)
target_link_libraries(eval_worker PRIVATE
    ${SECCOMP_LIBRARIES}
    pthread
)
target_include_directories(eval_worker PRIVATE
    ${SECCOMP_INCLUDE_DIRS}
    ${CMAKE_SOURCE_DIR}/include
)

# orchestrator: orchestration shell + HTTP status + preemption model
add_executable(orchestrator
    src/orchestrator.cpp
    src/thread_pool.cpp
    src/cgroup_manager.cpp
)
target_include_directories(orchestrator PRIVATE
    ${CMAKE_SOURCE_DIR}/include
)
target_link_libraries(orchestrator PRIVATE pthread)
