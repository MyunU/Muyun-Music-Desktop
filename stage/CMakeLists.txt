# ============================================================
#  MuyunStage — Mineradio 舞台引擎宿主（独立进程壳）
#  纯 Win32 + WebView2 SDK（动态加载 WebView2Loader.dll），不依赖 Qt
# ============================================================
cmake_minimum_required(VERSION 3.20)
project(MuyunStage VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(MuyunStage WIN32
    src/stage_main.cpp
    src/ipc.cpp
)

target_include_directories(MuyunStage PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/sdk/include
)

target_link_libraries(MuyunStage PRIVATE
    ole32 oleaut32 uuid shell32 shlwapi user32 gdi32
)

if(MINGW)
    # __argc/__argv/__wcsdup 兼容
    target_link_libraries(MuyunStage PRIVATE mingwex)
endif()

# GUI 程序（--console 调试时关闭）
option(STAGE_CONSOLE "Build MuyunStage as console app for diagnostics" OFF)
if(NOT STAGE_CONSOLE)
    set_target_properties(MuyunStage PROPERTIES WIN32_EXECUTABLE TRUE)
endif()
target_compile_definitions(MuyunStage PRIVATE UNICODE _UNICODE)

# WebView2Loader.dll 拷到产物目录
add_custom_command(TARGET MuyunStage POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${CMAKE_CURRENT_SOURCE_DIR}/sdk/WebView2Loader.dll
        $<TARGET_FILE_DIR:MuyunStage>/WebView2Loader.dll
    COMMENT "部署 WebView2Loader.dll"
)