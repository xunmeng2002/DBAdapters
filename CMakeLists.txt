cmake_minimum_required(VERSION 3.25)
include(submodules/CMakeCommon/CMakeCommon.cmake)
restore_vcpkg_root()
reuse_git_proxy_for_vcpkg()
set(CMAKE_TOOLCHAIN_FILE "$ENV{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake")
message(STATUS "ENV{VCPKG_ROOT} = $ENV{VCPKG_ROOT}")
message(STATUS "ENV{VCPKG_DOWNLOADS} = $ENV{VCPKG_DOWNLOADS}")
message(STATUS "ENV{VCPKG_DEFAULT_BINARY_CACHE} = $ENV{VCPKG_DEFAULT_BINARY_CACHE}")
message(STATUS "CMAKE_TOOLCHAIN_FILE = ${CMAKE_TOOLCHAIN_FILE}")
SET(PROJECT_NAME "DBAdapters")
project(${PROJECT_NAME} VERSION 1.0.0 LANGUAGES CXX)
include(GNUInstallDirs)
include(GenerateExportHeader)


set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_DEBUG_POSTFIX "d")
set(CMAKE_VERBOSE_MAKEFILE OFF)
# 平台/配置相关编译选项：MSVC 的 /EHsc 及各配置优化级别、Linux 的 -g/-O 系、
# NDEBUG/_DEBUG 均为 CMake+编译器默认行为，不再显式重复；仅保留净增量项
if(WIN32)
    add_compile_definitions(UNICODE _UNICODE _CRT_SECURE_NO_WARNINGS _SILENCE_CXX17_CODECVT_HEADER_DEPRECATION_WARNING)
    add_compile_options(/utf-8 /bigobj)
endif()
message("CMAKE_BUILD_TYPE:" ${CMAKE_BUILD_TYPE})
message("CMAKE_SYSTEM_NAME:" ${CMAKE_SYSTEM_NAME})
message("CMAKE_CL_64:" ${CMAKE_CL_64})
message("CMAKE_CXX_FLAGS:" ${CMAKE_CXX_FLAGS})
message("CMAKE_CXX_FLAGS_RELEASE:" ${CMAKE_CXX_FLAGS_RELEASE})
message("CMAKE_CXX_FLAGS_DEBUG:" ${CMAKE_CXX_FLAGS_DEBUG})


set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/bin/$<CONFIG>)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/lib/$<CONFIG>)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/lib/$<CONFIG>)
 
message(STATUS "VCPKG_TARGET_TRIPLET = ${VCPKG_TARGET_TRIPLET}")
message(STATUS "CMAKE_PREFIX_PATH = ${CMAKE_PREFIX_PATH}")
add_template_library(DBInterface		${CMAKE_CURRENT_SOURCE_DIR}/include)
add_shared_module(AsyncDBWriter			${CMAKE_CURRENT_SOURCE_DIR}/include ${CMAKE_CURRENT_SOURCE_DIR}/src/DBAdapters/AsyncDBWriter)
add_shared_module(MysqlWrapper			${CMAKE_CURRENT_SOURCE_DIR}/include ${CMAKE_CURRENT_SOURCE_DIR}/src/DBAdapters/MysqlWrapper)
add_shared_module(MariadbWrapper		${CMAKE_CURRENT_SOURCE_DIR}/include ${CMAKE_CURRENT_SOURCE_DIR}/src/DBAdapters/MariadbWrapper)
add_shared_module(DuckdbWrapper			${CMAKE_CURRENT_SOURCE_DIR}/include ${CMAKE_CURRENT_SOURCE_DIR}/src/DBAdapters/DuckdbWrapper)
add_shared_module(SqliteWrapper			${CMAKE_CURRENT_SOURCE_DIR}/include ${CMAKE_CURRENT_SOURCE_DIR}/src/DBAdapters/SqliteWrapper)

if(WIN32)
    find_package(Spark REQUIRED PATHS "../Libs/Spark/x64-windows")
    find_package(duckdb CONFIG REQUIRED PATHS "../Libs/duckdb/x64-windows")
else()
    find_package(Spark REQUIRED PATHS "../Libs/Spark/x64-linux")
    find_package(duckdb CONFIG REQUIRED PATHS "../Libs/duckdb/x64-linux")
