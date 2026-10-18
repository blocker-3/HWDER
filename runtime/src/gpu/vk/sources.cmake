# Owned by the Vulkan renderer. Paths relative to the repo root.
list(APPEND RUNTIME_SOURCES
    runtime/src/gpu/vk/vk_renderer.cpp
    runtime/src/gpu/vk/texture_cache.cpp
    runtime/src/gpu/vk/formats.cpp
    runtime/src/gpu/vk/astc.cpp
    runtime/src/gpu/vk/pipeline_warmup_cache.cpp
)

# Standalone codec tests: no Vulkan device, generated game, SDL or guest data required.
add_executable(pipeline_warmup_cache_test EXCLUDE_FROM_ALL
    runtime/tests/pipeline_warmup_cache_test.cpp
    runtime/src/gpu/vk/pipeline_warmup_cache.cpp
)
target_include_directories(pipeline_warmup_cache_test PRIVATE runtime/src runtime/include)
target_compile_definitions(pipeline_warmup_cache_test PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(pipeline_warmup_cache_test PRIVATE /EHsc /O2)
