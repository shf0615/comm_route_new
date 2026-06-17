cmake_minimum_required(VERSION 3.14)
project(comm_route C)
set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

# 库
add_library(comm_route src/comm_route.c)
target_include_directories(comm_route PUBLIC src/)

# Unity 测试框架
include(FetchContent)
FetchContent_Declare(unity
  GIT_REPOSITORY https://github.com/ThrowTheSwitch/Unity.git
  GIT_TAG v2.5.2)
FetchContent_MakeAvailable(unity)

# 测试
enable_testing()
add_executable(test_all tests/test_main.c tests/test_instance.c tests/test_send.c tests/test_broadcast.c tests/test_recv.c tests/test_ack.c tests/test_multihop.c tests/test_extra.c tests/test_coverage.c tests/test_len_field.c)
target_link_libraries(test_all comm_route unity)
add_test(NAME unit_tests COMMAND test_all)

# 示例
add_executable(sim_demo examples/sim_demo.c)
target_link_libraries(sim_demo comm_route)
