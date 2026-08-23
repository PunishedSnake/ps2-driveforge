#include "ps2hdd/http_client.hpp"

#include <algorithm>
#include <limits>
#include <memory>
#include <string>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winhttp.h>
#endif

namespace ps2hdd {

#ifdef _WIN32
namespace {

class InternetHandle final {
public:
    InternetHandle() = default;
    explicit InternetHandle(HINTERNET handle) : handle_(handle) {}
    ~InternetHandle()
    {
        if (handle_ != nullptr) {
            WinHttpCloseHandle(handle_);
        }
    }

    InternetHandle(const InternetHandle&) = delete;
    InternetHandle& operator=(const InternetHandle&) = delete;

    [[nodiscard]] HINTERNET get() const noexcept { return handle_; }
    [[nodiscard]] explicit operator bool() const noexcept { return handle_ != nullptr; }

private:
    HINTERNET handle_{};
};

std::wstring utf8_to_wide(std::string_view text)
{
    if (text.empty()) {
        return {};
    }
    if (text.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        return {};
    }
    const int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                            text.data(), static_cast<int>(text.size()),
                                            nullptr, 0);
    if (length <= 0) {
        return {};
    }
    std::wstring result(static_cast<std::size_t>(length), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                            text.data(), static_cast<int>(text.size()),
                            result.data(), length) != length) {
        return {};
    }
    return result;
}

std::string wide_to_utf8(std::wstring_view text)
{
    if (text.empty()) {
        return {};
    }
    if (text.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        return {};
    }
    const int length = WideCharToMultiByte(CP_UTF8, 0, text.data(),
                                           static_cast<int>(text.size()),
                                           nullptr, 0, nullptr, nullptr);
    if (length <= 0) {
        return {};
    }
    std::string result(static_cast<std::size_t>(length), '\0');
    if (WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                            result.data(), length, nullptr, nullptr) != length) {
        return {};
    }
    return result;
}

std::string winhttp_error(const char* operation)
{
    return std::string(operation) + " failed with WinHTTP error " +
           std::to_string(GetLastError());
}

class WinHttpClient final : public HttpClient {
public:
    HttpResponse get(std::string_view url, std::size_t max_bytes) override
    {
        HttpResponse response;
        if (url.empty() || max_bytes == 0) {
            response.error = "HTTP request has an empty URL or zero byte limit";
            return response;
        }

        const std::wstring wide_url = utf8_to_wide(url);
        if (wide_url.empty()) {
            response.error = "HTTP URL is not valid UTF-8";
            return response;
        }

        URL_COMPONENTS parts{};
        parts.dwStructSize = sizeof(parts);
        parts.dwSchemeLength = static_cast<DWORD>(-1);
        parts.dwHostNameLength = static_cast<DWORD>(-1);
        parts.dwUrlPathLength = static_cast<DWORD>(-1);
        parts.dwExtraInfoLength = static_cast<DWORD>(-1);
        if (!WinHttpCrackUrl(wide_url.c_str(), 0, 0, &parts)) {
            response.error = winhttp_error("WinHttpCrackUrl");
            return response;
        }
        if (parts.nScheme != INTERNET_SCHEME_HTTPS && parts.nScheme != INTERNET_SCHEME_HTTP) {
            response.error = "Only HTTP(S) asset URLs are supported";
            return response;
        }

        const std::wstring host(parts.lpszHostName, parts.dwHostNameLength);
        std::wstring path;
        if (parts.dwUrlPathLength != 0) {
            path.assign(parts.lpszUrlPath, parts.dwUrlPathLength);
        }
        if (parts.dwExtraInfoLength != 0) {
            path.append(parts.lpszExtraInfo, parts.dwExtraInfoLength);
        }
        if (path.empty()) {
            path = L"/";
        }

        InternetHandle session(WinHttpOpen(L"PS2 DriveForge/0.6 Frieren",
                                            WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                            WINHTTP_NO_PROXY_NAME,
                                            WINHTTP_NO_PROXY_BYPASS, 0));
        if (!session) {
            response.error = winhttp_error("WinHttpOpen");
            return response;
        }
        WinHttpSetTimeouts(session.get(), 10000, 10000, 15000, 30000);

        InternetHandle connection(WinHttpConnect(session.get(), host.c_str(), parts.nPort, 0));
        if (!connection) {
            response.error = winhttp_error("WinHttpConnect");
            return response;
        }

        const DWORD flags = parts.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0;
        InternetHandle request(WinHttpOpenRequest(connection.get(), L"GET", path.c_str(),
                                                   nullptr, WINHTTP_NO_REFERER,
                                                   WINHTTP_DEFAULT_ACCEPT_TYPES, flags));
        if (!request) {
            response.error = winhttp_error("WinHttpOpenRequest");
            return response;
        }

        if (!WinHttpSendRequest(request.get(), WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                WINHTTP_NO_REQUEST_DATA, 0, 0, 0)) {
            response.error = winhttp_error("WinHttpSendRequest");
            return response;
        }
        if (!WinHttpReceiveResponse(request.get(), nullptr)) {
            response.error = winhttp_error("WinHttpReceiveResponse");
            return response;
        }

        DWORD status = 0;
        DWORD status_size = sizeof(status);
        if (!WinHttpQueryHeaders(request.get(), WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                 WINHTTP_HEADER_NAME_BY_INDEX, &status, &status_size,
                                 WINHTTP_NO_HEADER_INDEX)) {
            response.error = winhttp_error("WinHttpQueryHeaders(status)");
            return response;
        }
        response.status = status;

        DWORD type_size = 0;
        WinHttpQueryHeaders(request.get(), WINHTTP_QUERY_CONTENT_TYPE,
                            WINHTTP_HEADER_NAME_BY_INDEX, WINHTTP_NO_OUTPUT_BUFFER,
                            &type_size, WINHTTP_NO_HEADER_INDEX);
        if (GetLastError() == ERROR_INSUFFICIENT_BUFFER && type_size >= sizeof(wchar_t)) {
            std::wstring type(type_size / sizeof(wchar_t), L'\0');
            if (WinHttpQueryHeaders(request.get(), WINHTTP_QUERY_CONTENT_TYPE,
                                    WINHTTP_HEADER_NAME_BY_INDEX, type.data(), &type_size,
                                    WINHTTP_NO_HEADER_INDEX)) {
                if (!type.empty() && type.back() == L'\0') {
                    type.pop_back();
                }
                response.content_type = wide_to_utf8(type);
            }
        }

        for (;;) {
            DWORD available = 0;
            if (!WinHttpQueryDataAvailable(request.get(), &available)) {
                response.error = winhttp_error("WinHttpQueryDataAvailable");
                response.body.clear();
                return response;
            }
            if (available == 0) {
                break;
            }
            if (response.body.size() > max_bytes ||
                static_cast<std::size_t>(available) > max_bytes - response.body.size()) {
                response.error = "HTTP asset exceeded the configured download size limit";
                response.body.clear();
                return response;
            }

            const std::size_t old_size = response.body.size();
            response.body.resize(old_size + available);
            DWORD read = 0;
            if (!WinHttpReadData(request.get(), response.body.data() + old_size, available, &read)) {
                response.error = winhttp_error("WinHttpReadData");
                response.body.clear();
                return response;
            }
            response.body.resize(old_size + read);
            if (read == 0) {
                break;
            }
        }

        return response;
    }
};

} // namespace
#endif

std::unique_ptr<HttpClient> make_platform_http_client()
{
#ifdef _WIN32
    return std::make_unique<WinHttpClient>();
#else
    return {};
#endif
}

} // namespace ps2hdd
