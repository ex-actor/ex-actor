CPMAddPackage(
  NAME gtest
  GITHUB_REPOSITORY google/googletest
  GIT_TAG v1.17.0
  VERSION 1.17.0
  OPTIONS
      "INSTALL_GTEST OFF"
      "gtest_force_shared_crt ON"
)


add_executable(basic_api_test basic_api_test.cc)
target_link_libraries(basic_api_test PRIVATE ex_actor gtest_main)
add_test(NAME basic_api_test COMMAND $<TARGET_FILE:basic_api_test>)


add_executable(util_test util_test.cc)
target_link_libraries(util_test PRIVATE ex_actor gtest_main)
add_test(NAME util_test COMMAND $<TARGET_FILE:util_test>)
set_tests_properties(util_test PROPERTIES TIMEOUT 3)