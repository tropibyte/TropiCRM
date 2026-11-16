// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tropibyte, Inc.

#pragma once

#include <stdexcept>
#include <string>
#include <vector>

namespace tcrm {

/// An error reported to API clients as
///   [{"errorCode": "...", "message": "...", "fields": [...]}]
/// with the given HTTP status.
class ApiError : public std::runtime_error {
public:
    ApiError(int status, std::string code, const std::string& message, std::vector<std::string> fields = {})
        : std::runtime_error(message), status_(status), code_(std::move(code)), fields_(std::move(fields)) {}

    int status() const { return status_; }
    const std::string& code() const { return code_; }
    const std::vector<std::string>& fields() const { return fields_; }

private:
    int status_;
    std::string code_;
    std::vector<std::string> fields_;
};

namespace errors {

inline ApiError bad_request(const std::string& code, const std::string& message, std::vector<std::string> fields = {}) {
    return ApiError(400, code, message, std::move(fields));
}

inline ApiError invalid_field(const std::string& field, const std::string& message) {
    return ApiError(400, "INVALID_FIELD", message, {field});
}

inline ApiError not_found(const std::string& message = "The requested resource does not exist") {
    return ApiError(404, "NOT_FOUND", message);
}

inline ApiError entity_deleted(const std::string& message = "entity is deleted") {
    return ApiError(404, "ENTITY_IS_DELETED", message);
}

inline ApiError unauthorized(const std::string& message = "Session expired or invalid") {
    return ApiError(401, "INVALID_SESSION_ID", message);
}

inline ApiError forbidden(const std::string& message = "insufficient access rights on object id") {
    return ApiError(403, "INSUFFICIENT_ACCESS", message);
}

inline ApiError conflict(const std::string& code, const std::string& message) {
    return ApiError(409, code, message);
}

inline ApiError limit_exceeded(const std::string& message) {
    return ApiError(429, "REQUEST_LIMIT_EXCEEDED", message);
}

inline ApiError malformed_query(const std::string& message) {
    return ApiError(400, "MALFORMED_QUERY", message);
}

inline ApiError internal(const std::string& message) {
    return ApiError(500, "UNKNOWN_EXCEPTION", message);
}

}  // namespace errors
}  // namespace tcrm
