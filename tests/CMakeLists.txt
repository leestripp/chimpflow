# Unit tests always run under ctest; integration needs the live service
# (it SKIPs with exit 0 when the service or key is unavailable).
add_executable(unit_tests test_wav.cpp)
target_include_directories(unit_tests PRIVATE ${CMAKE_SOURCE_DIR}/demo/src)
target_compile_options(unit_tests PRIVATE -Wall -Wextra)
add_test(NAME unit COMMAND unit_tests)
set_tests_properties(unit PROPERTIES LABELS unit)

add_executable(unit_audio_format test_audio_format.cpp ../src/audio_format.cpp)
target_include_directories(unit_audio_format PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_compile_options(unit_audio_format PRIVATE -Wall -Wextra)
add_test(NAME unit_audio_format COMMAND unit_audio_format)

add_executable(unit_text_chunk test_text_chunk.cpp ../src/text_chunk.cpp)
target_include_directories(unit_text_chunk PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_compile_options(unit_text_chunk PRIVATE -Wall -Wextra)
add_test(NAME unit_text_chunk COMMAND unit_text_chunk)
set_tests_properties(unit_audio_format unit_text_chunk PROPERTIES LABELS unit)

find_program(BASH_PROGRAM bash)
if(BASH_PROGRAM)
  add_test(NAME integration COMMAND ${BASH_PROGRAM} ${CMAKE_CURRENT_SOURCE_DIR}/integration.sh)
  set_tests_properties(integration PROPERTIES LABELS integration)
endif()
