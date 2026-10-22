find_package(Python3 3.11 REQUIRED COMPONENTS Interpreter)
set(SYNTHETIC_SCRIPT "${CMAKE_SOURCE_DIR}/tools/tests/recompiler_synthetic.py")
set(SYNTHETIC_C "${CMAKE_BINARY_DIR}/synthetic_a64.c")
add_custom_command(OUTPUT "${SYNTHETIC_C}"
    COMMAND "${Python3_EXECUTABLE}" -B "${SYNTHETIC_SCRIPT}" --emit-c "${SYNTHETIC_C}"
    DEPENDS "${SYNTHETIC_SCRIPT}" tools/recomp/a64.py tools/recomp/a64_simd.py
    VERBATIM)
add_library(recompiler_synthetic SHARED "${SYNTHETIC_C}")
target_include_directories(recompiler_synthetic PRIVATE runtime/include)
if(MSVC)
    target_compile_options(recompiler_synthetic PRIVATE /O2 /arch:AVX2 -mfma -mf16c -maes -mpclmul -msse4.2)
else()
    target_compile_options(recompiler_synthetic PRIVATE -O2 -mavx2 -mfma -mf16c -maes -mpclmul -msse4.2)
endif()
add_test(NAME recompiler_synthetic
    COMMAND "${Python3_EXECUTABLE}" -B "${SYNTHETIC_SCRIPT}" --library "$<TARGET_FILE:recompiler_synthetic>")
add_custom_target(synthetic_tests DEPENDS recompiler_synthetic pipeline_warmup_cache_test pipeline_catalog_test
    save_folder_selection_test)
