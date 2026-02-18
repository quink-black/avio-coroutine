cmake_minimum_required(VERSION 3.20)
project(avio_boost_spawn)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBAV REQUIRED IMPORTED_TARGET
    libavformat
    libavcodec
    libavutil
)

# (context + coroutine for boost::asio::spawn)
find_package(Boost REQUIRED COMPONENTS context coroutine)
message(STATUS "Found Boost: ${Boost_VERSION}")

add_executable(avio_coroutine main.cpp remux.cpp remux.h)
target_include_directories(avio_coroutine PRIVATE ${Boost_INCLUDE_DIRS})
target_link_libraries(avio_coroutine PRIVATE
    PkgConfig::LIBAV
    Boost::context
    Boost::coroutine)
