# Owned by the Vulkan renderer. Paths relative to the repo root.
list(APPEND RUNTIME_SOURCES
    runtime/src/gpu/vk/vk_renderer.cpp
    runtime/src/gpu/vk/vk_sched.cpp
    runtime/src/gpu/vk/vk_resources.cpp
    runtime/src/gpu/vk/texture_cache.cpp
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
    target_compile_options(pipeline_cache_persistence_bench PRIVATE /EHsc /O2)
endif()

# Standalone codec tests: no Vulkan device, generated game, SDL or guest data required.
add_executable(pipeline_warmup_cache_test EXCLUDE_FROM_ALL
    runtime/tests/pipeline_warmup_cache_test.cpp
    runtime/src/gpu/vk/pipeline_warmup_cache.cpp
)
target_include_directories(pipeline_warmup_cache_test PRIVATE runtime/src runtime/include)
target_compile_definitions(pipeline_warmup_cache_test PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(pipeline_warmup_cache_test PRIVATE /EHsc /O2)

set(CATALOG_TRANSLATOR_SOURCES
    runtime/src/shader/decode.cpp runtime/src/shader/fallback.cpp
    runtime/src/shader/translate.cpp runtime/src/shader/program.cpp
    runtime/src/shader/emit_alu.cpp runtime/src/shader/emit_mem.cpp)
add_executable(pipeline_catalog_test EXCLUDE_FROM_ALL
    runtime/tests/pipeline_catalog_test.cpp
    runtime/src/gpu/vk/catalog_game_sources.cpp
    runtime/src/gpu/vk/pipeline_catalog.cpp
    runtime/src/gpu/vk/pipeline_warmup_cache.cpp ${CATALOG_TRANSLATOR_SOURCES})
add_executable(pipeline_catalog_tool EXCLUDE_FROM_ALL
    tools/pipeline_catalog/main.cpp runtime/src/config.cpp runtime/src/fs/nsp.cpp
    runtime/src/gpu/vk/catalog_game_sources.cpp runtime/src/gpu/vk/pipeline_catalog.cpp
    runtime/src/gpu/vk/pipeline_warmup_cache.cpp ${CATALOG_TRANSLATOR_SOURCES})
foreach(target pipeline_catalog_test pipeline_catalog_tool)
    target_include_directories(${target} PRIVATE runtime/src runtime/include ${TP}/spirv-headers/include ${TP}/vulkan-headers/include)
    target_compile_definitions(${target} PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_compile_options(${target} PRIVATE /EHsc /O2)
    target_link_libraries(${target} PRIVATE bcrypt)
endforeach()
