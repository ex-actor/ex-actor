function(ex_actor_test NAME)
  cmake_parse_arguments(TEST "" "TIMEOUT" "" ${ARGN})
  if(NOT DEFINED TEST_TIMEOUT)
    set(TEST_TIMEOUT 3)
  endif()
  add_executable(${NAME} ${NAME}.cc)
  target_link_libraries(${NAME} PRIVATE ex_actor GTest::gtest GTest::gtest_main
                                        GTest::gmock)
  add_test(NAME ${NAME} COMMAND $<TARGET_FILE:${NAME}>)
  set_tests_properties(${NAME} PROPERTIES TIMEOUT ${TEST_TIMEOUT})
endfunction()

ex_actor_test(basic_api_test)
ex_actor_test(util_test)
ex_actor_test(scheduler_test)
ex_actor_test(stress_test)
ex_actor_test(shuffle_merge_test TIMEOUT 10)
ex_actor_test(complex_api_test TIMEOUT 5)
ex_actor_test(distributed_test TIMEOUT 10)
ex_actor_test(network_test TIMEOUT 15)
ex_actor_test(skynet_test TIMEOUT 10)
ex_actor_test(logging_test)

find_package(Python3 REQUIRED COMPONENTS Interpreter)
# multi-process test
if(NOT WIN32)
  add_executable(multi_process_test multi_process_test.cc)
  target_link_libraries(multi_process_test PRIVATE ex_actor GTest::gtest
                                                   GTest::gmock)
  configure_file(${CMAKE_CURRENT_SOURCE_DIR}/multi_process_test.sh
                 ${CMAKE_CURRENT_BINARY_DIR}/multi_process_test.sh COPYONLY)
  add_test(NAME multi_process_test COMMAND multi_process_test.sh
                                           $<TARGET_FILE:multi_process_test>)
  set_tests_properties(multi_process_test PROPERTIES TIMEOUT 3)
endif()

# heartbeat_test
add_executable(heartbeat_test heartbeat_test.cc)
target_link_libraries(heartbeat_test PRIVATE ex_actor GTest::gtest GTest::gmock)
add_test(
  NAME heartbeat_test
  COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/heartbeat_test.py
          $<TARGET_FILE:heartbeat_test>)
set_tests_properties(heartbeat_test PROPERTIES TIMEOUT 10)

add_executable(dynamic_connectivity_test dynamic_connectivity_test.cc)
target_link_libraries(dynamic_connectivity_test PRIVATE ex_actor)
add_test(
  NAME dynamic_connectivity_star_test
  COMMAND
    ${Python3_EXECUTABLE}
    ${CMAKE_CURRENT_SOURCE_DIR}/dynamic_connectivity_test.py
    $<TARGET_FILE:dynamic_connectivity_test> star)
set_tests_properties(dynamic_connectivity_star_test PROPERTIES TIMEOUT 20)

add_test(
  NAME dynamic_connectivity_chain_test
  COMMAND
    ${Python3_EXECUTABLE}
    ${CMAKE_CURRENT_SOURCE_DIR}/dynamic_connectivity_test.py
    $<TARGET_FILE:dynamic_connectivity_test> chain)
set_tests_properties(dynamic_connectivity_chain_test PROPERTIES TIMEOUT 20)
