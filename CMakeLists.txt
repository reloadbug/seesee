cmake_minimum_required(VERSION 3.20)
project(seesee VERSION 0.3.0 LANGUAGES CXX)

enable_language(RC)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

add_executable(seesee WIN32
  src/main.cpp
  src/app.cpp
  src/loader.cpp
  src/settings.cpp
  src/assoc.cpp
  src/i18n.cpp
  src/keys.cpp
  resources/seesee.rc
)

target_include_directories(seesee PRIVATE src)
target_compile_definitions(seesee PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)

# 极速开图优先：全程序优化 + 静态 CRT（无 VC 运行时依赖，单文件发布）
target_compile_options(seesee PRIVATE
  /utf-8 /W4 /permissive- /EHsc /O2 /Oi /Ot /GL /Gy /Gw /GS- /std:c++20
)
# /MANIFEST:NO —— 清单由 resources/seesee.rc 内嵌，避免与链接器自动生成的清单重复
target_link_options(seesee PRIVATE /LTCG /OPT:REF /OPT:ICF /SUBSYSTEM:WINDOWS /MANIFEST:NO)
set_property(TARGET seesee PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded")

target_link_libraries(seesee PRIVATE
  d2d1 dwrite windowscodecs dwmapi shcore
  user32 gdi32 shell32 ole32 uuid
)

# 输出到 bin/，方便直接双击
set_target_properties(seesee PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/bin"
  OUTPUT_NAME "seesee"
)
