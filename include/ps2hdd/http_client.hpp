#pragma once

#include <cstddef>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ps2hdd {

struct HttpResponse {
    unsigned status{};
    std::vector<std::byte> body;
    std::string content_type;
    std::string error;

    [[nodiscard]] bool ok() const noexcept
    {
        return error.empty() && status >= 200 && status < 300;
    }
};

class HttpClient {
public:
    virtual ~HttpClient() = default;
    virtual HttpResponse get(std::string_view url, std::size_t max_bytes) = 0;
};

// Returns the native HTTPS implementation on supported desktop targets. Linux
// sanitizer builds intentionally return nullptr; tests inject a deterministic
// fake transport and therefore never depend on the public internet.
[[nodiscard]] std::unique_ptr<HttpClient> make_platform_http_client();

} // namespace ps2hdd
