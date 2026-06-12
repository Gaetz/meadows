add_executable(jobs-test JobsTest.cpp)
target_link_libraries(jobs-test PRIVATE meadows)
meadows_set_warnings(jobs-test)
add_test(NAME jobs COMMAND jobs-test)
