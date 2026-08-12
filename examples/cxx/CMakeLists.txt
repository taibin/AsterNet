# AsterNet C++ 单元测试
cmake_minimum_required(VERSION 3.18)

set(TEST_SOURCES
    test_event_loop.cpp
    test_network_modules.cpp
)

set(NETWORK_TEST_SOURCES
    test_abi.cpp
    test_http1_engine.cpp
    test_http2_engine.cpp
    test_protocol_switch.cpp
    test_e2e.cpp)

foreach(src ${TEST_SOURCES})
    get_filename_component(name ${src} NAME_WE)
    add_executable(${name} ${src})
    target_link_libraries(${name} PRIVATE asternet)
    target_include_directories(${name} PRIVATE
        ${CMAKE_SOURCE_DIR}/sdk/cxx/public
        ${CMAKE_SOURCE_DIR}/sdk/cxx
        ${CMAKE_SOURCE_DIR}/sdk/third_party/nghttp2/lib/includes
        ${CMAKE_SOURCE_DIR}/sdk/third_party/xquic/third_party/boringssl/include)
    if(ASTERNET_ENABLE_XQUIC)
        target_include_directories(${name} PRIVATE
            ${CMAKE_SOURCE_DIR}/sdk/third_party/xquic/include)
        target_compile_definitions(${name} PRIVATE ASTERNET_ENABLE_XQUIC=1)
    endif()
    add_test(NAME ${name} COMMAND ${name})
endforeach()

if(ASTERNET_REGISTER_NETWORK_TESTS)
    foreach(src ${NETWORK_TEST_SOURCES})
        get_filename_component(name ${src} NAME_WE)
        add_executable(${name} ${src})
        target_link_libraries(${name} PRIVATE asternet)
        target_include_directories(${name} PRIVATE
            ${CMAKE_SOURCE_DIR}/sdk/cxx/public
            ${CMAKE_SOURCE_DIR}/sdk/cxx)
        add_test(NAME ${name} COMMAND ${name})
    endforeach()

    if(ASTERNET_ENABLE_XQUIC)
        foreach(src test_quic_engine.cpp test_quic_integration.cpp test_h3_get.cpp)
            get_filename_component(name ${src} NAME_WE)
            add_executable(${name} ${src})
            target_link_libraries(${name} PRIVATE asternet)
            target_include_directories(${name} PRIVATE
                ${CMAKE_SOURCE_DIR}/sdk/cxx/public
                ${CMAKE_SOURCE_DIR}/sdk/cxx
                ${CMAKE_SOURCE_DIR}/sdk/third_party/nghttp2/lib/includes
                ${CMAKE_SOURCE_DIR}/sdk/third_party/xquic/third_party/boringssl/include
                ${CMAKE_SOURCE_DIR}/sdk/third_party/xquic/include)
            target_compile_definitions(${name} PRIVATE ASTERNET_ENABLE_XQUIC=1)
            add_test(NAME ${name} COMMAND ${name})
        endforeach()
    endif()
endif()
