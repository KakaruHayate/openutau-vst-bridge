# One binary for every test: doctest registers cases across translation units, and a single
# executable keeps the loopback conformance tests in the same process as the unit tests.
add_executable(bridge-tests
        test_main.cpp
        test_hash.cpp
        test_frame.cpp
        test_messages.cpp
        test_fader.cpp
        test_reader.cpp
        test_discovery.cpp
        test_audio_store.cpp
        test_timeline.cpp
        test_transport.cpp
        test_connection.cpp
        test_socket.cpp
        test_session.cpp
        test_conformance.cpp
)
target_include_directories(bridge-tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../libs/doctest
)
target_link_libraries(bridge-tests PRIVATE ${PROJECT_NAME}-impl)

add_test(NAME bridge-tests COMMAND bridge-tests)
