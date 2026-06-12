add_executable(cooker cooker/Main.cpp)
target_link_libraries(cooker PRIVATE meadows-data)
meadows_set_warnings(cooker)
