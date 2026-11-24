cmake_minimum_required(VERSION 3.21)
project(npu_broadcast LANGUAGES C CXX)

set(RNNOISE_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
get_filename_component(stage "${CMAKE_CURRENT_SOURCE_DIR}/../.phase1" ABSOLUTE)
add_subdirectory("${stage}/rnnoise" "${CMAKE_CURRENT_BINARY_DIR}/rnnoise")

add_executable(npu-broadcast main.cpp)
target_compile_features(npu-broadcast PRIVATE cxx_std_17)
target_compile_definitions(npu-broadcast PRIVATE UNICODE _UNICODE)
target_include_directories(npu-broadcast PRIVATE "${stage}/rnnoise/src")
target_link_libraries(npu-broadcast PRIVATE rnnoise ole32 avrt)
set(app_output "${CMAKE_CURRENT_BINARY_DIR}/$<CONFIG>")
add_custom_target(npu-broadcast-assets
    COMMAND ${CMAKE_COMMAND} -E make_directory "${app_output}"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${RNNOISE_WINML_PACKAGE_ROOT}/runtimes/win-x64/native/Microsoft.Windows.AI.MachineLearning.dll"
        "${RNNOISE_WINML_PACKAGE_ROOT}/runtimes/win-x64/native/onnxruntime.dll"
        "${RNNOISE_WINML_PACKAGE_ROOT}/runtimes/win-x64/native/DirectML.dll"
        "${app_output}"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${stage}/models/rnnoise_split" "${app_output}/models"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${stage}/rnnoise/COPYING" "${app_output}/RNNoise-COPYING.txt"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${RNNOISE_WINML_PACKAGE_ROOT}/license.txt" "${app_output}/WinML-license.txt"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${RNNOISE_WINML_PACKAGE_ROOT}/ThirdPartyNotices.txt" "${app_output}/WinML-ThirdPartyNotices.txt"
    VERBATIM
)
add_dependencies(npu-broadcast npu-broadcast-assets)

if(EXISTS "${stage}/models/room_echo_split/room_echo_host.onnx")
    add_custom_target(npu-broadcast-room-echo-assets
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${stage}/models/room_echo_split" "${app_output}/models/room-echo"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${stage}/localvqe/LICENSE" "${app_output}/LocalVQE-LICENSE.txt"
        VERBATIM
    )
    add_dependencies(npu-broadcast npu-broadcast-room-echo-assets)
endif()
