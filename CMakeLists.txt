cmake_minimum_required(VERSION 3.21)

if(POLICY CMP0091)
  cmake_policy(SET CMP0091 NEW)
endif()

if(POLICY CMP0144)
  cmake_policy(SET CMP0144 NEW)
endif()

project(ShutdownGuard
  VERSION 0.1.0
  LANGUAGES CXX
)

option(SHUTDOWN_GUARD_WITH_DETOURS "Build with Microsoft Detours shutdown API hooks" ON)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(MSVC)
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

add_executable(ShutdownGuard WIN32
  src/main.cpp
  src/ui.cpp
)

add_library(ShutdownGuardHook SHARED
  src/global_shutdown_hook.cpp
)

foreach(target ShutdownGuard ShutdownGuardHook)
target_compile_definitions(${target} PRIVATE
  UNICODE
  _UNICODE
  WIN32_LEAN_AND_MEAN
  NOMINMAX
  _WIN32_WINNT=0x0A00
)
endforeach()

target_link_libraries(ShutdownGuard PRIVATE
  user32
  gdi32
  advapi32
  dwmapi
)

target_link_libraries(ShutdownGuardHook PRIVATE
  user32
  advapi32
  shell32
  wtsapi32
)

add_subdirectory(third_party/detours)

if(SHUTDOWN_GUARD_WITH_DETOURS)
  target_compile_definitions(ShutdownGuardHook PRIVATE SHUTDOWN_GUARD_WITH_DETOURS=1)
  target_link_libraries(ShutdownGuardHook PRIVATE Detours)
else()
  message(FATAL_ERROR "ShutdownGuardHook requires Detours. Configure with SHUTDOWN_GUARD_WITH_DETOURS=ON.")
endif()

if(MSVC)
  target_compile_options(ShutdownGuard PRIVATE /W4 /permissive- /utf-8)
  target_compile_options(ShutdownGuardHook PRIVATE /W4 /permissive- /utf-8)
endif()
