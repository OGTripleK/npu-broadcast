cmake_minimum_required(VERSION 3.20)
project(NpuBroadcastMicProbe LANGUAGES CXX)

add_executable(npu-mic-probe probe.cpp)
target_compile_features(npu-mic-probe PRIVATE cxx_std_17)
target_compile_definitions(npu-mic-probe PRIVATE UNICODE _UNICODE)
target_link_libraries(npu-mic-probe PRIVATE ole32)
