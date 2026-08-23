#include "ps2hdd/opl_asset_pipeline.hpp"

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

void check(bool condition, const char* message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}

std::vector<std::byte> bytes(std::string_view text)
{
    const auto* first = reinterpret_cast<const std::byte*>(text.data());
    return {first, first + text.size()};
}

class FakeHttp final : public ps2hdd::HttpClient {
public:
    ps2hdd::HttpResponse get(std::string_view url, std::size_t) override
    {
        ++calls;
        const auto it = responses.find(std::string(url));
        if (it == responses.end()) {
            return {404, {}, "text/plain", {}};
        }
        return it->second;
    }

    void text(std::string url, std::string body, std::string content_type = "text/plain")
    {
        responses[std::move(url)] = {200, bytes(body), std::move(content_type), {}};
    }

    std::map<std::string, ps2hdd::HttpResponse> responses;
    std::size_t calls{};
};

class TempDir final {
public:
    TempDir()
    {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        path = std::filesystem::temp_directory_path() /
               ("ps2-driveforge-opl-assets-" + std::to_string(stamp));
        std::filesystem::create_directories(path);
    }
    ~TempDir()
    {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }
    std::filesystem::path path;
};

ps2hdd::opl::AssetOptions none()
{
    ps2hdd::opl::AssetOptions options;
    options.title_database = false;
    options.redump_database = false;
    options.cfg_metadata = false;
    options.cfg_compatibility = false;
    options.widescreen = false;
    options.verified_mastercode = false;
    options.artwork_cover = false;
    options.artwork_icon = false;
    options.artwork_background = false;
    options.artwork_screenshot = false;
    options.artwork_logo = false;
    options.artwork_label = false;
    options.hdd_osd_icon = false;
    return options;
}

std::string read_text(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

void test_art_fallback()
{
    auto options = none();
    options.artwork_cover = true;
    const auto plan = ps2hdd::opl::plan_assets({"SLUS_209.46", "Game"}, options);
    check(plan.ok && plan.requests.size() == 1, "cover-only plan failed");

    FakeHttp http;
    const auto& request = plan.requests.front();
    check(request.candidates.size() == 2, "cover fallback provider missing");
    http.text(request.candidates[1].url, "JPEG", "image/jpeg");

    TempDir temp;
    const auto result = ps2hdd::opl::fetch_assets(plan, http, temp.path);
    check(result.ok, "optional cover fallback made pipeline fatal");
    check(result.assets.size() == 1, "cover fallback was not staged");
    check(result.assets[0].target_path == "ART/SLUS_209.46_COV.jpg",
          "fallback artwork kept the wrong target extension");
    check(read_text(result.assets[0].staged_path) == "JPEG", "staged fallback body mismatch");
}

void test_cfg_overlay()
{
    auto options = none();
    options.cfg_metadata = true;
    options.cfg_compatibility = true;
    const auto plan = ps2hdd::opl::plan_assets({"SCES_503.61", "Game"}, options);
    check(plan.ok && plan.requests.size() == 1, "CFG-only plan failed");

    FakeHttp http;
    const auto& request = plan.requests.front();
    http.text(request.candidates[0].url, "Title=Game\n$Compatibility=0x01\n");
    http.text(request.candidates[1].url, "$Compatibility=0x20\n$ConfigSource=GDX-X\n");

    TempDir temp;
    const auto result = ps2hdd::opl::fetch_assets(plan, http, temp.path);
    check(result.ok && result.assets.size() == 1, "CFG merge was not staged");
    check(result.assets[0].merged, "CFG asset not marked merged");
    const auto merged = read_text(result.assets[0].staged_path);
    check(merged.find("$Compatibility=0x20") != std::string::npos,
          "compatibility overlay did not win");
    check(merged.find("Title=Game") != std::string::npos, "base CFG metadata disappeared");
}

void test_mastercode_fallback()
{
    auto options = none();
    options.widescreen = true;
    options.verified_mastercode = true;
    const auto plan = ps2hdd::opl::plan_assets({"SLUS_212.15", "Game"}, options);
    check(plan.ok && plan.requests.size() == 1, "CHT-only plan failed");

    FakeHttp http;
    const auto& request = plan.requests.front();
    http.text(request.candidates[0].url,
              "\"Game /ID SLUS_212.15\"\n// Widescreen\n20100000 00000000\n");
    http.text(request.candidates[1].url,
              "\"Game /ID SLUS_212.15\"\nMastercode\n90123456 0C123456\n");

    TempDir temp;
    const auto result = ps2hdd::opl::fetch_assets(plan, http, temp.path);
    check(result.ok && result.assets.size() == 1, "CHT fallback was not staged");
    check(result.assets[0].verified_mastercode, "mastercode was not verified");
    check(result.assets[0].merged, "fallback mastercode was not marked merged");
    const auto merged = read_text(result.assets[0].staged_path);
    check(merged.find("90123456 0C123456") != std::string::npos,
          "mastercode payload missing from staged CHT");
}

void test_catalog_cache_reuse()
{
    auto options = none();
    options.title_database = true;
    const auto plan = ps2hdd::opl::plan_assets({"SLUS_209.46", "Game"}, options);
    check(plan.ok && plan.requests.size() == 1, "catalog-only plan failed");

    TempDir temp;
    const auto path = temp.path / "cache" / "catalog" / "gamename.csv";
    std::filesystem::create_directories(path.parent_path());
    {
        std::ofstream output(path, std::ios::binary);
        output << "SLUS_209.46;Cached title\n";
    }

    FakeHttp http;
    const auto result = ps2hdd::opl::fetch_assets(plan, http, temp.path);
    check(result.ok && result.assets.size() == 1, "cached catalog was not reused");
    check(result.assets[0].from_cache, "catalog asset was not marked cached");
    check(http.calls == 0, "catalog cache unexpectedly performed an HTTP request");
}

} // namespace

int main()
{
    try {
        test_art_fallback();
        test_cfg_overlay();
        test_mastercode_fallback();
        test_catalog_cache_reuse();
        std::cout << "OPL asset pipeline tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "OPL asset pipeline test failure: " << error.what() << '\n';
        return 1;
    }
}
