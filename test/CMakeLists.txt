CPMAddPackage(
  NAME Catch2
  GITHUB_REPOSITORY catchorg/Catch2
  VERSION 3.10.0
)

function(ex_actor_test NAME)
  add_executable(${NAME} ${NAME}.cc)
  target_link_libraries(${NAME} PRIVATE ex_actor Catch2::Catch2WithMain)
  add_test(NAME ${NAME} COMMAND $<TARGET_FILE:${NAME}>)
  set_tests_properties(${NAME} PROPERTIES TIMEOUT 3)
endfunction()

ex_actor_test(basic_api_test)
ex_actor_test(util_test)
ex_actor_test(scheduler_test)