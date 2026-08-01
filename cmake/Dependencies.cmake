# Prefer system packages (apt: nlohmann-json3-dev, libgtest-dev); fall back to pinned downloads.
include(FetchContent)

find_package(nlohmann_json 3.10 CONFIG QUIET)
if(nlohmann_json_FOUND)
    message(STATUS "Using system nlohmann_json ${nlohmann_json_VERSION}")
else()
    message(STATUS "nlohmann_json not found; fetching v3.11.3")
    FetchContent_Declare(nlohmann_json
        GIT_REPOSITORY https://github.com/nlohmann/json.git
        GIT_TAG v3.11.3
        GIT_SHALLOW TRUE)
    set(JSON_BuildTests OFF CACHE INTERNAL "")
    FetchContent_MakeAvailable(nlohmann_json)
endif()

if(TELEMETRY_BUILD_TESTS)
    find_package(GTest 1.10 CONFIG QUIET)
    if(GTest_FOUND)
        message(STATUS "Using system GoogleTest ${GTest_VERSION}")
    else()
        message(STATUS "GoogleTest not found; fetching v1.14.0")
        FetchContent_Declare(googletest
            GIT_REPOSITORY https://github.com/google/googletest.git
            GIT_TAG v1.14.0
            GIT_SHALLOW TRUE)
        set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
        FetchContent_MakeAvailable(googletest)
    endif()
endif()
