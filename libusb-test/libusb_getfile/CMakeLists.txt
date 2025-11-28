cmake_minimum_required(VERSION 3.0)

project(libusb_getfile)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

###############################################################################
find_package(Threads REQUIRED)
###############################################################################

add_definitions(-D_GNU_SOURCE)

file(GLOB sources libusb_getfile.c)

# include_directories(sources)
add_executable(${PROJECT_NAME} ${sources})

target_link_options(${PROJECT_NAME} PRIVATE -Wl,-Map=${PROJECT_NAME}.map)

target_link_libraries(${PROJECT_NAME} usb-1.0)
target_compile_options(
  ${PROJECT_NAME}
  PRIVATE
    $<$<COMPILE_LANGUAGE:C>:-std=c99;-Wall;-Wextra;-Wunused-macros;-Wmissing-braces;-Wno-unused-parameter;-Wmissing-include-dirs>
)
