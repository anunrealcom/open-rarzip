cmake_minimum_required(VERSION 3.16)
project(YasuoCompressor LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

find_package(Qt6 REQUIRED COMPONENTS Core Widgets)
qt_standard_project_setup()

# 优先使用 libarchive（全格式：zip/7z/tar/gz/bz2/xz/zst...）；
# 若未安装（无网络环境），回退到本地 zlib 引擎，仅支持 ZIP / GZ。
set(ENGINE_SRC src/zipengine.cpp)
set(EXTRA_LIB "")
set(EXTRA_INC "")
find_package(LibArchive QUIET)
if(LibArchive_FOUND)
    message(STATUS "使用 libarchive 全格式引擎")
    set(ENGINE_SRC src/archiveengine.cpp)
    set(EXTRA_LIB LibArchive::LibArchive)
else()
    message(STATUS "使用本地 zlib 引擎（ZIP / GZ）")
    set(ZLIB_INC C:/Qt/Tools/mingw1310_64/x86_64-w64-mingw32/include)
    set(ZLIB_LIB C:/Qt/Tools/mingw1310_64/x86_64-w64-mingw32/lib/libz.a)
    set(EXTRA_INC ${ZLIB_INC})
    set(EXTRA_LIB ${ZLIB_LIB})
endif()

add_executable(YasuoCompressor
    src/main.cpp
    src/mainwindow.h
    src/mainwindow.cpp
    src/archiveengine.h
    src/archiveworker.h
    src/archiveworker.cpp
    ${ENGINE_SRC}
)

target_link_libraries(YasuoCompressor PRIVATE
    Qt6::Core
    Qt6::Widgets
    Qt6::EntryPointPrivate
    ${EXTRA_LIB}
)

target_include_directories(YasuoCompressor PRIVATE src ${EXTRA_INC})

if(WIN32)
    set_target_properties(YasuoCompressor PROPERTIES WIN32_EXECUTABLE ON)
endif()

# 无界面自测：验证压缩/解压逻辑，并用系统 tar / PowerShell 交叉验证归档兼容性
add_executable(test_engine test_engine.cpp ${ENGINE_SRC})
target_include_directories(test_engine PRIVATE src ${EXTRA_INC})
target_link_libraries(test_engine PRIVATE Qt6::Core ${EXTRA_LIB})
