cmake_minimum_required(VERSION 3.16)
project(TinyServer
    VERSION 1.0.0
    DESCRIPTION "A lightweight C++20 HTTP server with Reactor pattern, MySQL, and JWT"
    LANGUAGES CXX
)

# ============================================================
# C++ Standard
# ============================================================
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ============================================================
# Platform check — Linux only
# ============================================================
if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR "${PROJECT_NAME} supports Linux only (detected: ${CMAKE_SYSTEM_NAME})")
endif()

# ============================================================
# Build options
# ============================================================
option(TINYSERVER_BUILD_TESTS "Build test executables" ON)

# ============================================================
# Dependencies
# ============================================================

# ---- Threads (pthread) ----
find_package(Threads REQUIRED)

# ---- Boost (Beast is header-only; JSON needs the library) ----
find_package(Boost REQUIRED COMPONENTS json)
if(NOT Boost_FOUND)
    # Fallback: if Boost is installed but json component wasn't built,
    # at least find the headers (Beast is header-only)
    find_package(Boost REQUIRED)
endif()

# ---- MySQL / MariaDB client ----
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
    pkg_check_modules(MYSQL mysqlclient)
endif()

if(NOT MYSQL_FOUND)
    find_path(MYSQL_INCLUDE_DIR
        NAMES mysql/mysql.h
        PATHS /usr/include /usr/local/include
    )
    find_library(MYSQL_LIBRARY
        NAMES mysqlclient mariadb
        PATHS /usr/lib /usr/local/lib /usr/lib64
    )
    if(MYSQL_INCLUDE_DIR AND MYSQL_LIBRARY)
        set(MYSQL_FOUND TRUE)
    endif()
endif()

if(NOT MYSQL_FOUND)
    message(FATAL_ERROR "mysqlclient (MariaDB / MySQL) not found. "
        "Install libmysqlclient-dev or mariadb-connector-c-dev.")
endif()

# ---- JWT (libjwt) ----
if(PkgConfig_FOUND)
    pkg_check_modules(JWT libjwt)
endif()

if(NOT JWT_FOUND)
    find_path(JWT_INCLUDE_DIR
        NAMES jwt.h
        PATHS /usr/include /usr/local/include
    )
    find_library(JWT_LIBRARY
        NAMES jwt
        PATHS /usr/lib /usr/local/lib /usr/lib64
    )
    if(JWT_INCLUDE_DIR AND JWT_LIBRARY)
        set(JWT_FOUND TRUE)
    endif()
endif()

if(NOT JWT_FOUND)
    message(FATAL_ERROR "libjwt not found. Install libjwt-dev or libjwt-gnutls-dev.")
endif()

# ============================================================
# Sources
# ============================================================
set(SOURCES
    main.cpp
    server.cpp
    config.cpp
    acceptor.cpp
    reactor.cpp
    connectionpool.cpp
    router.cpp
    http.cpp
    log.cpp
    service.cpp
    threadpool.cpp
    redis_store.cpp
)

set(PUBLIC_HEADERS
    server.hpp
    config.hpp
    acceptor.hpp
    reactor.hpp
    connectionpool.hpp
    router.hpp
    http.hpp
    log.hpp
    service.hpp
    threadpool.hpp
    thread.hpp
    connection.hpp
    threadsafe_queue.hpp
    mutex.hpp
    alloc.hpp
    error.hpp
    redis_store.hpp
)

# ============================================================
# Main executable
# ============================================================
add_executable(${PROJECT_NAME} ${SOURCES})

target_include_directories(${PROJECT_NAME} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${Boost_INCLUDE_DIRS}
    ${MYSQL_INCLUDE_DIR}
    ${JWT_INCLUDE_DIR}
)

target_link_libraries(${PROJECT_NAME} PRIVATE
    Threads::Threads
    ${Boost_LIBRARIES}
    ${MYSQL_LIBRARY}
    ${JWT_LIBRARY}
)

# If link_directories was populated by pkg-config, apply them
if(MYSQL_LIBRARY_DIRS)
    target_link_directories(${PROJECT_NAME} PRIVATE ${MYSQL_LIBRARY_DIRS})
endif()
if(JWT_LIBRARY_DIRS)
    target_link_directories(${PROJECT_NAME} PRIVATE ${JWT_LIBRARY_DIRS})
endif()

# Debug preprocessor definitions
target_compile_definitions(${PROJECT_NAME} PRIVATE
    $<$<CONFIG:Debug>:_DEBUG>
)

# ============================================================
# Redis（默认依赖）：redis-plus-plus（动态库 libredis++）+ hiredis
# - hiredis：发行版通常可直接安装（Ubuntu: sudo apt install libhiredis-dev）
# - redis-plus-plus：无官方发行版 release，需源码自行编译安装（产物 libredis++.so）
# 运行期连接不上会自动降级；构建期两者必需（找不到即报错并给出指引）
# ============================================================
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
    pkg_check_modules(HIREDIS QUIET hiredis)
    pkg_check_modules(REDISXX QUIET redis++)
