# Owned by the Vulkan renderer. Paths relative to the repo root.
if(MSVC)
    set(HW_CODEC_TEST_FLAGS /EHsc /O2)
else()
    set(HW_CODEC_TEST_FLAGS -O2)
endif()
list(APPEND RUNTIME_SOURCES
    runtime/src/gpu/vk/vk_renderer.cpp
    runtime/src/gpu/vk/vk_sched.cpp
    runtime/src/gpu/vk/vk_resources.cpp
    runtime/src/gpu/vk/vk_pipelines.cpp
    runtime/src/gpu/vk/vk_interp_objects.cpp
    runtime/src/gpu/vk/vk_draw.cpp
    runtime/src/gpu/vk/vk_present.cpp
    runtime/src/gpu/vk/vk_debug.cpp
    runtime/src/gpu/vk/texture_cache.cpp
    runtime/src/gpu/vk/texture_layout.cpp
    runtime/src/gpu/vk/formats.cpp
    runtime/src/gpu/vk/astc.cpp
    runtime/src/gpu/vk/pipeline_warmup_cache.cpp
    runtime/src/gpu/vk/pipeline_catalog.cpp
    runtime/src/gpu/vk/catalog_game_sources.cpp
)

# Optional private-input probe; never part of the game or synthetic test aggregate.
if(WIN32)
    add_executable(pipeline_cache_persistence_bench EXCLUDE_FROM_ALL
        runtime/tests/pipeline_cache_persistence_bench.cpp runtime/src/gpu/vk/pipeline_warmup_cache.cpp)
    target_include_directories(pipeline_cache_persistence_bench PRIVATE runtime/src runtime/include ${TP}/vulkan-headers/include)
    target_compile_definitions(pipeline_cache_persistence_bench PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_compile_options(pipeline_cache_persistence_bench PRIVATE ${HW_CODEC_TEST_FLAGS})
endif()

# Standalone codec tests: no Vulkan device, generated game, SDL or guest data required.
add_executable(pipeline_warmup_cache_test EXCLUDE_FROM_ALL
    runtime/tests/pipeline_warmup_cache_test.cpp
    runtime/src/gpu/vk/pipeline_warmup_cache.cpp
)
target_include_directories(pipeline_warmup_cache_test PRIVATE runtime/src runtime/include)
target_compile_options(pipeline_warmup_cache_test PRIVATE ${HW_CODEC_TEST_FLAGS})
if(WIN32)
    target_compile_definitions(pipeline_warmup_cache_test PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
else()
    find_package(Threads REQUIRED)
    find_package(OpenSSL 3.0 REQUIRED COMPONENTS Crypto)
    target_link_libraries(pipeline_warmup_cache_test PRIVATE Threads::Threads)
endif()

set(CATALOG_TRANSLATOR_SOURCES
    runtime/src/shader/decode.cpp runtime/src/shader/fallback.cpp
    runtime/src/shader/translate.cpp runtime/src/shader/program.cpp
    runtime/src/shader/emit_alu.cpp runtime/src/shader/emit_mem.cpp
    runtime/src/debug_flags.cpp)  # translate.cpp reads HWDER_SHADER_* through the table
add_executable(pipeline_catalog_test EXCLUDE_FROM_ALL
    runtime/tests/pipeline_catalog_test.cpp
    runtime/src/gpu/vk/catalog_game_sources.cpp
    runtime/src/gpu/vk/pipeline_catalog.cpp
    runtime/src/gpu/vk/pipeline_warmup_cache.cpp ${CATALOG_TRANSLATOR_SOURCES})
if(WIN32)
add_executable(pipeline_catalog_tool EXCLUDE_FROM_ALL
    tools/pipeline_catalog/main.cpp runtime/src/config.cpp runtime/src/fs/nsp.cpp runtime/src/host/file.cpp
    runtime/src/gpu/vk/catalog_game_sources.cpp runtime/src/gpu/vk/pipeline_catalog.cpp
    runtime/src/gpu/vk/pipeline_warmup_cache.cpp ${CATALOG_TRANSLATOR_SOURCES})
endif()
set(CATALOG_TARGETS pipeline_catalog_test)
if(WIN32)
    list(APPEND CATALOG_TARGETS pipeline_catalog_tool)
endif()
foreach(target ${CATALOG_TARGETS})
    target_include_directories(${target} PRIVATE runtime/src runtime/include ${TP}/spirv-headers/include ${TP}/vulkan-headers/include)
    target_compile_options(${target} PRIVATE ${HW_CODEC_TEST_FLAGS})
    if(WIN32)
        target_compile_definitions(${target} PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
        target_link_libraries(${target} PRIVATE bcrypt)
    else()
        target_link_libraries(${target} PRIVATE OpenSSL::Crypto Threads::Threads)
    endif()
endforeach()
