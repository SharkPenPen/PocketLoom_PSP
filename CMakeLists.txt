# PocketLoom —— PC 编辑器 + PC 预览
cmake_minimum_required(VERSION 3.20)

project(PocketLoom
        VERSION 0.1.0
        DESCRIPTION "PocketLoom - lightweight 2D handheld engine"
        LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")

if(PSP)
  # PSP 只编 runtime + engine，不编编辑器/工具/测试。
  add_compile_options(
    -G0
    -fexceptions
    -frtti
    -fno-threadsafe-statics
    -ffunction-sections
    -fdata-sections
    -Wall
    -Wextra
  )
  add_subdirectory(engine)
  add_subdirectory(runtime)
  return()
endif()

if(MSVC)
  add_compile_options(/EHs-c- /GR-)
else()
  add_compile_options(
    -fno-exceptions
    -fno-rtti
    -fno-threadsafe-statics
    -ffunction-sections
    -fdata-sections
  )
  add_link_options(-Wl,--gc-sections -static)
  add_compile_options(-Wall -Wextra -Werror=switch)
endif()

add_custom_target(check-include-deps
  COMMAND ${CMAKE_COMMAND}
          -DROOT=${CMAKE_SOURCE_DIR}
          -P ${CMAKE_SOURCE_DIR}/cmake/check-include-deps.cmake
  COMMENT "check-include-deps: engine must not include editor"
  VERBATIM)

enable_testing()
add_subdirectory(engine)
add_subdirectory(runtime)
add_subdirectory(pgmmv)
add_subdirectory(editor)
add_subdirectory(tests)

add_executable(make-demo tools/make-demo.cpp)
target_link_libraries(make-demo PRIVATE p3d_format)



