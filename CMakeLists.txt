cmake_minimum_required(VERSION 3.22.1)
project(rdma_write_respond)

set(CMAKE_CXX_STANDARD 17)

add_definitions(-DDOCA_ALLOW_EXPERIMENTAL_API)

add_compile_options(-std=c++11 -g -Wall -pthread -I./)

include_directories(/opt/mellanox/doca/include/)
link_directories(/opt/mellanox/doca/lib/x86_64-linux-gnu/)
link_directories(/opt/mellanox/flexio/lib/)

add_executable(rdma_write_respond main.cpp
        rdma_common.cpp
        rdma_common.h
        rdma_write_respond_dep.cpp
        rdma_write_respond_dep.h
)

target_link_libraries(rdma_write_respond flexio doca_common stdc++ ibverbs mlx5 pthread doca_rdma doca_argp)

