
add_application(TestDB         ${CMAKE_CURRENT_SOURCE_DIR}/TestDB ${CMAKE_CURRENT_SOURCE_DIR}/TestDB)
target_link_libraries(TestDB PRIVATE Spark::Core BackendLoaderStatic DuckdbWrapper SqliteWrapper MysqlWrapper MariadbWrapper)
if(WIN32)
    copy_dll_file(TestDB)
endif()
