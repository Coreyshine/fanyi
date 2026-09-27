cmake_minimum_required(VERSION 3.16)
project(fanyi VERSION 1.0.0 LANGUAGES C CXX)

if(APPLE)
    enable_language(OBJC)
    enable_language(OBJCXX)
endif()

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# ---- llama.cpp (vendored, pinned) ----
set(LLAMA_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(LLAMA_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
set(LLAMA_BUILD_TOOLS    OFF CACHE BOOL "" FORCE)
set(LLAMA_BUILD_SERVER   OFF CACHE BOOL "" FORCE)
set(LLAMA_CURL           OFF CACHE BOOL "" FORCE)
# embed the Metal shader library so the binary is self-contained on macOS
set(GGML_METAL_EMBED_LIBRARY ON CACHE BOOL "" FORCE)
add_subdirectory(third_party/llama.cpp)

# ---- vendored single-file deps ----
add_library(cjson STATIC third_party/cJSON/cJSON.c)
target_include_directories(cjson PUBLIC third_party/cJSON)

# cpp-httplib（llama.cpp vendored，编译库布局）
add_library(httplib STATIC third_party/llama.cpp/vendor/cpp-httplib/httplib.cpp)
target_include_directories(httplib PUBLIC third_party/llama.cpp/vendor/cpp-httplib)
find_package(Threads REQUIRED)
target_link_libraries(httplib PUBLIC Threads::Threads)


# ---- fanyi core (C) ----
add_library(fanyi_core STATIC
    server/src/translator.c
    server/src/config.c
)
target_include_directories(fanyi_core PUBLIC server/include)
target_link_libraries(fanyi_core PUBLIC llama cjson)

# ---- fanyi-cli: translation test tool ----
add_executable(fanyi-cli server/src/cli.c)
target_link_libraries(fanyi-cli PRIVATE fanyi_core)

# ---- fanyi-server ----
if(WIN32)
    set(FANYI_TRAY_SRC server/src/tray_win.cpp)
elseif(APPLE)
    set(FANYI_TRAY_SRC server/src/tray_mac.mm)
else()
    option(FANYI_TRAY "Build Linux tray icon (needs libayatana-appindicator)" OFF)
    if(FANYI_TRAY)
        set(FANYI_TRAY_SRC server/src/tray_linux.cpp)
    else()
        set(FANYI_TRAY_SRC server/src/tray_headless.cpp)
    endif()
endif()

add_executable(fanyi-server
    server/src/main.cpp
    server/src/http_server.cpp
    server/src/model_download.cpp
    ${FANYI_TRAY_SRC}
)
target_include_directories(fanyi-server PRIVATE server/src)
target_link_libraries(fanyi-server PRIVATE fanyi_core httplib)

if(WIN32)
    target_link_libraries(fanyi-server PRIVATE ws2_32)
elseif(APPLE)
    target_link_libraries(fanyi-server PRIVATE "-framework Foundation" "-framework AppKit")
else()
    find_package(Threads REQUIRED)
    target_link_libraries(fanyi-server PRIVATE Threads::Threads dl)
    if(FANYI_TRAY)
        target_compile_definitions(fanyi-server PRIVATE FANYI_HAS_TRAY)
        find_package(PkgConfig REQUIRED)
        pkg_check_modules(AYATANA REQUIRED libayatana-appindicator3)
        target_include_directories(fanyi-server PRIVATE ${AYATANA_INCLUDE_DIRS})
        target_link_libraries(fanyi-server PRIVATE ${AYATANA_LIBRARIES})
    endif()
endif()

# settings page next to the binary
add_custom_command(TARGET fanyi-server POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${CMAKE_CURRENT_SOURCE_DIR}/server/assets/settings.html
        $<TARGET_FILE_DIR:fanyi-server>/settings.html
)
