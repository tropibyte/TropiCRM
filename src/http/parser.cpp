// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tropibyte, Inc.

#include "http/parser.hpp"

#include "util/strutil.hpp"

namespace tcrm::http {
namespace {

bool is_token_char(char c) {
    if (c >= 'a' && c <= 'z') return true;
    if (c >= 'A' && c <= 'Z') return true;
    if (c >= '0' && c <= '9') return true;
    switch (c) {
        case '!': case '#': case '$': case '%': case '&': case '\'': case '*': case '+':
        case '-': case '.': case '^': case '_': case '`': case '|': case '~':
            return true;
        default:
            return false;
    }
}

bool parse_hex_size(std::string_view s, size_t* out) {
    s = str::trim(s);
    const size_t semi = s.find(';');  // chunk extensions are ignored
    if (semi != std::string_view::npos) s = str::trim(s.substr(0, semi));
    if (s.empty() || s.size() > 15) return false;
    size_t v = 0;
    for (char c : s) {
        int d = -1;
        if (c >= '0' && c <= '9') d = c - '0';
        else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
        if (d < 0) return false;
        v = v * 16 + static_cast<size_t>(d);
    }
    *out = v;
    return true;
}

}  // namespace

RequestParser::Result RequestParser::fail(int status, std::string message) {
    error_status_ = status;
    error_message_ = std::move(message);
    return Result::Error;
}

bool RequestParser::parse_head(std::string_view head) {
    const size_t line_end = head.find("\r\n");
    const std::string_view request_line = head.substr(0, line_end);
    const size_t sp1 = request_line.find(' ');
    const size_t sp2 = request_line.rfind(' ');
    if (sp1 == std::string_view::npos || sp2 == sp1) return false;
    request_.method_text = std::string(request_line.substr(0, sp1));
    request_.method = parse_method(request_.method_text);
    request_.target = std::string(request_line.substr(sp1 + 1, sp2 - sp1 - 1));
    request_.version = std::string(request_line.substr(sp2 + 1));
    if (request_.version != "HTTP/1.1" && request_.version != "HTTP/1.0") return false;
    if (request_.target.empty() || (request_.target[0] != '/' && request_.target != "*")) return false;

    const size_t q = request_.target.find('?');
    request_.path = request_.target.substr(0, q);
    if (q != std::string::npos) {
        request_.query_string = request_.target.substr(q + 1);
        request_.query = url::parse_query(request_.query_string);
    }

    size_t pos = line_end == std::string_view::npos ? head.size() : line_end + 2;
    while (pos < head.size()) {
        size_t end = head.find("\r\n", pos);
        if (end == std::string_view::npos) end = head.size();
        const std::string_view line = head.substr(pos, end - pos);
        pos = end + 2;
        if (line.empty()) continue;
        const size_t colon = line.find(':');
        if (colon == std::string_view::npos || colon == 0) return false;
        const std::string_view name = line.substr(0, colon);
        for (char c : name) {
            if (!is_token_char(c)) return false;
        }
        request_.headers.add(std::string(name), std::string(str::trim(line.substr(colon + 1))));
        if (request_.headers.size() > limits_.max_headers) return false;
    }
    return true;
}

RequestParser::Result RequestParser::parse(std::string& buffer) {
    while (true) {
        switch (state_) {
            case State::Headers: {
                // Tolerate stray CRLFs between pipelined requests.
                while (buffer.size() >= 2 && buffer[0] == '\r' && buffer[1] == '\n') buffer.erase(0, 2);
                const size_t end = buffer.find("\r\n\r\n");
                if (end == std::string::npos) {
                    if (buffer.size() > limits_.max_header_bytes) return fail(431, "request header too large");
                    return Result::NeedMore;
                }
                if (end > limits_.max_header_bytes) return fail(431, "request header too large");
                if (!parse_head(std::string_view(buffer).substr(0, end))) return fail(400, "malformed request");
                buffer.erase(0, end + 4);

                const std::string cl = request_.headers.get("Content-Length");
                const std::string te = str::to_lower(request_.headers.get("Transfer-Encoding"));
                if (!cl.empty()) {
                    int64_t n = 0;
                    if (!str::parse_int64(cl, &n) || n < 0) return fail(400, "invalid Content-Length");
                    if (static_cast<uint64_t>(n) > limits_.max_body_bytes) return fail(413, "request body too large");
                    content_length_ = static_cast<size_t>(n);
                    state_ = content_length_ == 0 ? State::Done : State::Body;
                } else if (!te.empty()) {
                    if (te != "chunked") return fail(501, "unsupported transfer encoding");
                    state_ = State::ChunkSize;
                } else {
                    state_ = State::Done;
                }
                break;
            }
            case State::Body: {
                if (buffer.size() < content_length_) return Result::NeedMore;
                request_.body = buffer.substr(0, content_length_);
                buffer.clear();
                state_ = State::Done;
                break;
            }
            case State::ChunkSize: {
                const size_t eol = buffer.find("\r\n");
                if (eol == std::string::npos) {
                    if (buffer.size() > 1024) return fail(400, "chunk size line too long");
                    return Result::NeedMore;
                }
                size_t size = 0;
                if (!parse_hex_size(std::string_view(buffer).substr(0, eol), &size)) {
                    return fail(400, "invalid chunk size");
                }
                if (size > limits_.max_body_bytes) return fail(413, "request body too large");
                buffer.erase(0, eol + 2);
                chunk_remaining_ = size;
                state_ = size == 0 ? State::ChunkTrailer : State::ChunkData;
                break;
            }
            case State::ChunkData: {
                if (buffer.size() < chunk_remaining_ + 2) return Result::NeedMore;
                if (buffer.compare(chunk_remaining_, 2, "\r\n") != 0) return fail(400, "missing CRLF after chunk");
                request_.body.append(buffer, 0, chunk_remaining_);
                buffer.erase(0, chunk_remaining_ + 2);
                state_ = State::ChunkSize;
                break;
            }
            case State::ChunkTrailer: {
                const size_t eol = buffer.find("\r\n");
                if (eol == std::string::npos) return Result::NeedMore;
                buffer.erase(0, eol + 2);
                if (eol == 0) state_ = State::Done;  // blank line ends the trailer
                break;
            }
            case State::Done:
                return Result::Complete;
        }
    }
}

Request RequestParser::take() {
    Request out = std::move(request_);
    request_ = Request();
    state_ = State::Headers;
    content_length_ = 0;
    chunk_remaining_ = 0;
    return out;
}

}  // namespace tcrm::http
