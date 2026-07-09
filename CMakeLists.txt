cmake_minimum_required(VERSION 3.16)
project(zepto VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wextra -O2 -march=native -msse4.2")

add_executable(zepto
    main.cpp
    zepto.cpp
)

target_include_directories(zepto PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
