# Public production compilation plus executable synthetic contracts. Never game inputs.
enable_testing()
set(PROBE_FEATURES -mavx2 -mfma -mf16c -maes -mpclmul -msse4.2 -mcx16)
add_library(managed_production_units OBJECT
    runtime/src/core.cpp runtime/src/threads.cpp runtime/src/audio/audio.cpp
    runtime/src/video/decoder.cpp runtime/src/gpu/vk/vk_renderer.cpp
    runtime/src/gpu/vk/pipeline_warmup_cache.cpp runtime/src/gpu/vk/pipeline_catalog.cpp)
target_include_directories(managed_production_units PRIVATE runtime/include runtime/src
    ${TP}/vulkan-headers/include ${TP}/spirv-headers/include ${TP}/imgui ${TP}/imgui/backends)
target_compile_definitions(managed_production_units PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN
    IMGUI_IMPL_VULKAN_NO_PROTOTYPES IMGUI_IMPL_VULKAN_USE_LOADER
    IMGUI_DISABLE_OBSOLETE_FUNCTIONS IMGUI_USER_CONFIG="gpu/hwder_imconfig.h")
target_compile_options(managed_production_units PRIVATE ${PROBE_FEATURES} -O2 -g -fno-strict-aliasing -Wno-everything)
set(PROBE_VERSION_DIR ${CMAKE_BINARY_DIR}/version)
add_custom_target(managed_probe_version
    COMMAND ${CMAKE_COMMAND} -DSRC=${CMAKE_SOURCE_DIR} -DOUT=${PROBE_VERSION_DIR}/hwder_version.h
            -DCMAKE_DISABLE_FIND_PACKAGE_Git=TRUE -P ${CMAKE_SOURCE_DIR}/tools/version.cmake
    BYPRODUCTS ${PROBE_VERSION_DIR}/hwder_version.h
    VERBATIM)
add_dependencies(managed_production_units managed_probe_version)
target_include_directories(managed_production_units PRIVATE ${PROBE_VERSION_DIR})

add_executable(managed_runtime_probe tools/tests/managed_runtime_probe.cpp)
target_include_directories(managed_runtime_probe PRIVATE runtime/include)
target_compile_definitions(managed_runtime_probe PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(managed_runtime_probe PRIVATE ${PROBE_FEATURES} -O2)
target_link_options(managed_runtime_probe PRIVATE -static -Wl,--stack,16777216 -Wl,--disable-dynamicbase)
target_link_libraries(managed_runtime_probe PRIVATE mfplat mfuuid wmcodecdspuuid ole32 uuid dbghelp)
add_test(NAME managed_runtime_contract COMMAND managed_runtime_probe)

include(runtime/src/gpu/vk/sources.cmake)
include(tools/tests/recompiler.cmake)
add_test(NAME pipeline_warmup_cache COMMAND pipeline_warmup_cache_test)
add_test(NAME pipeline_catalog COMMAND pipeline_catalog_test)
foreach(target recompiler_synthetic pipeline_warmup_cache_test pipeline_catalog_test)
    target_link_options(${target} PRIVATE -static)
endforeach()
add_custom_target(managed_compatibility DEPENDS managed_production_units managed_runtime_probe synthetic_tests)
set_tests_properties(managed_runtime_contract recompiler_synthetic pipeline_warmup_cache pipeline_catalog PROPERTIES TIMEOUT 30)
