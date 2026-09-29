# Build with a separately downloaded Steinberg ASIO SDK.
set(ASIOSDK_DIR "${CMAKE_SOURCE_DIR}/third_party/asiosdk" CACHE PATH "Steinberg ASIO SDK root")
if(NOT EXISTS "${ASIOSDK_DIR}/common/iasiodrv.h")
    message(FATAL_ERROR "ASIO SDK missing. Set ASIOSDK_DIR to the directory containing common/iasiodrv.h")
endif()

add_library(PWARASIO SHARED
    pwarASIO.cpp pwarASIOLog.cpp pwarASIO.def
    ../../protocol/pwar_router.c ../../protocol/latency_manager.c
    "${ASIOSDK_DIR}/common/combase.cpp"
    "${ASIOSDK_DIR}/common/dllentry.cpp"
    "${ASIOSDK_DIR}/common/register.cpp"
)
target_include_directories(PWARASIO PRIVATE "${ASIOSDK_DIR}/common")
target_compile_definitions(PWARASIO PRIVATE WIN32 _WINDOWS _CRT_SECURE_NO_WARNINGS NOMINMAX)
target_link_libraries(PWARASIO PRIVATE ws2_32 avrt winmm ole32 advapi32 user32)
set_target_properties(PWARASIO PROPERTIES OUTPUT_NAME PWARASIO)

if(BUILD_TESTING)
    add_executable(pwar_asio_lifecycle_test tests/lifecycle.cpp)
    target_include_directories(pwar_asio_lifecycle_test PRIVATE "${ASIOSDK_DIR}/common")
    target_compile_definitions(pwar_asio_lifecycle_test PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_link_libraries(pwar_asio_lifecycle_test PRIVATE ws2_32 ole32)
    add_dependencies(pwar_asio_lifecycle_test PWARASIO)
    # Isolate the tests from the workstation's actual Pi destination.
    file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/test-profile")
    file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/test-profile/pwarASIO.cfg" "udp_send_ip=127.0.0.1\n")
    foreach(scenario offline receive lifecycle bind-conflict)
        add_test(NAME asio_${scenario} COMMAND pwar_asio_lifecycle_test $<TARGET_FILE:PWARASIO> ${scenario})
        set_tests_properties(asio_${scenario} PROPERTIES
            TIMEOUT 30 RUN_SERIAL TRUE
            ENVIRONMENT "USERPROFILE=${CMAKE_CURRENT_BINARY_DIR}/test-profile")
    endforeach()
endif()
