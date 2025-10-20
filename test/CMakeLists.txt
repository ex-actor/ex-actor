CPMAddPackage(
  NAME gtest
  GITHUB_REPOSITORY google/googletest
  GIT_TAG v1.17.0
  OPTIONS
      "INSTALL_GTEST OFF"
      "gtest_force_shared_crt ON"
)


function(ex_actor_test NAME)
  add_executable(${NAME} ${NAME}.cc)
  target_link_libraries(${NAME} PRIVATE ex_actor GTest::gtest_main GTest::gmock)
  add_test(NAME ${NAME} COMMAND $<TARGET_FILE:${NAME}>)
  set_tests_properties(${NAME} PROPERTIES TIMEOUT 3)
endfunction()

ex_actor_test(basic_api_test)
ex_actor_test(util_test)
ex_actor_test(scheduler_test)
ex_actor_test(stress_test)
ex_actor_test(distributed_test)
ex_actor_test(network_test)