endif()
find_package(unofficial-sqlite3 CONFIG REQUIRED)
find_package(unofficial-mysql-connector-cpp CONFIG REQUIRED)
find_package(unofficial-mariadb-connector-cpp CONFIG REQUIRED)
find_package(protobuf CONFIG REQUIRED)
find_package(lz4 CONFIG REQUIRED)
find_package(zstd CONFIG REQUIRED)


# 各 wrapper 的后端错误（连接失败/语句失败）经 FailureLogThrottle 走 Spark 日志上报，故统一私有链接 Spark::Core
target_link_libraries(AsyncDBWriter PRIVATE Spark::Core)
target_link_libraries(DuckdbWrapper PUBLIC DBInterface PRIVATE Spark::Core duckdb::duckdb)
target_link_libraries(SqliteWrapper PUBLIC DBInterface PRIVATE Spark::Core unofficial::sqlite3::sqlite3)
target_link_libraries(MysqlWrapper PUBLIC DBInterface PRIVATE Spark::Core unofficial::mysql-connector-cpp::connector protobuf::libprotobuf LZ4::lz4 zstd::libzstd)


target_link_libraries(MariadbWrapper PUBLIC DBInterface PRIVATE Spark::Core unofficial::mariadb-connector-cpp::mariadbcpp)

# libmariadb 运行时动态加载认证插件（非 PE 依赖，无法通过 vcpkg applocal 自动部署）
# 编译时注入插件路径，Connect() 内设环境变量 MARIADB_PLUGIN_DIR 告知 libmariadb
target_compile_definitions(MariadbWrapper PRIVATE
  MARIADB_PLUGIN_DIR="${CMAKE_BINARY_DIR}/vcpkg_installed/${VCPKG_TARGET_TRIPLET}/$<IF:$<CONFIG:Debug>,debug/,>plugins/libmariadb"
)

# Linux: install() 会剥掉 build 时注入的 RPATH，安装版 libDuckdbWrapper.so 因此找不到
# libduckdb.so（IMPORTED_LINK_DEPENDENT_LIBRARIES 不会把 duckdb 目录传给消费方）。
# 方案：INSTALL_RPATH 用 $ORIGIN（相对自身目录），并把 libduckdb.so 一并装入安装包，
# 使安装树自包含，可整体搬移到任意 Linux 机器。
if(LINUX)
    get_target_property(_duckdb_runtime_file duckdb::duckdb IMPORTED_LOCATION)
    set_target_properties(DuckdbWrapper PROPERTIES INSTALL_RPATH "\$ORIGIN")
    install(FILES "${_duckdb_runtime_file}" DESTINATION ${CMAKE_INSTALL_LIBDIR})
endif()

option(BUILD_TESTS "Build test projects" ON)
if(BUILD_TESTS)
    add_subdirectory(test)
endif()

string(TIMESTAMP BUILD_TIMESTAMP "%Y%m%d-%H%M%S")
message("Build timestamp is ${BUILD_TIMESTAMP}")

install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(TARGETS
    AsyncDBWriter DBInterface DuckdbWrapper SqliteWrapper MysqlWrapper MariadbWrapper
    EXPORT ${PROJECT_NAME}Targets
    #CONFIGURATIONS Release
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

install(EXPORT ${PROJECT_NAME}Targets
    NAMESPACE ${PROJECT_NAME}::
    FILE ${PROJECT_NAME}Targets.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/${PROJECT_NAME}
)

include(CMakePackageConfigHelpers)
write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/${PROJECT_NAME}ConfigVersion.cmake
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion
)

# 生成 DBAdaptersConfig.cmake
# find_dependency 使 vcpkg 注册这些包，applocal 才能自动拷贝 DLL
file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/${PROJECT_NAME}Config.cmake
    "include(CMakeFindDependencyMacro)\n"
    "find_dependency(unofficial-sqlite3 CONFIG)\n"
    "find_dependency(unofficial-mysql-connector-cpp CONFIG)\n"
    "find_dependency(unofficial-mariadb-connector-cpp CONFIG)\n"
    "find_dependency(ZLIB)\n"
    "include(\"\${CMAKE_CURRENT_LIST_DIR}/${PROJECT_NAME}Targets.cmake\")\n"
    "set(${PROJECT_NAME}_FOUND TRUE)\n"
)
install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/${PROJECT_NAME}Config.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/${PROJECT_NAME}ConfigVersion.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/${PROJECT_NAME}
)
