cmake_minimum_required(VERSION 3.28)
project(openutau-vst-bridge VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if (MSVC)
    # The protocol carries UTF-8 payloads and this file has none, but MSVC otherwise
    # guesses the source charset from the active code page.
    add_compile_options(/utf-8)

    # clap-wrapper links the static CRT, but it only sets this inside its own directory
    # scope, so the wrapper sources it adds straight onto our module target would compile
    # /MD and fail to link against its /MT libraries. Setting it here, before any
    # subdirectory, puts the whole build on one runtime - and a statically linked plugin
    # needs no VC++ redistributable on the user's machine.
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

set(PRODUCT_NAME "OpenUtau Bridge")

# clap-wrapper resolves the (MIT) VST3 SDK itself. Its copy of CPM is vendored inside the
# submodule, so configuring only ever reaches github.com.
set(CLAP_WRAPPER_DOWNLOAD_DEPENDENCIES TRUE CACHE BOOL "" FORCE)

add_subdirectory(libs/clap EXCLUDE_FROM_ALL)
add_subdirectory(libs/clap-wrapper EXCLUDE_FROM_ALL)

# The format-agnostic implementation. Everything real goes here; the per-format modules
# are generated below and carry nothing but an entry point.
add_library(${PROJECT_NAME}-impl STATIC
        src/plugin.cpp
)
target_include_directories(${PROJECT_NAME}-impl PRIVATE
        src
        libs/json
)
target_link_libraries(${PROJECT_NAME}-impl PUBLIC clap clap-wrapper-extensions)

make_clapfirst_plugins(
        TARGET_NAME ${PROJECT_NAME}
        IMPL_TARGET ${PROJECT_NAME}-impl

        OUTPUT_NAME "${PRODUCT_NAME}"
        ENTRY_SOURCE src/bridge_entry.cpp

        BUNDLE_IDENTIFIER "moe.kakaru.openutau-bridge"
        BUNDLE_VERSION ${PROJECT_VERSION}

        COPY_AFTER_BUILD FALSE

        PLUGIN_FORMATS CLAP VST3

        ASSET_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/assets
)
