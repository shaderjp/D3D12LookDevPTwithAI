cmake_minimum_required(VERSION 3.20)

project(D3D12LookDevPTRtxdiRuntime LANGUAGES CXX)

if(NOT DEFINED D3D12LOOKDEVPT_RTXDI_RUNTIME_ROOT)
    message(FATAL_ERROR "D3D12LOOKDEVPT_RTXDI_RUNTIME_ROOT must point to Libraries/Rtxdi.")
endif()

get_filename_component(
    D3D12LOOKDEVPT_RTXDI_RUNTIME_ROOT
    "${D3D12LOOKDEVPT_RTXDI_RUNTIME_ROOT}"
    ABSOLUTE)

if(NOT EXISTS "${D3D12LOOKDEVPT_RTXDI_RUNTIME_ROOT}/CMakeLists.txt")
    message(FATAL_ERROR
        "RTXDI runtime CMakeLists.txt was not found under "
        "${D3D12LOOKDEVPT_RTXDI_RUNTIME_ROOT}.")
endif()

# RTXDI's Libraries/Rtxdi file is an add_subdirectory fragment, not a
# standalone CMake project. Keep the pinned submodule untouched and provide
# the project()/cmake_minimum_required() context from this repository.
add_subdirectory("${D3D12LOOKDEVPT_RTXDI_RUNTIME_ROOT}" RtxdiRuntime)
