
add_application(TestDB         ${CMAKE_CURRENT_SOURCE_DIR}/TestDB ${CMAKE_CURRENT_SOURCE_DIR}/TestDB)
target_link_libraries(TestDB PRIVATE PersonalLib::Core SqliteWrapper DuckdbWrapper MysqlWrapper MariadbWrapper)
copy_dll_file(TestDB)
