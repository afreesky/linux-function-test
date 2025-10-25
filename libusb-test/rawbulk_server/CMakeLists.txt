cmake_minimum_required(VERSION 3.0)

project(rawbulk_server)

set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR arm)

# 指定交叉编译器的路径
set(CMAKE_C_COMPILER aarch64-linux-gnu-gcc)
set(CMAKE_CXX_COMPILER aarch64-linux-gnu-g++)


# set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
# set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

###############################################################################
find_package(Threads REQUIRED)
###############################################################################

add_definitions(-D_GNU_SOURCE)

file(GLOB sources rawbulk_server.c)

# include_directories(sources)

add_executable(${PROJECT_NAME} ${sources})

target_compile_options(
  ${PROJECT_NAME}
  PRIVATE
    $<$<COMPILE_LANGUAGE:C>:-std=c99;-Wall;-Wextra;-Wunused-macros;-Wmissing-braces;-Wno-unused-parameter;-Wmissing-include-dirs>
)
