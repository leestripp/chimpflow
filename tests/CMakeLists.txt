# Unit tests always run under ctest; integration needs the live service
# (it SKIPs with exit 0 when the service or key is unavailable).
add_executable(unit_tests test_wav.cpp)
target_include_directories(unit_tests PRIVATE ${CMAKE_SOURCE_DIR}/demo/src)
target_compile_options(unit_tests PRIVATE -Wall -Wextra)
add_test(NAME unit COMMAND unit_tests)
set_tests_properties(unit PROPERTIES LABELS unit)

find_program(BASH_PROGRAM bash)
if(BASH_PROGRAM)
  add_test(NAME integration COMMAND ${BASH_PROGRAM} ${CMAKE_CURRENT_SOURCE_DIR}/integration.sh)
  set_tests_properties(integration PROPERTIES LABELS integration)
endif()
