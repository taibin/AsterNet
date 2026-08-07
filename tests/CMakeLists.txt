# ABI 冒烟测试：不依赖第三方框架，仅链接核心库
add_executable(test_abi test_abi.cpp)
target_link_libraries(test_abi PRIVATE asternet-core)
target_include_directories(test_abi PRIVATE ${CMAKE_SOURCE_DIR}/include)

# 端到端测试：经 CABI → ProtocolSelector 降级链
add_executable(test_e2e test_e2e.cpp)
target_link_libraries(test_e2e PRIVATE asternet-core)
target_include_directories(test_e2e PRIVATE ${CMAKE_SOURCE_DIR}/include)

# 协议切换测试：对比 H1/H2/H3 耗时
add_executable(test_protocol_switch test_protocol_switch.cpp)
target_link_libraries(test_protocol_switch PRIVATE asternet-core)
target_include_directories(test_protocol_switch PRIVATE ${CMAKE_SOURCE_DIR}/include)

# HTTP/2 冒烟工具：可传 host/port/path 对本地或指定 H2 服务端验证。
add_executable(test_http2_engine test_http2_engine.cpp)
target_link_libraries(test_http2_engine PRIVATE asternet-core)
target_include_directories(test_http2_engine PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src)
if(ANDROID)
    target_link_libraries(test_http2_engine PRIVATE log)
endif()

add_executable(test_http1_engine test_http1_engine.cpp)
target_link_libraries(test_http1_engine PRIVATE asternet-core)
target_include_directories(test_http1_engine PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src)

# 抑制测试代码中的 assert 在 Release 下被消除（确保断言生效）
target_compile_options(test_abi PRIVATE -UNDEBUG)
target_compile_options(test_e2e PRIVATE -UNDEBUG)
target_compile_options(test_protocol_switch PRIVATE -UNDEBUG)
target_compile_options(test_http2_engine PRIVATE -UNDEBUG)
target_compile_options(test_http1_engine PRIVATE -UNDEBUG)

add_test(NAME test_abi COMMAND test_abi)
if(ASTERNET_REGISTER_NETWORK_TESTS)
    add_test(NAME test_e2e COMMAND test_e2e)
    add_test(NAME test_protocol_switch COMMAND test_protocol_switch)
endif()
