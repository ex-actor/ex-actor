add_executable(basic_api_test basic_api_test.cc)
target_link_libraries(basic_api_test ex_actor)
add_test(NAME basic_api_test COMMAND $<TARGET_FILE:basic_api_test>)