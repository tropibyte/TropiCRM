// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tropibyte, Inc.

#include <chrono>
#include <thread>

#include "http/client.hpp"
#include "http/parser.hpp"
#include "http/router.hpp"
#include "http/server.hpp"
#include "testing.hpp"

using namespace tcrm;
using http::RequestParser;

TEST(http_parser, simple_get) {
    RequestParser p;
    std::string buf = "GET /services/data/v1/query?q=SELECT+Id+FROM+Account HTTP/1.1\r\nHost: x\r\n"
                      "Authorization: Bearer abc\r\n\r\n";
    REQUIRE(p.parse(buf) == RequestParser::Result::Complete);
    http::Request req = p.take();
    CHECK(req.method == http::Method::Get);
    CHECK_EQ(req.path, std::string("/services/data/v1/query"));
    CHECK_EQ(req.query_param("q"), std::string("SELECT Id FROM Account"));
    CHECK_EQ(req.headers.get("authorization"), std::string("Bearer abc"));
    CHECK(req.keep_alive());
    CHECK(buf.empty());
}

TEST(http_parser, incremental_body) {
    RequestParser p;
    std::string buf = "POST /x HTTP/1.1\r\nContent-Length: 11\r\nConnection: close\r\n\r\nhello";
    CHECK(p.parse(buf) == RequestParser::Result::NeedMore);
    buf += " world";
    REQUIRE(p.parse(buf) == RequestParser::Result::Complete);
    http::Request req = p.take();
    CHECK_EQ(req.body, std::string("hello world"));
    CHECK(!req.keep_alive());
}

TEST(http_parser, chunked_body) {
    RequestParser p;
    std::string buf = "PUT /jobs HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nName,\r\n6;ext=1\r\nPhone\n\r\n0\r\n\r\n";
    REQUIRE(p.parse(buf) == RequestParser::Result::Complete);
    CHECK_EQ(p.take().body, std::string("Name,Phone\n"));
}

TEST(http_parser, pipelined_get_requests) {
    RequestParser p;
    std::string buf = "GET /a HTTP/1.1\r\n\r\nGET /b HTTP/1.1\r\n\r\n";
    REQUIRE(p.parse(buf) == RequestParser::Result::Complete);
    CHECK_EQ(p.take().path, std::string("/a"));
    REQUIRE(p.parse(buf) == RequestParser::Result::Complete);
    CHECK_EQ(p.take().path, std::string("/b"));
}

TEST(http_parser, rejects_bad_input) {
    {
        RequestParser p;
        std::string buf = "GARBAGE\r\n\r\n";
        CHECK(p.parse(buf) == RequestParser::Result::Error);
        CHECK_EQ(p.error_status(), 400);
    }
    {
        RequestParser p;
        std::string buf = "GET /x HTTP/2.0\r\n\r\n";
        CHECK(p.parse(buf) == RequestParser::Result::Error);
    }
    {
        http::ParserLimits limits;
        limits.max_body_bytes = 10;
        RequestParser p(limits);
        std::string buf = "POST /x HTTP/1.1\r\nContent-Length: 11\r\n\r\n";
        CHECK(p.parse(buf) == RequestParser::Result::Error);
        CHECK_EQ(p.error_status(), 413);
    }
    {
        http::ParserLimits limits;
        limits.max_header_bytes = 32;
        RequestParser p(limits);
        std::string buf = "GET /" + std::string(100, 'a');
        CHECK(p.parse(buf) == RequestParser::Result::Error);
        CHECK_EQ(p.error_status(), 431);
    }
    {
        RequestParser p;
        std::string buf = "POST /x HTTP/1.1\r\nTransfer-Encoding: gzip\r\n\r\n";
        CHECK(p.parse(buf) == RequestParser::Result::Error);
        CHECK_EQ(p.error_status(), 501);
    }
    {
        RequestParser p;
        std::string buf = "GET /x HTTP/1.1\r\nBad Header: x\r\n\r\n";
        CHECK(p.parse(buf) == RequestParser::Result::Error);
    }
}

