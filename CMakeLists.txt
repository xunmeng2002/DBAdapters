cmake_minimum_required(VERSION 3.25)
include(submodules/CMakeCommon/CMakeCommon.cmake)
restore_vcpkg_root()
reuse_git_proxy_for_vcpkg()
set(CMAKE_TOOLCHAIN_FILE "$ENV{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake")
message(STATUS "ENV{VCPKG_ROOT} = $ENV{VCPKG_ROOT}")
message(STATUS "ENV{VCPKG_DOWNLOADS} = $ENV{VCPKG_DOWNLOADS}")
message(STATUS "ENV{VCPKG_DEFAULT_BINARY_CACHE} = $ENV{VCPKG_DEFAULT_BINARY_CACHE}")
message(STATUS "CMAKE_TOOLCHAIN_FILE = ${CMAKE_TOOLCHAIN_FILE}")
SET(PROJECT_NAME "DbAdapters")
project(${PROJECT_NAME} VERSION 1.0.0 LANGUAGES CXX)
include(GNUInstallDirs)
include(GenerateExportHeader)


set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_DEBUG_POSTFIX "d")
set(CMAKE_VERBOSE_MAKEFILE OFF)
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
set(CMAKE_CXX_VISIBILITY_PRESET hidden)
set(CMAKE_VISIBILITY_INLINES_HIDDEN ON)
if(LINUX)
    set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} -Wl,--exclude-libs,ALL")
endif()
add_template_library(DbInterface		${CMAKE_CURRENT_SOURCE_DIR}/include)
add_shared_module(AsyncDbWriter			${CMAKE_CURRENT_SOURCE_DIR}/include ${CMAKE_CURRENT_SOURCE_DIR}/src/DbAdapters/AsyncDbWriter)
add_shared_module(MysqlWrapper			${CMAKE_CURRENT_SOURCE_DIR}/include ${CMAKE_CURRENT_SOURCE_DIR}/src/DbAdapters/MysqlWrapper)
add_shared_module(MariadbWrapper		${CMAKE_CURRENT_SOURCE_DIR}/include ${CMAKE_CURRENT_SOURCE_DIR}/src/DbAdapters/MariadbWrapper)
add_shared_module(DuckdbWrapper			${CMAKE_CURRENT_SOURCE_DIR}/include ${CMAKE_CURRENT_SOURCE_DIR}/src/DbAdapters/DuckdbWrapper)
add_shared_module(SqliteWrapper			${CMAKE_CURRENT_SOURCE_DIR}/include ${CMAKE_CURRENT_SOURCE_DIR}/src/DbAdapters/SqliteWrapper)

target_include_directories(MysqlWrapper		PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/DbAdapters/Common)
target_include_directories(MariadbWrapper	PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/DbAdapters/Common)
target_include_directories(DuckdbWrapper	PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/DbAdapters/Common)
target_include_directories(SqliteWrapper	PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/DbAdapters/Common)

add_static_md_module(BackendLoader		${CMAKE_CURRENT_SOURCE_DIR}/include ${CMAKE_CURRENT_SOURCE_DIR}/src/DbAdapters/BackendLoader)

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


target_link_libraries(AsyncDbWriter PRIVATE Spark::Core)
target_link_libraries(BackendLoaderStatic PUBLIC DbInterface PRIVATE Spark::Core)
target_compile_definitions(BackendLoaderStatic PUBLIC
    BACKENDLOADER_FILENAME_DEBUG_POSTFIX="$<$<CONFIG:Debug>:${CMAKE_DEBUG_POSTFIX}>")
target_link_libraries(DuckdbWrapper PUBLIC DbInterface PRIVATE Spark::Core duckdb::duckdb)
target_link_libraries(SqliteWrapper PUBLIC DbInterface PRIVATE Spark::Core unofficial::sqlite3::sqlite3)
target_link_libraries(MysqlWrapper PUBLIC DbInterface PRIVATE Spark::Core unofficial::mysql-connector-cpp::connector protobuf::libprotobuf LZ4::lz4 zstd::libzstd)

get_target_property(_mysql_connector_library_kind unofficial::mysql-connector-cpp::connector TYPE)
if(_mysql_connector_library_kind STREQUAL "STATIC_LIBRARY")
    find_package(OpenSSL REQUIRED)
    find_package(ZLIB REQUIRED)
    target_link_libraries(MysqlWrapper PRIVATE OpenSSL::SSL OpenSSL::Crypto ZLIB::ZLIB)
    if(NOT WIN32 AND NOT CMAKE_SYSTEM_NAME MATCHES "FreeBSD")
        target_link_libraries(MysqlWrapper PRIVATE resolv)
    endif()
endif()


target_link_libraries(MariadbWrapper PUBLIC DbInterface PRIVATE Spark::Core unofficial::mariadb-connector-cpp::mariadbcpp)

target_compile_definitions(MariadbWrapper PRIVATE
  MARIADB_PLUGIN_DIR="${CMAKE_BINARY_DIR}/vcpkg_installed/${VCPKG_TARGET_TRIPLET}/$<IF:$<CONFIG:Debug>,debug/,>plugins/libmariadb"
)

if(LINUX)
    get_target_property(_duckdb_runtime_file duckdb::duckdb IMPORTED_LOCATION)
    set_target_properties(DuckdbWrapper PROPERTIES INSTALL_RPATH "\$ORIGIN")
    install(FILES "${_duckdb_runtime_file}" DESTINATION ${CMAKE_INSTALL_LIBDIR})
endif()

option(BUILD_TESTS "Build test projects (requires ../Libs/doctest)" ON)
if(BUILD_TESTS)
    find_package(doctest CONFIG REQUIRED PATHS "../Libs/doctest")
    add_subdirectory(test)
endif()

string(TIMESTAMP BUILD_TIMESTAMP "%Y%m%d-%H%M%S")
message("Build timestamp is ${BUILD_TIMESTAMP}")

install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(TARGETS
    AsyncDbWriter DbInterface BackendLoaderStatic DuckdbWrapper SqliteWrapper MysqlWrapper MariadbWrapper
    EXPORT ${PROJECT_NAME}Targets
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

file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/${PROJECT_NAME}Config.cmake
    "include(CMakeFindDependencyMacro)\n"
    "find_dependency(Spark CONFIG)\n"
    "find_dependency(unofficial-sqlite3 CONFIG)\n"
    "find_dependency(unofficial-mysql-connector-cpp CONFIG)\n"
    "find_dependency(unofficial-mariadb-connector-cpp CONFIG)\n"
    "find_dependency(ZLIB)\n"
    "find_dependency(OpenSSL)\n"
    "include(\"\${CMAKE_CURRENT_LIST_DIR}/${PROJECT_NAME}Targets.cmake\")\n"
    "set(${PROJECT_NAME}_FOUND TRUE)\n"
)
install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/${PROJECT_NAME}Config.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/${PROJECT_NAME}ConfigVersion.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/${PROJECT_NAME}
)
