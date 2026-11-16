// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tropibyte, Inc.

// In-process API harness: an Application with in-memory storage, driven
// through Application::handle() without sockets.

#pragma once

#include <string>
#include <utility>
#include <vector>

#include "app/application.hpp"
#include "json/json.hpp"
#include "testing.hpp"

namespace tcrm::testing {

/// Test addresses are assembled at run time so the sources contain no
/// literal email addresses.
inline std::string address(const std::string& local, const std::string& domain = "acme") {
    return local + "@" + domain + ".test";
}

inline Config test_config() {
    Config c = Config::defaults();
    c.set("data.dir", "");
    c.set("auth.pbkdf2_iterations", "1000");
    c.set("log.requests", "false");
    c.set("log.level", "off");
    c.set("webhooks.workers", "1");
    c.set("limits.public_requests_per_minute", "100000");
    return c;
}

class TestApp {
public:
    struct Resp {
        int status = 0;
        json::Value json;
        std::string body;
        http::Headers headers;
    };

    explicit TestApp(Config config = test_config()) : app(std::move(config)) {
        std::string err;
        if (!app.init(&err)) throw std::runtime_error("init failed: " + err);
    }

    Resp call(const std::string& method, const std::string& target, const std::string& body = {},
              const std::string& token = {}, const std::vector<std::pair<std::string, std::string>>& headers = {}) {
        http::Request req;
        req.method_text = method;
        req.method = http::parse_method(method);
        req.target = target;
        const size_t q = target.find('?');
        req.path = target.substr(0, q);
        if (q != std::string::npos) {
            req.query_string = target.substr(q + 1);
            req.query = url::parse_query(req.query_string);
        }
        req.body = body;
        req.remote_addr = "127.0.0.1:50000";
        req.request_id = "test-" + std::to_string(++counter_);
        if (!body.empty()) req.headers.set("Content-Type", "application/json");
        if (!token.empty()) req.headers.set("Authorization", "Bearer " + token);
        for (const auto& [k, v] : headers) req.headers.set(k, v);
        http::Response resp = app.handle(req);
        Resp out;
        out.status = resp.status;
        out.body = resp.body;
        out.headers = resp.headers;
        if (!resp.body.empty()) json::try_parse(resp.body, &out.json);
        return out;
    }

    Resp get(const std::string& target, const std::string& token) { return call("GET", target, {}, token); }
    Resp post(const std::string& target, const json::Value& body, const std::string& token = {}) {
        return call("POST", target, body.dump(), token);
    }
    Resp patch(const std::string& target, const json::Value& body, const std::string& token) {
        return call("PATCH", target, body.dump(), token);
    }
    Resp put(const std::string& target, const json::Value& body, const std::string& token) {
        return call("PUT", target, body.dump(), token);
    }
    Resp del(const std::string& target, const std::string& token) { return call("DELETE", target, {}, token); }

    /// Creates an org through self-service signup; returns its id.
    std::string signup(const std::string& company, const std::string& username, const std::string& password) {
        json::Value body = json::Value::object();
        body.set("company", company);
        body.set("username", username);
        body.set("firstName", "Test");
        body.set("lastName", "Admin");
        body.set("password", password);
        Resp r = post("/services/signup", body);
        if (r.status != 201) throw std::runtime_error("signup failed: " + r.body);
        return r.json.get_string("orgId");
    }

    std::string login(const std::string& username, const std::string& password) {
        Resp r = call("POST", "/services/oauth2/token",
                      "grant_type=password&username=" + url::percent_encode(username) +
                          "&password=" + url::percent_encode(password),
                      {}, {{"Content-Type", "application/x-www-form-urlencoded"}});
        if (r.status != 200) throw std::runtime_error("login failed: " + r.body);
        return r.json.get_string("access_token");
    }

    /// Creates a record and returns its id; fails the test on error.
    std::string create(const std::string& token, const std::string& object, const json::Value& fields) {
        Resp r = post("/services/data/v1/sobjects/" + object, fields, token);
        if (r.status != 201) {
            record_failure(__FILE__, __LINE__, "create " + object + " failed: " + std::to_string(r.status) + " " + r.body);
            throw AbortTest("create failed");
        }
        return r.json.get_string("id");
    }

    /// Creates a user in the admin's org with the given profile; returns the id.
    std::string create_user(const std::string& admin_token, const std::string& username, const std::string& last_name,
                            const std::string& profile, const std::string& password, const std::string& role_id = {}) {
        json::Value body = json::Value::object();
        body.set("username", username);
        body.set("lastName", last_name);
        body.set("firstName", "T");
        body.set("profileName", profile);
        body.set("password", password);
        if (!role_id.empty()) body.set("roleId", role_id);
        Resp r = post("/services/data/v1/users", body, admin_token);
        if (r.status != 201) {
            record_failure(__FILE__, __LINE__, "create user failed: " + r.body);
            throw AbortTest("create user failed");
        }
        return r.json.get_string("id");
    }

    Resp query(const std::string& tql, const std::string& token) {
        return get("/services/data/v1/query?q=" + url::percent_encode(tql), token);
    }

    Application app;

private:
    int counter_ = 0;
};

/// A ready-made org with an admin, for tests that just need data.
struct Fixture {
    TestApp api;
    std::string org_id;
    std::string admin_user;
    std::string admin_token;
    static constexpr const char* kPassword = "Sunrise2026x";

    Fixture() {
        admin_user = address("admin");
        org_id = api.signup("Acme Corp", admin_user, kPassword);
        admin_token = api.login(admin_user, kPassword);
    }

    json::Value obj(std::initializer_list<std::pair<const char*, json::Value>> fields) const {
        json::Value v = json::Value::object();
        for (const auto& [k, val] : fields) v.set(k, val);
        return v;
    }
};

}  // namespace tcrm::testing