TEST(http_router, matches_patterns) {
    http::Router r;
    std::string hit;
    r.get("/services/data/v1/sobjects/{object}/describe", [&](http::Request&) {
        hit = "describe";
        return http::Response::no_content();
    });
    r.get("/services/data/v1/sobjects/{object}/{id}", [&](http::Request&) {
        hit = "get";
        return http::Response::no_content();
    });
    r.patch("/services/data/v1/sobjects/{object}/{id}", [&](http::Request&) {
        hit = "patch";
        return http::Response::no_content();
    });
    r.get("/files/{*path}", [&](http::Request&) {
        hit = "files";
        return http::Response::no_content();
    });

    http::Request req;
    req.method = http::Method::Get;
    req.path = "/services/data/v1/sobjects/Account/describe";
    auto m = r.match(req);
    REQUIRE(m.handler != nullptr);
    (*m.handler)(req);
    CHECK_EQ(hit, std::string("describe"));
    CHECK_EQ(req.path_param("object"), std::string("Account"));

    req.path = "/services/data/v1/sobjects/Account/001A0000006Vm9rIAC";
    m = r.match(req);
    REQUIRE(m.handler != nullptr);
    CHECK_EQ(req.path_param("id"), std::string("001A0000006Vm9rIAC"));

    req.method = http::Method::Delete;
    m = r.match(req);
    CHECK(m.handler == nullptr);
    CHECK_EQ(m.allowed.size(), size_t{2});

    req.method = http::Method::Head;
    req.path = "/services/data/v1/sobjects/Lead/x";
    CHECK(r.match(req).handler != nullptr);

    req.method = http::Method::Get;
    req.path = "/files/a/b%20c/d";
    m = r.match(req);
    REQUIRE(m.handler != nullptr);
    CHECK_EQ(req.path_param("path"), std::string("a/b c/d"));

    req.path = "/nothing/here";
    m = r.match(req);
    CHECK(m.handler == nullptr);
    CHECK(m.allowed.empty());
}

TEST(http_message, response_serialization) {
    http::Response r = http::Response::json(201, tcrm::json::parse(R"({"id":"x"})"));
    r.headers.set("Location", "/x");
    const std::string wire = r.serialize(false);
    CHECK(wire.rfind("HTTP/1.1 201 Created\r\n", 0) == 0);
    CHECK(wire.find("Content-Length: 10\r\n") != std::string::npos);
    CHECK(wire.find("Location: /x\r\n") != std::string::npos);
    CHECK(wire.find("\r\n\r\n{\"id\":\"x\"}") != std::string::npos);
    http::Response nc = http::Response::no_content();
    CHECK(nc.serialize(false).find("Content-Length") == std::string::npos);
    CHECK_EQ(std::string(http::reason_phrase(404)), std::string("Not Found"));
    http::Headers h;
    h.add("X-A", "1");
    h.add("x-a", "2");
    CHECK_EQ(h.count("X-A"), size_t{2});
    h.set("X-a", "3");
    CHECK_EQ(h.count("x-a"), size_t{1});
    CHECK_EQ(h.get("X-A"), std::string("3"));
}

TEST(http_server, serves_requests_over_tcp) {
    http::ServerOptions opts;
    opts.host = "127.0.0.1";
    opts.port = 0;
    opts.workers = 2;
    http::Server server(opts, [](http::Request& req) {
        tcrm::json::Value v = tcrm::json::Value::object();
        v.set("path", req.path);
        v.set("method", req.method_text);
        v.set("body", req.body);
        return http::Response::json(200, v);
    });
    std::string err;
    REQUIRE(server.start(&err));
    REQUIRE(server.port() > 0);

    url::Url target;
    REQUIRE(url::parse_url("http://127.0.0.1:" + std::to_string(server.port()) + "/echo?x=1", &target));
    http::Headers headers;
    headers.set("Content-Type", "application/json");
    http::ClientOptions copts;
    copts.timeout_ms = 3000;
    http::ClientResponse resp;
    REQUIRE(http::send_request(http::Method::Post, target, headers, "{\"hello\":1}", copts, &resp, &err));
    CHECK_EQ(resp.status, 200);
    const tcrm::json::Value body = tcrm::json::parse(resp.body);
    CHECK_EQ(body.get_string("path"), std::string("/echo"));
    CHECK_EQ(body.get_string("method"), std::string("POST"));
    CHECK_EQ(body.get_string("body"), std::string("{\"hello\":1}"));
    CHECK(!resp.headers.get("X-Request-Id").empty());

    url::Url bad;
    REQUIRE(url::parse_url("http://127.0.0.1:1/", &bad));
    CHECK(!http::send_request(http::Method::Get, bad, {}, "", copts, &resp, &err));

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    server.stop();
    CHECK(server.requests_served() >= 1);
}
