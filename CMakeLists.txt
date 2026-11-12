cmake_minimum_required(VERSION 3.20)
project(YasuoCompressor VERSION 2.0.0 LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

# 自动定位 Qt 安装前缀：不硬编码 Qt 版本与盘符，兼容 GitHub Actions 的 QT_ROOT_DIR。
set(_qt_roots "$ENV{QT_ROOT_DIR}" "$ENV{QT_ROOT}" "D:/Qt" "C:/Qt" "E:/Qt")
set(_qt_prefixes "")
foreach(_root IN LISTS _qt_roots)
    if(NOT _root)
        continue()
    endif()
    file(GLOB _qt_versions "${_root}/*/mingw_64")
    foreach(_candidate IN LISTS _qt_versions)
        if(EXISTS "${_candidate}/lib/cmake/Qt6")
            list(APPEND _qt_prefixes "${_candidate}")
        endif()
    endforeach()
    # QT_ROOT_DIR 有时直接指向 <version>/mingw_64
    if(EXISTS "${_root}/lib/cmake/Qt6")
        list(APPEND _qt_prefixes "${_root}")
    endif()
endforeach()
if(_qt_prefixes)
    list(REMOVE_DUPLICATES _qt_prefixes)
    list(SORT _qt_prefixes)
    list(REVERSE _qt_prefixes)
    list(GET _qt_prefixes 0 _qt_best)
    # 把 cache/命令行传入的前缀带入普通变量，避免 list(APPEND) 覆盖用户显式指定的路径
    set(CMAKE_PREFIX_PATH "${CMAKE_PREFIX_PATH};${_qt_best}")
    list(REMOVE_DUPLICATES CMAKE_PREFIX_PATH)
    message(STATUS "Qt 前缀: ${_qt_best}")
endif()

find_package(Qt6 REQUIRED COMPONENTS Core Widgets Network Concurrent)
qt_standard_project_setup()

# ---------------- zlib ----------------
find_package(ZLIB QUIET)
if(NOT ZLIB_FOUND)
    # MinGW sysroot 自带 zlib；工具链目录名带版本号，用通配扫描避免写死 mingw1310_64
    foreach(_root IN LISTS _qt_roots)
        if(NOT _root)
            continue()
        endif()
        file(GLOB _sysroots "${_root}/Tools/mingw*/x86_64-w64-mingw32")
        foreach(_sys IN LISTS _sysroots)
            if(EXISTS "${_sys}/include/zlib.h" AND EXISTS "${_sys}/lib/libz.a")
                set(ZLIB_INCLUDE_DIRS "${_sys}/include")
                set(ZLIB_LIBRARIES "${_sys}/lib/libz.a")
                set(ZLIB_FOUND TRUE)
                break()
            endif()
        endforeach()
        if(ZLIB_FOUND)
            break()
        endif()
    endforeach()
endif()
if(NOT ZLIB_FOUND)
    message(FATAL_ERROR "找不到 zlib，请安装或通过 QT_ROOT 指定 Qt 目录")
endif()

# ---------------- libarchive（本地静态优先，系统库次之，最后回退内置引擎） ----------------
option(YASUO_ENABLE_LIBARCHIVE
       "启用 libarchive 全格式引擎；OFF = 只编内置流式 ZIP/GZ 引擎（CI / 零外部依赖验证）" ON)
set(YASUO_LIBARCHIVE OFF)
set(LA_INCLUDE_DIRS "")
set(LA_LINK_LIBS "")

if(YASUO_ENABLE_LIBARCHIVE)
    set(_dist "${CMAKE_CURRENT_SOURCE_DIR}/deps/dist")
    if(EXISTS "${_dist}/include/archive.h" AND EXISTS "${_dist}/lib/libarchive.a")
        set(YASUO_LIBARCHIVE ON)
        set(LA_INCLUDE_DIRS "${_dist}/include")
        set(LA_LINK_LIBS
            "${_dist}/lib/libarchive.a"
            "${_dist}/lib/liblzma.a"
            "${_dist}/lib/libzstd.a"
            "${_dist}/lib/libbz2.a"
            "${ZLIB_LIBRARIES}"
            bcrypt advapi32 xmllite)
        message(STATUS "引擎: 本地静态 libarchive 3.9（全格式 + RAR 解压 + ZIP AES 加密）")
    else()
        find_package(LibArchive QUIET)
        if(LibArchive_FOUND)
            set(YASUO_LIBARCHIVE ON)
            set(LA_LINK_LIBS LibArchive::LibArchive "${ZLIB_LIBRARIES}")
            message(STATUS "引擎: 系统 libarchive")
        endif()
    endif()
endif()
if(NOT YASUO_LIBARCHIVE)
    message(STATUS "引擎: 内置流式 ZIP/GZ（运行 build_deps_local.sh 可获得全格式）")
endif()

# ---------------- 7-Zip SDK 后端（运行时加载 7z.dll；仅读取 RAR/7z） ----------------
# 复用 7-Zip 的客户端辅助源（与官方 Client7z 一致的最小集）编译进本进程，
# 仅通过 LoadLibrary 动态加载 Format7zF 的 7z.dll，不引入 7-Zip 的编解码器源码。
option(YASUO_ENABLE_SEVENZIP
       "启用 7-Zip SDK 后端（仅读取：RAR/RAR5 解压、7z AES/加密文件名/固实/分卷）" ON)
set(YASUO_SEVENZIP OFF)
set(SZ_INCLUDE_DIRS "")
set(SZ_SOURCES "")
if(YASUO_ENABLE_SEVENZIP)
    set(_7z_root "${CMAKE_CURRENT_SOURCE_DIR}/deps/src/7z/extracted")
    if(EXISTS "${_7z_root}/CPP/Common/Common.h" AND
       EXISTS "${_7z_root}/CPP/7zip/Bundles/Format7zF/_o/7z.dll")
        set(YASUO_SEVENZIP ON)
        set(SZ_INCLUDE_DIRS "${_7z_root}" "${_7z_root}/CPP")
        set(SZ_7Z_DLL "${_7z_root}/CPP/7zip/Bundles/Format7zF/_o/7z.dll")
        # 注意：这些 .cpp/.c 各自以 #include "StdAfx.h" 开头，StdAfx.h 会按相对路径
        # 引入 7-Zip 自己的 Common/Common.h，因此只需把 <7z>/CPP 加入包含路径即可。
        set(SZ_SOURCES
            "${_7z_root}/C/Alloc.c"
            "${_7z_root}/CPP/Common/IntToString.cpp"
            "${_7z_root}/CPP/Common/MyString.cpp"
            "${_7z_root}/CPP/Common/MyVector.cpp"
            "${_7z_root}/CPP/Common/NewHandler.cpp"
            "${_7z_root}/CPP/Common/StringConvert.cpp"
            "${_7z_root}/CPP/Common/StringToInt.cpp"
            "${_7z_root}/CPP/Common/UTFConvert.cpp"
            "${_7z_root}/CPP/Common/Wildcard.cpp"
            "${_7z_root}/CPP/Windows/DLL.cpp"
            "${_7z_root}/CPP/Windows/FileDir.cpp"
            "${_7z_root}/CPP/Windows/FileFind.cpp"
            "${_7z_root}/CPP/Windows/FileIO.cpp"
            "${_7z_root}/CPP/Windows/FileName.cpp"
            "${_7z_root}/CPP/Windows/PropVariant.cpp"
            "${_7z_root}/CPP/Windows/PropVariantConv.cpp"
            "${_7z_root}/CPP/Windows/TimeUtils.cpp"
            "${_7z_root}/CPP/7zip/Common/FileStreams.cpp"
            src/core/sevenzip/sevenzip_wrap.cpp)
        message(STATUS "引擎: 7-Zip SDK 后端（运行时加载 7z.dll，仅读取）")
    else()
        message(STATUS "引擎: 未找到 7-Zip 源码/7z.dll，跳过 7-Zip 后端（deps/src/7z/extracted）")
    endif()
endif()

set(CORE_SOURCES
    src/core/archivetypes.cpp
    src/core/pathguard.cpp
    src/core/accelerator.cpp
    src/core/enginefactory.cpp
    src/core/zlibengine.cpp
)
if(YASUO_LIBARCHIVE)
    list(APPEND CORE_SOURCES src/core/libarchiveengine.cpp)
endif()
if(YASUO_SEVENZIP)
    list(APPEND CORE_SOURCES src/core/sevenzipengine.cpp ${SZ_SOURCES})
endif()

set(APP_HEADERS
    src/ui/mainwindow.h
    src/ui/archivebrowser.h
    src/ui/taskrunner.h
    src/ui/iconfactory.h
    src/shell/shellintegration.h
)

if(WIN32)
    enable_language(RC)
endif()

add_executable(YasuoCompressor WIN32
    src/main.cpp
    src/ui/mainwindow.cpp
    src/ui/archivebrowser.cpp
    src/ui/taskrunner.cpp
    src/ui/theme.cpp
    src/ui/iconfactory.cpp
    src/shell/shellintegration.cpp
    ${CORE_SOURCES}
    ${APP_HEADERS}
    src/resources.qrc
    src/resources/app.rc
)

target_include_directories(YasuoCompressor PRIVATE
    src src/core src/ui src/shell
    ${LA_INCLUDE_DIRS} ${ZLIB_INCLUDE_DIRS} ${SZ_INCLUDE_DIRS}
    src/resources)

target_link_libraries(YasuoCompressor PRIVATE
    Qt6::Core Qt6::Widgets Qt6::Network Qt6::Concurrent
    ${LA_LINK_LIBS} ${ZLIB_LIBRARIES})

if(TARGET Qt6::EntryPointPrivate)
    target_link_libraries(YasuoCompressor PRIVATE Qt6::EntryPointPrivate)
endif()

if(YASUO_LIBARCHIVE)
    target_compile_definitions(YasuoCompressor PRIVATE YASUO_HAVE_LIBARCHIVE)
endif()
if(YASUO_SEVENZIP)
    target_compile_definitions(YasuoCompressor PRIVATE YASUO_HAVE_SEVENZIP)
    # 7-Zip 客户端辅助代码所需的系统 COM/Win32 库
    target_link_libraries(YasuoCompressor PRIVATE ole32 oleaut32 uuid advapi32 user32)
    # 抑制第三方 7-Zip 辅助源产生的告警
    set_source_files_properties(${SZ_SOURCES} PROPERTIES COMPILE_OPTIONS "-w")
endif()

# ---------------- 无界面自测 ----------------
add_executable(test_engine
    test_engine.cpp
    ${CORE_SOURCES}
    src/core/archivetypes.h
    src/core/iengine.h
    src/core/pathguard.h
    src/core/accelerator.h
)
target_include_directories(test_engine PRIVATE
    src/core ${LA_INCLUDE_DIRS} ${ZLIB_INCLUDE_DIRS} ${SZ_INCLUDE_DIRS})
target_link_libraries(test_engine PRIVATE
    Qt6::Core Qt6::Concurrent
    ${LA_LINK_LIBS} ${ZLIB_LIBRARIES})
if(YASUO_LIBARCHIVE)
    target_compile_definitions(test_engine PRIVATE YASUO_HAVE_LIBARCHIVE)
endif()
if(YASUO_SEVENZIP)
    target_compile_definitions(test_engine PRIVATE YASUO_HAVE_SEVENZIP)
    target_link_libraries(test_engine PRIVATE ole32 oleaut32 uuid advapi32 user32)
endif()

if(WIN32)
    set_target_properties(YasuoCompressor PROPERTIES WIN32_EXECUTABLE ON)
endif()

include(CTest)
add_test(NAME engine_selftest COMMAND test_engine)

# 将 7z.dll 随构建产物部署到运行时目录（7-Zip 后端运行时加载）
if(YASUO_SEVENZIP AND EXISTS "${SZ_7Z_DLL}")
    add_custom_command(TARGET YasuoCompressor POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${SZ_7Z_DLL}"
                "$<TARGET_FILE_DIR:YasuoCompressor>/7z.dll"
        COMMENT "部署 7z.dll 到可执行文件目录")
    add_custom_command(TARGET test_engine POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${SZ_7Z_DLL}"
                "$<TARGET_FILE_DIR:test_engine>/7z.dll"
        COMMENT "部署 7z.dll 到测试程序目录")
endif()
