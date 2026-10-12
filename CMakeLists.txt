cmake_minimum_required(VERSION 3.20)
project(hoi4_loader LANGUAGES C CXX ASM_MASM)

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "hoi4_loader must be built for x64 (HOI4 is a 64-bit game).")
endif()

# Static CRT so version.dll has no runtime dependencies beyond system DLLs.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# MinHook, pinned. Only the sources are fetched; we build them ourselves so the
# upstream CMakeLists (install/export rules) stays out of the way.
include(FetchContent)
FetchContent_Declare(minhook
    GIT_REPOSITORY https://github.com/TsudaKageyu/minhook.git
    GIT_TAG        v1.3.3
    GIT_SHALLOW    TRUE
    SOURCE_SUBDIR  do-not-add-subdirectory)
FetchContent_MakeAvailable(minhook)

add_library(minhook STATIC
    ${minhook_SOURCE_DIR}/src/buffer.c
    ${minhook_SOURCE_DIR}/src/hook.c
    ${minhook_SOURCE_DIR}/src/trampoline.c
    ${minhook_SOURCE_DIR}/src/hde/hde64.c)
target_include_directories(minhook PUBLIC ${minhook_SOURCE_DIR}/include)

add_library(hoi4_loader SHARED
    src/main.cpp
    src/hash.cpp
    src/vfs.cpp
    src/sigscan.cpp
    src/features.cpp
    src/plugin_engine_probe.cpp
    src/plugin_region_names.cpp
    src/engine.cpp
    src/engine.h
    src/lifecycle.cpp
    src/lifecycle.h
    src/loader.h
    src/signatures.h
    src/sha256.h
    src/proxy.asm
    src/version.def)
set_target_properties(hoi4_loader PROPERTIES OUTPUT_NAME "version")
target_compile_features(hoi4_loader PRIVATE cxx_std_17)
target_compile_definitions(hoi4_loader PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(hoi4_loader PRIVATE $<$<COMPILE_LANGUAGE:CXX>:/W4>)
target_link_libraries(hoi4_loader PRIVATE minhook)

# Keep the config next to the built DLL so the output folder is drop-in ready.
add_custom_command(TARGET hoi4_loader POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${CMAKE_CURRENT_SOURCE_DIR}/loader_config.ini $<TARGET_FILE_DIR:hoi4_loader>)

# `cmake --install build --config Release --prefix "<HOI4 folder>"` drops both files in place.
install(TARGETS hoi4_loader RUNTIME DESTINATION .)
install(FILES loader_config.ini DESTINATION .)

# Out-of-game smoke test (tests/run_smoke.sh). Two stand-in executables, both named hoi4.exe
# and importing version.dll, so the proxy loads exactly as in the game:
#   build/smoke/sig/hoi4.exe    contains the PhysFS signature bytes (engine_probe can succeed)
#   build/smoke/nosig/hoi4.exe  does not (engine_probe must fail safe)
option(HOI4_LOADER_BUILD_TESTS "Build the out-of-game smoke test executables" ON)
if(HOI4_LOADER_BUILD_TESTS)
    foreach(variant sig nosig)
        set(target smoke_${variant})
        add_executable(${target} tests/smoke.cpp)
        if(variant STREQUAL "sig")
            target_sources(${target} PRIVATE tests/fake_physfs.asm)
            target_compile_definitions(${target} PRIVATE WITH_FAKE_PHYSFS)
        endif()
        target_compile_features(${target} PRIVATE cxx_std_17)
        target_link_libraries(${target} PRIVATE version.lib)  # the system import library
        set_target_properties(${target} PROPERTIES
            OUTPUT_NAME hoi4
            RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/smoke/${variant}/$<0:>)
        add_dependencies(${target} hoi4_loader)
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:hoi4_loader> $<TARGET_FILE_DIR:${target}>)
    endforeach()
endif()
