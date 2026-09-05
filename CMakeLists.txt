cmake_minimum_required(VERSION 3.31)

# Before project(), which is where CMake creates this cache entry. Left empty it is not
# "undefined" either, so clap-wrapper's own default never fires and instead its prologue reads the
# empty string as older than 10.15 and downloads gulrak/filesystem as a stub. 11.0 is also the
# honest floor: std::filesystem in libc++ is annotated as introduced in macOS 10.15, and this
# project's discovery layer is built on it.
if (APPLE AND NOT CMAKE_OSX_DEPLOYMENT_TARGET)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "Minimum macOS version")
endif()

project(openutau-vst-bridge VERSION 0.2.0 LANGUAGES C CXX)

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
# std::thread, and r8brain calls pthread_mutex_* directly. Linux with glibc 2.34 or newer folds
# libpthread into libc and links this anyway, which is exactly why it is easy to leave out and
# find later on an older or musl toolchain. PUBLIC because the tests and the standalone host link
# this library and start threads of their own.
find_package(Threads REQUIRED)
target_link_libraries(${PROJECT_NAME}-impl PUBLIC Threads::Threads)
if (WIN32)
    # Winsock. PUBLIC because the tests link the implementation directly and open a real
    # loopback socket to exercise the framing end to end.
    target_link_libraries(${PROJECT_NAME}-impl PUBLIC ws2_32)
endif()

# The plugin window: a track picker plus the info rows. YUP (ISC, JUCE7-derived, Rive
# renderer) draws it on every platform with one component tree; the stub backend stands
# in when this is off, and then the plugin advertises no gui at all.
option(BRIDGE_ENABLE_GUI "Build the plugin window on the YUP toolkit" ON)
if (BRIDGE_ENABLE_GUI)
    include(FetchContent)
    # Pinned: YUP moves fast and its GUI APIs are still settling. Bump the pin together
    # with any code that follows its API.
    FetchContent_Declare(yup
        GIT_REPOSITORY https://github.com/kunitoki/yup.git
        GIT_TAG ff8ad8ee64d7f4cdea17436863b4eee39427d8d6)
    # Off by default when fetched as a subproject; pinned explicitly so a future YUP
    # default flip cannot silently turn our builds into its demo gallery.
    set(YUP_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(YUP_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(YUP_BUILD_PLUGINS OFF CACHE BOOL "" FORCE)
    set(YUP_BUILD_WHEEL OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(yup)

    # YUP's gui module compiles its SDL3 windowing backend into the consumer, but only its
    # audio-plugin and standalone helpers fetch SDL3 — linking yup::yup_gui alone leaves the
    # module without the headers. The helpers' internal fetch is exactly what we need, and
    # the function stays defined in scope after MakeAvailable, so call it here and link the
    # result: the module sources compile inside our impl target, on this target's include
    # path and against this target's libraries.
    _yup_fetch_sdl()

    target_sources(${PROJECT_NAME}-impl PRIVATE src/gui_yup.cpp)
    target_link_libraries(${PROJECT_NAME}-impl PRIVATE yup::yup_gui sdl::sdl)
else()
    target_sources(${PROJECT_NAME}-impl PRIVATE src/gui_stub.cpp)
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