endif()
if(NOT HIREDIS_FOUND)
    find_path(HIREDIS_INCLUDE_DIR hiredis/hiredis.h PATHS /usr/include /usr/local/include)
    find_library(HIREDIS_LIBRARY NAMES hiredis PATHS /usr/lib /usr/local/lib /usr/lib64)
    if(HIREDIS_INCLUDE_DIR AND HIREDIS_LIBRARY)
        set(HIREDIS_FOUND TRUE)
    endif()
endif()
if(NOT HIREDIS_FOUND)
    message(FATAL_ERROR "hiredis not found. Install it, e.g. Ubuntu: sudo apt install libhiredis-dev")
endif()

# redis-plus-plus：头文件 + 动态库（redis++ 为纯第三方源码项目，无发行版包）
find_path(REDISXX_INCLUDE_DIR sw/redis++/redis++.h PATHS /usr/include /usr/local/include)
if(NOT REDISXX_FOUND)
    find_library(REDISXX_LIBRARY NAMES redis++ PATHS /usr/lib /usr/local/lib /usr/lib64 /usr/local/lib64)
    if(REDISXX_LIBRARY)
        set(REDISXX_FOUND TRUE)
    endif()
endif()
if(NOT REDISXX_FOUND OR NOT REDISXX_INCLUDE_DIR)
    message(FATAL_ERROR "redis-plus-plus not found. It has no distro release; build it yourself:
    git clone https://github.com/sewenew/redis-plus-plus && cd redis-plus-plus
    mkdir -p build && cd build
    cmake .. -DCMAKE_BUILD_TYPE=Release -DREDIS_PLUS_PLUS_CXX_STANDARD=20 -DREDIS_PLUS_PLUS_BUILD_TEST=OFF
    make -j && sudo make install
  (产物 libredis++.so 与头文件默认装到 /usr/local)")
endif()

target_include_directories(${PROJECT_NAME} PRIVATE ${HIREDIS_INCLUDE_DIR} ${REDISXX_INCLUDE_DIR})
# libredis++ 依赖 hiredis；两者都显式链接
target_link_libraries(${PROJECT_NAME} PRIVATE ${REDISXX_LIBRARY} ${HIREDIS_LIBRARY})
if(HIREDIS_LIBRARY_DIRS)
    target_link_directories(${PROJECT_NAME} PRIVATE ${HIREDIS_LIBRARY_DIRS})
endif()
if(REDISXX_LIBRARY_DIRS)
    target_link_directories(${PROJECT_NAME} PRIVATE ${REDISXX_LIBRARY_DIRS})
endif()
message(STATUS "Redis: enabled (libredis++ = ${REDISXX_LIBRARY}, hiredis = ${HIREDIS_LIBRARY})")


# ============================================================
# Test executables
# ============================================================
if(TINYSERVER_BUILD_TESTS)
    enable_testing()

    function(add_tiny_test test_name test_source)
        add_executable(${test_name} ${test_source})
        target_include_directories(${test_name} PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}
            ${Boost_INCLUDE_DIRS}
            ${MYSQL_INCLUDE_DIR}
            ${JWT_INCLUDE_DIR}
        )
        target_link_libraries(${test_name} PRIVATE
            Threads::Threads
            ${Boost_LIBRARIES}
            ${MYSQL_LIBRARY}
            ${JWT_LIBRARY}
        )
        target_compile_definitions(${test_name} PRIVATE
            $<$<CONFIG:Debug>:_DEBUG>
        )
        add_test(NAME ${test_name} COMMAND ${test_name})
    endfunction()

    add_tiny_test(test_compiler test_compiler.cpp)
    add_tiny_test(test_connsql  test_connsql.cpp)
    add_tiny_test(test_log      test_log.cpp)
    add_tiny_test(test_queue    test_queue.cpp)
endif()

# ============================================================
# Install
# ============================================================
install(TARGETS ${PROJECT_NAME} RUNTIME DESTINATION bin)
install(FILES ${PUBLIC_HEADERS} DESTINATION include/TinyServer)

# ============================================================
# Print summary
# ============================================================
message(STATUS "--------------------------------------------------")
message(STATUS "  ${PROJECT_NAME} ${PROJECT_VERSION}")
message(STATUS "  C++ Standard: ${CMAKE_CXX_STANDARD}")
message(STATUS "  Boost:        ${Boost_VERSION}")
message(STATUS "  MySQL:        ${MYSQL_INCLUDE_DIR}")
message(STATUS "  JWT:          ${JWT_INCLUDE_DIR}")
message(STATUS "  Tests:        ${TINYSERVER_BUILD_TESTS}")
message(STATUS "--------------------------------------------------")
