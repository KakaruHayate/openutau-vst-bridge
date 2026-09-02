cmake_minimum_required(VERSION 3.28)
project(openutau-vst-bridge VERSION 0.1.0 LANGUAGES C CXX)

if (APPLE)
    # clap-wrapper's macOS path attaches Objective-C++ sources to the format modules, and those
    # targets are created in this directory scope. A language enabled inside the wrapper's own
    # subdirectory does not reach up here, so generating fails with a missing
    # CMAKE_OBJCXX_COMPILE_OBJECT unless the languages are declared before it is added.
    enable_language(OBJC)
    enable_language(OBJCXX)
endif()

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
        src/log.cpp
        src/hash.cpp
        src/frame.cpp
        src/messages.cpp
        src/discovery.cpp
        src/reader.cpp
        src/socket.cpp
        src/connection.cpp
        src/audio_store.cpp
        src/timeline.cpp
        src/session.cpp
        src/plugin.cpp

        # r8brain's whole implementation is this one translation unit. Only the store includes
        # its headers; everything else stays out of the resampler's namespace.
        libs/r8brain/r8bbase.cpp
)
target_include_directories(${PROJECT_NAME}-impl
        PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src
        PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/libs/json
        ${CMAKE_CURRENT_SOURCE_DIR}/libs/xxhash
        ${CMAKE_CURRENT_SOURCE_DIR}/libs/r8brain
)
target_link_libraries(${PROJECT_NAME}-impl PUBLIC clap clap-wrapper-extensions)
if (WIN32)
    # Winsock. PUBLIC because the tests link the implementation directly and open a real
    # loopback socket to exercise the framing end to end.
    target_link_libraries(${PROJECT_NAME}-impl PUBLIC ws2_32)
endif()

make_clapfirst_plugins(
        TARGET_NAME ${PROJECT_NAME}
        IMPL_TARGET ${PROJECT_NAME}-impl

        OUTPUT_NAME "${PRODUCT_NAME}"
        ENTRY_SOURCE src/bridge_entry.cpp

        BUNDLE_IDENTIFIER "moe.kakaru.openutau-bridge"
        BUNDLE_VERSION ${PROJECT_VERSION}

        COPY_AFTER_BUILD FALSE

        # The bundle layout — Name.vst3/Contents/x86_64-win/Name.vst3 — rather than a bare DLL
        # named .vst3. Both load in most Windows hosts, but only the folder is what the current
        # VST3 spec describes, and Steinberg's validator rejects the file form outright.
        WINDOWS_FOLDER_VST3 TRUE

        PLUGIN_FORMATS CLAP VST3

        ASSET_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/assets
)

# Installing into the per-user plug-in folders, which is where a host looks and which needs no
# administrator rights — unlike the machine-wide ones under Program Files. Windows only: the
# other platforms' paths are known but have never been built here, so they are not guessed at.
if (WIN32 AND DEFINED ENV{LOCALAPPDATA})
    set(user_plugin_root "$ENV{LOCALAPPDATA}/Programs/Common")
    add_custom_target(bridge-install
            COMMAND ${CMAKE_COMMAND} -E make_directory "${user_plugin_root}/VST3"
            COMMAND ${CMAKE_COMMAND} -E copy_directory
                    "${CMAKE_BINARY_DIR}/assets/VST3/${PRODUCT_NAME}.vst3"
                    "${user_plugin_root}/VST3/${PRODUCT_NAME}.vst3"
            COMMAND ${CMAKE_COMMAND} -E make_directory "${user_plugin_root}/CLAP"
            COMMAND ${CMAKE_COMMAND} -E copy
                    "${CMAKE_BINARY_DIR}/assets/CLAP/$<CONFIG>/${PRODUCT_NAME}.clap"
                    "${user_plugin_root}/CLAP/${PRODUCT_NAME}.clap"
            COMMAND ${CMAKE_COMMAND} -E echo "Installed to ${user_plugin_root}."
            DEPENDS ${PROJECT_NAME}_all
            VERBATIM
    )
endif()

# The tests link the implementation directly rather than loading a built module, so a
# protocol regression fails the build instead of only failing in a host.
option(BRIDGE_BUILD_TESTS "Build the bridge's unit and conformance tests" ON)
if (BRIDGE_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

# Off by default: it is a verification tool, not something a user of the plugin needs. On, it
# gives OpenUtau's own client something real to connect to without a DAW in the way.
option(BRIDGE_BUILD_HOST "Build the standalone session host used for cross-language checks" OFF)
if (BRIDGE_BUILD_HOST)
    add_executable(bridge-host tools/bridge_host.cpp)
    target_link_libraries(bridge-host PRIVATE ${PROJECT_NAME}-impl)
endif()
