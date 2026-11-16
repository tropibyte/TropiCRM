cmake_minimum_required(VERSION 3.20)

project(TropiCRM
    VERSION 0.9.0
    DESCRIPTION "A multi-tenant CRM server with a Salesforce-style REST API"
    LANGUAGES CXX)

option(TROPICRM_BUILD_TESTS "Build the unit and API tests" ${PROJECT_IS_TOP_LEVEL})
option(TROPICRM_BUILD_TOOLS "Build tropicrm-admin" ON)
option(TROPICRM_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)
option(TROPICRM_SANITIZE "Build with AddressSanitizer and UBSan (GCC/Clang)" OFF)
option(TROPICRM_TSAN "Build with ThreadSanitizer (GCC/Clang)" OFF)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
find_package(Threads REQUIRED)

add_library(tropicrm_build_flags INTERFACE)
target_compile_features(tropicrm_build_flags INTERFACE cxx_std_20)
if(MSVC)
    target_compile_options(tropicrm_build_flags INTERFACE /W4 /permissive- /utf-8 /Zc:__cplusplus /EHsc)
    target_compile_definitions(tropicrm_build_flags INTERFACE _CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN)
    if(TROPICRM_WARNINGS_AS_ERRORS)
        target_compile_options(tropicrm_build_flags INTERFACE /WX)
    endif()
else()
    target_compile_options(tropicrm_build_flags INTERFACE -Wall -Wextra -Wpedantic)
    if(TROPICRM_WARNINGS_AS_ERRORS)
        target_compile_options(tropicrm_build_flags INTERFACE -Werror)
    endif()
    if(TROPICRM_SANITIZE)
        target_compile_options(tropicrm_build_flags INTERFACE -fsanitize=address,undefined -fno-omit-frame-pointer)
        target_link_options(tropicrm_build_flags INTERFACE -fsanitize=address,undefined)
    endif()
    if(TROPICRM_TSAN)
        target_compile_options(tropicrm_build_flags INTERFACE -fsanitize=thread)
        target_link_options(tropicrm_build_flags INTERFACE -fsanitize=thread)
    endif()
endif()

add_library(tropicrm_core STATIC
    src/util/config.cpp
    src/util/crypto.cpp
    src/util/csv.cpp
    src/util/id.cpp
    src/util/log.cpp
    src/util/strutil.cpp
    src/util/thread_pool.cpp
    src/util/time.cpp
    src/util/url.cpp
    src/json/json.cpp
    src/http/client.cpp
    src/http/message.cpp
    src/http/parser.cpp
    src/http/router.cpp
    src/http/server.cpp
    src/http/socket.cpp
    src/model/value.cpp
    src/meta/codec.cpp
    src/meta/field.cpp
    src/meta/schema.cpp
    src/meta/standard_objects.cpp
    src/storage/database.cpp
    src/storage/table.cpp
    src/storage/wal.cpp
    src/auth/token.cpp
    src/auth/user.cpp
    src/security/profile.cpp
    src/security/sharing.cpp
    src/formula/evaluator.cpp
    src/formula/parser.cpp
    src/query/executor.cpp
    src/query/parser.cpp
    src/engine/access.cpp
    src/engine/approvals.cpp
    src/engine/behaviors.cpp
    src/engine/forecast.cpp
    src/engine/formula_context.cpp
    src/engine/lead_convert.cpp
    src/engine/merge.cpp
    src/engine/org.cpp
    src/engine/record_service.cpp
    src/engine/records.cpp
    src/services/audit.cpp
    src/services/bulk.cpp
    src/services/dashboards.cpp
    src/services/files.cpp
    src/services/outbox.cpp
    src/services/recent.cpp
    src/services/reports.cpp
    src/services/search.cpp
    src/services/templates.cpp
    src/services/webhooks.cpp
    src/api/admin_routes.cpp
    src/api/analytics_routes.cpp
    src/api/auth_routes.cpp
    src/api/authenticator.cpp
    src/api/bulk_routes.cpp
    src/api/helpers.cpp
    src/api/misc_routes.cpp
    src/api/process_routes.cpp
    src/api/public_routes.cpp
    src/api/query_routes.cpp
    src/api/rate_limiter.cpp
    src/api/report_routes.cpp
    src/api/setup_routes.cpp
    src/api/sobject_routes.cpp
    src/api/tooling_routes.cpp
    src/api/user_routes.cpp
    src/api/webhook_routes.cpp
    src/app/application.cpp
)
target_include_directories(tropicrm_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(tropicrm_core PUBLIC Threads::Threads PRIVATE tropicrm_build_flags)
if(WIN32)
    target_link_libraries(tropicrm_core PUBLIC ws2_32)
endif()

add_executable(tropicrm-server src/app/main.cpp)
target_link_libraries(tropicrm-server PRIVATE tropicrm_core tropicrm_build_flags)

if(TROPICRM_BUILD_TOOLS)
    add_executable(tropicrm-admin tools/tropicrm_admin.cpp)
    target_link_libraries(tropicrm-admin PRIVATE tropicrm_core tropicrm_build_flags)
endif()

if(TROPICRM_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

install(TARGETS tropicrm-server RUNTIME DESTINATION bin)
if(TROPICRM_BUILD_TOOLS)
    install(TARGETS tropicrm-admin RUNTIME DESTINATION bin)
endif()
