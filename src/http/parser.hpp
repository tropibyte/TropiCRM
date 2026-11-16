// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tropibyte, Inc.

#pragma once

#include <cstddef>
#include <string>

#include "http/message.hpp"

namespace tcrm::http {

struct ParserLimits {
    size_t max_header_bytes = 64 * 1024;
    size_t max_headers = 100;
    size_t max_body_bytes = 10 * 1024 * 1024;
};

/// Incremental HTTP/1.1 request parser. Feed it the connection's read
/// buffer; it consumes bytes from the front as it makes progress.
class RequestParser {
public:
    enum class Result { NeedMore, Complete, Error };

    explicit RequestParser(ParserLimits limits = {}) : limits_(limits) {}

    Result parse(std::string& buffer);

    /// The finished request. Valid after parse() returned Complete; resets
    /// the parser for the next request on the connection.
    Request take();

    /// For Error: the status to send (400, 413, 431, 501) and a message.
    int error_status() const { return error_status_; }
    const std::string& error_message() const { return error_message_; }

private:
    enum class State { Headers, Body, ChunkSize, ChunkData, ChunkTrailer, Done };

    Result fail(int status, std::string message);
    bool parse_head(std::string_view head);

    ParserLimits limits_;
    State state_ = State::Headers;
    Request request_;
    size_t content_length_ = 0;
    size_t chunk_remaining_ = 0;
    int error_status_ = 400;
    std::string error_message_;
};

}  // namespace tcrm::http
