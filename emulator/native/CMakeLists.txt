cmake_minimum_required(VERSION 3.16)
project(aalu_emulator LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

find_package(OpenSSL REQUIRED)
find_package(Threads REQUIRED)

add_executable(aalu_emulator main.cpp)

target_compile_options(aalu_emulator PRIVATE
  -Wall -Wextra -Wpedantic
  -Wno-unused-parameter
)

target_link_libraries(aalu_emulator PRIVATE
  OpenSSL::Crypto
  Threads::Threads
)

install(TARGETS aalu_emulator RUNTIME DESTINATION bin)
