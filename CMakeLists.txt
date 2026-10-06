cmake_minimum_required(VERSION 3.20)

project(MuyunMusic VERSION 1.1.7 LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# ---------------------------------------------------------------------------
# Qt
# ---------------------------------------------------------------------------
find_package(Qt6 REQUIRED COMPONENTS
    Core Gui Qml Quick QuickControls2 Network Multimedia Concurrent Sql Svg Widgets
)

qt_standard_project_setup(REQUIRES 6.5)

# ---------------------------------------------------------------------------
# QuickJS（LX 音源脚本引擎，纯 C 无依赖）
# ---------------------------------------------------------------------------
add_library(quickjs STATIC
    src/vendor/quickjs/quickjs.c
    src/vendor/quickjs/libregexp.c
    src/vendor/quickjs/libunicode.c
    src/vendor/quickjs/cutils.c
    src/vendor/quickjs/libbf.c
)
target_include_directories(quickjs PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src/vendor/quickjs)
target_compile_definitions(quickjs PRIVATE
    CONFIG_VERSION=\"2024-01-13\"
)
# 关闭对第三方代码的告警
target_compile_options(quickjs PRIVATE -w)
if(WIN32)
    target_link_libraries(quickjs PRIVATE winmm)
endif()

# ---------------------------------------------------------------------------
# 源文件
# ---------------------------------------------------------------------------
file(GLOB_RECURSE CORE_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/core/*.cpp"
)
# 排除测试文件（含 main 的独立可执行）
list(FILTER CORE_SOURCES EXCLUDE REGEX "/test/")
file(GLOB_RECURSE UI_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/ui/*.cpp"
)
# minimp3（公有领域 MP3 解码，音效管线用）实现单元
set(VENDOR_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/src/vendor/minimp3/minimp3_impl.cpp"
)
set(PROJECT_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/src/main.cpp
    ${CORE_SOURCES}
    ${UI_SOURCES}
    ${VENDOR_SOURCES}
)
list(REMOVE_DUPLICATES PROJECT_SOURCES)

# 自检开关（须在源列表定稿前处理）：OFF 时剔除测试专用实现单元
option(MUYUN_SELFTES "Compile self-tests and debug diagnostics" ON)
if(NOT MUYUN_SELFTES)
    list(FILTER PROJECT_SOURCES EXCLUDE REGEX "LxMockClient\\.cpp$")
endif()

# ---------------------------------------------------------------------------
# 可执行文件
# ---------------------------------------------------------------------------
qt_add_executable(MuyunMusic ${PROJECT_SOURCES})

# 通过 -DMUYUN_CONSOLE=ON 编译控制台版本，便于查看 Qt 运行日志
option(MUYUN_CONSOLE "Build as console app for diagnostics" OFF)
if(WIN32 AND NOT MUYUN_CONSOLE)
    set_target_properties(MuyunMusic PROPERTIES WIN32_EXECUTABLE TRUE)
endif()

# ---------------------------------------------------------------------------
# 自检/调试开关：发布打包用 -DMUYUN_SELFTES=OFF，把 --test-* 自检代码、
# MUYUN_DEBUG_* 诊断 printf、qDebug/qWarning 输出全部编译掉（不属于程序使用）
# ---------------------------------------------------------------------------
if(MUYUN_SELFTES)
    target_compile_definitions(MuyunMusic PRIVATE MUYUN_SELFTES)
else()
    target_compile_definitions(MuyunMusic PRIVATE QT_NO_DEBUG_OUTPUT QT_NO_WARNING_OUTPUT)
endif()

target_include_directories(MuyunMusic PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/src/core
    ${CMAKE_CURRENT_SOURCE_DIR}/src/vendor
    # FFmpeg 公共头（最小闭包 36 个，仅用于声明类型；符号运行时 LoadLibrary 解析，不链接）
    ${CMAKE_CURRENT_SOURCE_DIR}/src/vendor/ffmpeg/include
)
# minimp3 是第三方公有领域单头库，关掉其警告
set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/src/vendor/minimp3/minimp3_impl.cpp"
    PROPERTIES COMPILE_OPTIONS "-w")

target_link_libraries(MuyunMusic PRIVATE
    quickjs
    Qt6::Core Qt6::Gui Qt6::Qml Qt6::Quick Qt6::QuickControls2
    Qt6::Network Qt6::Multimedia Qt6::Concurrent Qt6::Sql Qt6::Svg Qt6::Widgets
)

# 洛雪同步 gzip 用 zlib：MinGW sysroot 自带 libz.a（g++ 原生 -lz），不走 FindZLIB
if(MINGW)
    target_link_libraries(MuyunMusic PRIVATE z)
else()
    find_package(ZLIB REQUIRED)
    target_link_libraries(MuyunMusic PRIVATE ZLIB::ZLIB)
endif()

target_compile_definitions(MuyunMusic PRIVATE
    MUYUN_VERSION="${PROJECT_VERSION}"
)

if(WIN32)
    # imm32：ImeGuard 要在"焦点不在输入框"时把窗口 IME 上下文接回来
    # （Qt 的 QWindowsInputContext 遇到不接受输入法的焦点对象会 ImmAssociateContext(hwnd, NULL)）
    target_link_libraries(MuyunMusic PRIVATE psapi imm32)
endif()

# ---------------------------------------------------------------------------
# QML 资源（整个 qml 目录打进 qrc，路径为 :/qml/...）
# ---------------------------------------------------------------------------
file(GLOB_RECURSE QML_FILES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/qml/*.qml"
    "${CMAKE_CURRENT_SOURCE_DIR}/qml/*.js"
)
qt_add_resources(MuyunMusic "qml_resources"
    PREFIX "/"
    BASE ${CMAKE_CURRENT_SOURCE_DIR}
    FILES ${QML_FILES}
)

# ---------------------------------------------------------------------------
# 图标资源（:/resources/app.svg）
# ---------------------------------------------------------------------------
# qt.conf：强制 Qt 仅从程序目录加载插件与 QML 模块，
# 避免系统环境变量（QT_PLUGIN_PATH 等）干扰导致"平台插件无法初始化"
add_custom_command(TARGET MuyunMusic POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${CMAKE_CURRENT_SOURCE_DIR}/resources/qt.conf
        $<TARGET_FILE_DIR:MuyunMusic>/qt.conf
    COMMENT "部署 qt.conf"
)

qt_add_resources(MuyunMusic "app_resources"
    PREFIX "/resources"
    BASE ${CMAKE_CURRENT_SOURCE_DIR}/resources
    FILES ${CMAKE_CURRENT_SOURCE_DIR}/resources/app.svg
)

# Windows 可执行文件图标（app.rc.in + app.ico）
# ⚠ app.rc 里的 ICON 路径由 windres 按"当前工作目录"解析，写相对路径不可靠、写死本机绝对路径
#   会让别人 clone 到别的目录编不过 → 仓库只留模板，configure 阶段生成带绝对路径的 app.rc。
if(WIN32)
    set(MUYUN_APP_ICON "${CMAKE_CURRENT_SOURCE_DIR}/resources/app.ico")
    configure_file("${CMAKE_CURRENT_SOURCE_DIR}/resources/app.rc.in"
                   "${CMAKE_CURRENT_BINARY_DIR}/app.rc" @ONLY)
    target_sources(MuyunMusic PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/app.rc")
endif()

# ---------------------------------------------------------------------------
# 线性图标资源（:/icons/*.svg）
# ---------------------------------------------------------------------------
file(GLOB ICON_FILES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/resources/icons/*.svg"
)
qt_add_resources(MuyunMusic "icon_resources"
    PREFIX "/icons"
    BASE ${CMAKE_CURRENT_SOURCE_DIR}/resources/icons
    FILES ${ICON_FILES}
)

# ---------------------------------------------------------------------------
# 部署依赖（Windows）
# ---------------------------------------------------------------------------
if(WIN32)
    find_program(WINDEPLOYQT_EXECUTABLE NAMES windeployqt
        HINTS ${Qt6_DIR}/../../../bin)
    if(WINDEPLOYQT_EXECUTABLE)
        add_custom_command(TARGET MuyunMusic POST_BUILD
            COMMAND ${WINDEPLOYQT_EXECUTABLE}
                --qmldir ${CMAKE_CURRENT_SOURCE_DIR}/qml
                --no-translations
                $<TARGET_FILE:MuyunMusic>
            COMMENT "部署 Qt 运行时依赖"
            VERBATIM
        )
    endif()
endif()

# ---------------------------------------------------------------------------
# MuyunStage：Mineradio 舞台引擎独立宿主壳（纯 Win32+WebView2，不依赖 Qt）
# ---------------------------------------------------------------------------
option(MUYUN_STAGE "Build MuyunStage helper (Windows only)" ${WIN32})
if(MUYUN_STAGE AND WIN32)
    add_subdirectory(stage)
    # 部署：把舞台页面资源（GPL mineradio + 自研 host glue）拷到 build/stage/web，
    # 与 MuyunStage.exe 同根目录，主程序按 applicationDir/stage/web 寻址
    add_custom_command(TARGET MuyunMusic POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            ${CMAKE_CURRENT_SOURCE_DIR}/stage/web
            $<TARGET_FILE_DIR:MuyunMusic>/stage/web
        COMMENT "部署舞台页面资源 stage/web"
    )
    # 主程序把 MuyunStage.exe 拷进自己目录（QProcess 同目录拉起）
    add_custom_command(TARGET MuyunMusic POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:MuyunStage>
            $<TARGET_FILE_DIR:MuyunMusic>/MuyunStage.exe
        COMMENT "部署 MuyunStage.exe"
    )
    add_dependencies(MuyunMusic MuyunStage)
endif()

# ---------------------------------------------------------------------------
# LX 脚本引擎冒烟测试（控制台，独立可执行；仅自检构建参与）
# ---------------------------------------------------------------------------
if(MUYUN_SELFTES)
add_executable(lx_smoke_test
    src/core/music/lx/test/test_lx.cpp
    src/core/music/lx/LxScriptEngine.cpp
    src/core/network/HttpClient.cpp
    src/core/utils/Crypto.cpp
)
target_include_directories(lx_smoke_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/src/core
)
target_link_libraries(lx_smoke_test PRIVATE
    quickjs
    Qt6::Core Qt6::Network Qt6::Concurrent
)
# LxScriptEngine 的 utils.zlib 用 zlib（与主程序同一套：MinGW sysroot 自带 libz.a）
if(MINGW)
    target_link_libraries(lx_smoke_test PRIVATE z)
else()
    find_package(ZLIB REQUIRED)
    target_link_libraries(lx_smoke_test PRIVATE ZLIB::ZLIB)
endif()
endif()
