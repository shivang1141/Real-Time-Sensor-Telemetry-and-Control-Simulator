# Warning set applied to project targets (not to third-party code).
add_library(telemetry_warnings INTERFACE)
target_compile_options(telemetry_warnings INTERFACE
    -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion
    -Wnon-virtual-dtor -Wold-style-cast -Woverloaded-virtual -Wformat=2)
if(TELEMETRY_WARNINGS_AS_ERRORS)
    target_compile_options(telemetry_warnings INTERFACE -Werror)
endif()

# Sanitizers are applied globally so every translation unit is instrumented consistently.
if(TELEMETRY_SANITIZER STREQUAL "address")
    message(STATUS "Sanitizers: AddressSanitizer + UndefinedBehaviorSanitizer")
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer
                        -fno-sanitize-recover=undefined)
    add_link_options(-fsanitize=address,undefined)
elseif(TELEMETRY_SANITIZER STREQUAL "thread")
    message(STATUS "Sanitizers: ThreadSanitizer")
    add_compile_options(-fsanitize=thread -fno-omit-frame-pointer)
    add_link_options(-fsanitize=thread)
elseif(NOT TELEMETRY_SANITIZER STREQUAL "")
    message(FATAL_ERROR "Unknown TELEMETRY_SANITIZER '${TELEMETRY_SANITIZER}'")
endif()
