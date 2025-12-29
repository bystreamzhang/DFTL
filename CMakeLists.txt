cmake_minimum_required(VERSION 2.8.12)

project(project_hw C)

set(CMAKE_C_STANDARD 99)

set(ALG_DIR ${PROJECT_SOURCE_DIR}/ftl)

set(HEADER_DIRS
    ${ALG_DIR}
)

set(SOURCES
    ${PROJECT_SOURCE_DIR}/main.c
    ${ALG_DIR}/ftl.c
)

include_directories(${HEADER_DIRS})

add_executable(project_hw  ${SOURCES})

target_compile_options(project_hw PRIVATE 
    -g
    -std=c99
    # 下面的几行，提交时注释掉
    # -Wall
    # -Wextra
    # -Wpedantic
    # 开启这一行会把所有警告当错误（本地调试建议开，比赛不确定就先关）
    # -Werror
    # -fsanitize=address,undefined
)

target_compile_definitions(project_hw PRIVATE _GNU_SOURCE)

find_package(Threads REQUIRED)

target_link_libraries(project_hw PRIVATE Threads::Threads)