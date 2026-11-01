cmake_minimum_required(VERSION 3.24)
project(acv_fsr31_proxy LANGUAGES CXX ASM_MASM)

add_library(dxgi_proxy SHARED
    src/dllmain_full.cpp
    src/dxgi_forwarders.asm
    src/dxgi_proxy_full.def
)

target_compile_features(dxgi_proxy PRIVATE cxx_std_20)
target_compile_definitions(dxgi_proxy PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
set_property(TARGET dxgi_proxy PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

set_target_properties(dxgi_proxy PROPERTIES
    OUTPUT_NAME "dxgi"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

# Experimental resource-allocation probe. This DLL previously prevented
# Valhalla from launching when installed, so do not even build it by default.
option(ACV_BUILD_D3D12_PROBE "Build the experimental D3D12 probe (not for installation)" OFF)
if(ACV_BUILD_D3D12_PROBE)
    add_library(d3d12_probe SHARED
        src/d3d12_probe.cpp
        src/d3d12_forwarders.asm
        src/d3d12_proxy_full.def
    )

    target_compile_features(d3d12_probe PRIVATE cxx_std_20)
    # FFX context test: headers only; the loader DLL is loaded at runtime.
    target_include_directories(d3d12_probe PRIVATE
        "${CMAKE_SOURCE_DIR}/third_party/FidelityFX-SDK/Kits/FidelityFX/api/include"
        "${CMAKE_SOURCE_DIR}/third_party/FidelityFX-SDK/Kits/FidelityFX/upscalers/include"
    )
    target_compile_definitions(d3d12_probe PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    set_property(TARGET d3d12_probe PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

    set_target_properties(d3d12_probe PROPERTIES
        OUTPUT_NAME "d3d12"
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/probe/bin"
    )
endif()

# The installable FSR 4 mod: same source as the probe, built with ACV_FSR4_RELEASE=1 (investigation features inert,
# FFX debug checking off, own small log). install_fsr4.ps1 builds this target.
option(ACV_BUILD_FSR4_RELEASE "Build the FSR 4 mod release DLL" OFF)
if(ACV_BUILD_FSR4_RELEASE)
    add_library(acv_fsr4 SHARED
        src/d3d12_probe.cpp
        src/d3d12_forwarders.asm
        src/d3d12_proxy_full.def
    )

    target_compile_features(acv_fsr4 PRIVATE cxx_std_20)
    target_compile_definitions(acv_fsr4 PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX ACV_FSR4_RELEASE=1)
    target_include_directories(acv_fsr4 PRIVATE
        "${CMAKE_SOURCE_DIR}/third_party/FidelityFX-SDK/Kits/FidelityFX/api/include"
        "${CMAKE_SOURCE_DIR}/third_party/FidelityFX-SDK/Kits/FidelityFX/upscalers/include"
    )
    set_property(TARGET acv_fsr4 PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

    set_target_properties(acv_fsr4 PROPERTIES
        OUTPUT_NAME "d3d12"
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/release/bin"
    )
endif()
