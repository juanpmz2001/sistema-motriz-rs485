include(CMakeParseArguments)

function(botfarms_configure_host_c_target target)
    target_compile_features(${target} PUBLIC c_std_11)
    set_target_properties(${target} PROPERTIES
        C_STANDARD 11
        C_STANDARD_REQUIRED YES
        C_EXTENSIONS NO
    )

    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /WX)
    elseif(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(${target} PRIVATE
            -Wall
            -Wextra
            -Wpedantic
            -Werror
            -Wstrict-prototypes
        )

        if(BOTFARMS_HOST_TEST_SANITIZERS)
            target_compile_options(${target} PRIVATE
                -fno-omit-frame-pointer
                -fsanitize=address,undefined
            )
            target_link_options(${target} PRIVATE -fsanitize=address,undefined)
        endif()
    endif()
endfunction()

function(botfarms_add_host_test)
    set(options NO_DEFAULT_SUPPORT)
    set(one_value_args NAME TIMEOUT)
    set(multi_value_args SOURCES INCLUDE_DIRS LINK_LIBRARIES COMPILE_DEFINITIONS)
    cmake_parse_arguments(BHT
        "${options}"
        "${one_value_args}"
        "${multi_value_args}"
        ${ARGN}
    )

    if(NOT BHT_NAME)
        message(FATAL_ERROR "botfarms_add_host_test requires NAME")
    endif()
    if(NOT BHT_SOURCES)
        message(FATAL_ERROR "botfarms_add_host_test(${BHT_NAME}) requires SOURCES")
    endif()
    if(NOT BHT_TIMEOUT)
        if(BOTFARMS_HOST_TEST_TIMEOUT_SECONDS)
            set(BHT_TIMEOUT "${BOTFARMS_HOST_TEST_TIMEOUT_SECONDS}")
        else()
            set(BHT_TIMEOUT "10")
        endif()
    endif()

    add_executable(${BHT_NAME} ${BHT_SOURCES})
    botfarms_configure_host_c_target(${BHT_NAME})

    if(BHT_INCLUDE_DIRS)
        target_include_directories(${BHT_NAME} PRIVATE ${BHT_INCLUDE_DIRS})
    endif()
    if(BHT_COMPILE_DEFINITIONS)
        target_compile_definitions(${BHT_NAME} PRIVATE ${BHT_COMPILE_DEFINITIONS})
    endif()
    if(NOT BHT_NO_DEFAULT_SUPPORT AND TARGET botfarms_host_test_support)
        target_link_libraries(${BHT_NAME} PRIVATE botfarms_host_test_support)
    endif()
    if(BHT_LINK_LIBRARIES)
        target_link_libraries(${BHT_NAME} PRIVATE ${BHT_LINK_LIBRARIES})
    endif()

    add_test(NAME ${BHT_NAME} COMMAND ${BHT_NAME})
    set_tests_properties(${BHT_NAME} PROPERTIES
        LABELS "host"
        TIMEOUT "${BHT_TIMEOUT}"
    )
endfunction()
