#pragma once
// Minimal HTTP/1.1 request parsing and static-file helpers for scuba-hostd. Request bodies are not
// supported (GET/HEAD only); every response closes the connection.
#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace scuba::hostd {

struct HttpRequest {
    std::string method;
    std::string target;     // as sent: "/assets/a.js?v=1"
    std::string path;       // percent-decoded path without the query: "/assets/a.js"
    std::string version;    // "HTTP/1.1"
    std::vector<std::pair<std::string, std::string>> headers;   // names lower-cased

    // The first header with this (lower-case) name, or nullptr.
    const std::string* header(std::string_view lowerName) const;
    // True when a comma-separated header contains token (case-insensitive).
    bool header_has_token(std::string_view lowerName, std::string_view token) const;
};

enum class HttpParse { Incomplete, Done, Error };

constexpr size_t kMaxHttpHead = 16 * 1024;

// Parses the request head at the start of buffer. Done: `consumed` is the head length including
// the blank line. Error: malformed, a body announced, or the head exceeds kMaxHttpHead.
HttpParse ParseHttpRequest(std::string_view buffer, HttpRequest& out, size_t& consumed);

std::string HttpResponse(int status, std::string_view contentType, std::string_view body,
                         const std::vector<std::pair<std::string, std::string>>& extraHeaders = {},
                         bool headOnly = false);

std::string_view HttpStatusText(int status);
std::string_view MimeTypeFor(const std::filesystem::path& file);

// Maps a decoded URL path onto a file under root. "/" maps to "/index.html". Refuses (nullopt)
// any path with "..", backslashes, NUL or a drive/root component, so nothing outside root is
// reachable. The file is not required to exist.
std::optional<std::filesystem::path> ResolveStaticPath(const std::filesystem::path& root, std::string_view urlPath);

}  // namespace scuba::hostd
