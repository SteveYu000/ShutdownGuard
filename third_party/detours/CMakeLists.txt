add_library(Detours STATIC
    creatwth.cpp
    detours.cpp
    disasm.cpp
    disolarm.cpp
    disolarm64.cpp
    disolia64.cpp
    disolx64.cpp
    disolx86.cpp
    image.cpp
    modules.cpp
)

target_include_directories(Detours
  PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}"
)

target_compile_definitions(Detours
  PRIVATE
    WIN32_LEAN_AND_MEAN
    DETOUR_DEBUG=$<IF:$<CONFIG:Debug>,1,0>
)

if(MSVC)
  target_compile_options(Detours
    PRIVATE
      /Zl
      /Gy
      /Gm-
  )
endif()
