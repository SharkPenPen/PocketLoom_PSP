cmake_minimum_required(VERSION 3.20)
project(PocketLoom3dsHello LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT DEFINED CTRULIB)
  set(CTRULIB "$ENV{DEVKITPRO}/libctru")
endif()
if(NOT EXISTS "${CTRULIB}/lib/libctru.a")
  message(FATAL_ERROR "libctru not found: ${CTRULIB}")
endif()

add_executable(pocketloom_3ds_hello source/main.cpp)
target_include_directories(pocketloom_3ds_hello PRIVATE "${CTRULIB}/include")
target_link_directories(pocketloom_3ds_hello PRIVATE "${CTRULIB}/lib")
target_link_libraries(pocketloom_3ds_hello PRIVATE citro2d citro3d ctru m)

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/3dsx.cmake")
eng_add_3dsx(pocketloom_3ds_hello
  TITLE "PocketLoom 3DS"
  DESC "PocketLoom minimal 3DS boot probe"
  AUTHOR "PocketLoom")
