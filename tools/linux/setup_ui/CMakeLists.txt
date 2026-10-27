# HWDER Setup window for Linux: SDL3 (prebuilt static, from tools/linux/build_deps.sh) + Dear ImGui.
# Built by tools/linux/package_release.py on Ubuntu 22.04; it needs only glibc from the system at run time
# (SDL loads X11/Wayland/OpenGL itself). It contains no game code.
cmake_minimum_required(VERSION 3.16)
project(hwder_setup_ui CXX)
set(CMAKE_CXX_STANDARD 20)
set(HWDER_LINUX_DEPS "" CACHE PATH "prebuilt static libraries (tools/linux/build_deps.sh)")
find_package(SDL3 CONFIG REQUIRED PATHS ${HWDER_LINUX_DEPS} NO_DEFAULT_PATH)
find_package(Python3 REQUIRED COMPONENTS Interpreter)
get_filename_component(ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../../.. ABSOLUTE)
set(IMGUI ${ROOT}/third_party/imgui)

add_custom_command(OUTPUT ${CMAKE_BINARY_DIR}/font_data.h
    COMMAND ${Python3_EXECUTABLE} -c "import sys; d=open(sys.argv[1],'rb').read(); open(sys.argv[2],'w').write('static const unsigned char kFontData[] = {' + ','.join(map(str, d)) + '};\\n')"
            ${IMGUI}/misc/fonts/Roboto-Medium.ttf ${CMAKE_BINARY_DIR}/font_data.h
    DEPENDS ${IMGUI}/misc/fonts/Roboto-Medium.ttf VERBATIM)

add_executable(hwder-setup-ui main.cpp ${CMAKE_BINARY_DIR}/font_data.h
    ${IMGUI}/imgui.cpp ${IMGUI}/imgui_draw.cpp ${IMGUI}/imgui_tables.cpp ${IMGUI}/imgui_widgets.cpp
    ${IMGUI}/backends/imgui_impl_sdl3.cpp ${IMGUI}/backends/imgui_impl_sdlrenderer3.cpp)
target_include_directories(hwder-setup-ui PRIVATE ${IMGUI} ${IMGUI}/backends ${CMAKE_BINARY_DIR})
target_compile_definitions(hwder-setup-ui PRIVATE IMGUI_DISABLE_OBSOLETE_FUNCTIONS)
target_link_libraries(hwder-setup-ui PRIVATE SDL3::SDL3-static)
target_link_options(hwder-setup-ui PRIVATE -static-libstdc++ -static-libgcc)
