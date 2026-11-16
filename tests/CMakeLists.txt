add_executable(tropicrm_tests
    testing.cpp
    test_util.cpp
    test_json.cpp
    test_http.cpp
    test_formula.cpp
    test_query.cpp
    test_engine.cpp
    test_api.cpp
    test_storage.cpp
    test_process.cpp
    test_analytics.cpp
    test_more.cpp
)
target_link_libraries(tropicrm_tests PRIVATE tropicrm_core tropicrm_build_flags)

# One CTest entry per suite so failures are easy to spot and suites run in
# parallel with `ctest -j`.
set(TROPICRM_TEST_SUITES
    strutil time crypto ids url csv config lru thread_pool
    json
    http_parser http_router http_message http_server
    formula
    query_parser query_exec
    records automation sharing custom_objects
    api_auth api_basics api_composite api_users api_reports api_bulk api_files api_webhooks api_admin api_tooling
    storage schema search_index
    approvals templates merge transfer activities audit web_forms metrics
    rate_limit forecasts dashboards setup
    bulk_ops picklists query_dates notes
)
foreach(suite IN LISTS TROPICRM_TEST_SUITES)
    add_test(NAME ${suite} COMMAND tropicrm_tests --suite ${suite})
    set_tests_properties(${suite} PROPERTIES TIMEOUT 120)
endforeach()
