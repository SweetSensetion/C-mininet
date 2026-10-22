cmake_minimum_required(VERSION 3.10)
project(mininet CXX)

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

add_compile_options(-Wall -Wextra -Wno-unused-parameter -g)
add_compile_options($<$<CONFIG:Release>:-O2>)

find_package(Threads REQUIRED)

set(MINET_SOURCES
  src/Acceptor.cc
  src/Buffer.cc
  src/Channel.cc
  src/Codec.cc
  src/EPollPoller.cc
  src/EventLoop.cc
  src/EventLoopThread.cc
  src/EventLoopThreadPool.cc
  src/InetAddress.cc
  src/Logging.cc
  src/Poller.cc
  src/Socket.cc
  src/TcpConnection.cc
  src/TcpServer.cc
  src/ThreadPool.cc
  src/Timer.cc
  src/TimerQueue.cc
)

add_library(minet STATIC ${MINET_SOURCES})
target_include_directories(minet PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(minet PUBLIC Threads::Threads)

add_executable(echo_server examples/echo_server.cc)
target_link_libraries(echo_server PRIVATE minet)

add_executable(bench_client examples/bench_client.cc)
target_link_libraries(bench_client PRIVATE minet)

enable_testing()
add_executable(codec_test tests/codec_test.cc)
target_link_libraries(codec_test PRIVATE minet)
add_test(NAME codec_test COMMAND codec_test)
