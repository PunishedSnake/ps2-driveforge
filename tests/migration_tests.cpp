#include "apa_fixture.hpp"

#include "ps2hdd/apa_migrate.hpp"
#include "ps2hdd/boot_chain.hpp"
#include "ps2hdd/file_block_device.hpp"
#include "ps2hdd/structure_health.hpp"

#include <iostream>
#include <stop_token>

namespace {

using fixture::kChunk;
using fixture::require;
namespace mg = ps2hdd::migrate;

std::filesystem::path dir(const char* name)
{
    const auto path = std::filesystem::temp_directory_path() / (std::string("ps2df-migrate-") + name);
    std::filesystem::remove_all(path);
    std::filesystem::create_directories(path);
    return path;
}

constexpr std::uint64_t chunks(std::uint64_t n) { return n * kChunk * ps2hdd::apa::kSectorSize; }

std::filesystem::path make_source(const std::filesystem::path& root)
{
    const auto image = root / "source.img";
    fixture::Spec spec{{"__sysconf", "__system", "+OPL"}, 8};
    spec.empty_chunks = 2;
    fixture::build(image, spec);
    fixture::put_files(image, "__sysconf", {{"/osdmenu/OSDMBR.CNF", "boot_auto = $HOSDSYS\n"}});
    fixture::put_files(image, "__system", {{"/osdmenu/hosdmenu.elf", "hosd"}});
    fixture::put_files(image, "+OPL", {{"/CFG/SLUS_123.45.cfg", "Title=Migrated\n"}});
    // Garbage inside the __empty payload must NOT be copied.
    std::fstream io(image, std::ios::binary | std::ios::in | std::ios::out);
    io.seekp(static_cast<std::streamoff>(chunks(4) + 1U * 1024U * 1024U));
    io.write("STALE", 5);
    return image;
}

bool same_range(const std::filesystem::path& a, const std::filesystem::path& b, std::uint64_t offset, std::uint64_t bytes)
{
    std::ifstream fa(a, std::ios::binary), fb(b, std::ios::binary);
    std::vector<char> ba(1 << 20), bb(1 << 20);
    while (bytes > 0) {
        const auto take = static_cast<std::streamsize>(std::min<std::uint64_t>(bytes, ba.size()));
        fa.seekg(static_cast<std::streamoff>(offset));
        fb.seekg(static_cast<std::streamoff>(offset));
        fa.read(ba.data(), take);
        fb.read(bb.data(), take);
        if (!std::equal(ba.begin(), ba.begin() + take, bb.begin())) return false;
        offset += static_cast<std::uint64_t>(take);
        bytes -= static_cast<std::uint64_t>(take);
    }
    return true;
}

void migrate_to_bigger_image()
{
    const auto root = dir("bigger");
    const auto source_path = make_source(root);
    ps2hdd::FileBlockDevice source(source_path);
    require(source.is_open(), "open source");

    const auto plan = mg::plan(source, chunks(16));
    if (!plan.ok) std::cerr << plan.error << '\n';
    require(plan.ok && plan.partitions == 4 && plan.required_bytes == chunks(6), "plan covers mbr + 3 PFS + __empty");
    require(plan.ranges.size() == 1 && plan.ranges[0].bytes == chunks(4) + 1024 && plan.copy_bytes == chunks(4) + 1024,
            "contiguous partitions merged, __empty copied header-only");
    require(plan.gained_bytes() == static_cast<std::int64_t>(chunks(8)), "8 chunks gained");

    const auto target_path = root / "target.img";
    std::string error;
    require(mg::create_sparse_image(target_path, chunks(16), error), "create sparse target");
    require(!mg::create_sparse_image(target_path, chunks(16), error), "never overwrite an existing image");

    ps2hdd::WritableFileBlockDevice target(target_path);
    require(target.is_open(), "open target");
    mg::Options options;
    options.target_is_zeroed = true;
    std::uint64_t last = 0;
    const auto result = mg::apply(source, target, plan, options, [&](std::uint64_t done, std::uint64_t) { last = done; });
    if (!result.ok) std::cerr << result.error << '\n';
    require(result.ok && result.verified && result.master_written, "migration ok and verified");
    require(last == plan.copy_bytes && result.skipped_zero_bytes > 0, "progress complete; zero chunks kept sparse");

    for (const auto& range : plan.ranges)
        require(same_range(source_path, target_path, range.offset, range.bytes), "byte-identical ranges");
    require(!same_range(source_path, target_path, chunks(4) + 1024U * 1024U, 5), "__empty payload was not copied");

    ps2hdd::FileBlockDevice check_target(target_path);
    const auto health = ps2hdd::health::assess(check_target);
    require(health.verdict == ps2hdd::health::Verdict::healthy && health.partitions == 5, "migrated disk is healthy");
    const auto boot = ps2hdd::bootchain::inspect(check_target);
    require(boot.family == "HDD-OSD / OSDMenu MBR", "boot chain survives migration");
}

void refusals_and_cancellation()
{
    const auto root = dir("refusals");
    const auto source_path = make_source(root);
    ps2hdd::FileBlockDevice source(source_path);
    require(!mg::plan(source, chunks(5)).ok, "target smaller than required is refused");
    const auto smaller = mg::plan(source, chunks(6));
    require(smaller.ok && !smaller.warnings.empty(), "smaller-but-sufficient target allowed with a warning");

    // Cancelled before the first chunk: no master, target is not an APA disk.
    const auto target_path = root / "cancel.img";
    std::string error;
    require(mg::create_sparse_image(target_path, chunks(8), error), "create target");
    {
        ps2hdd::WritableFileBlockDevice target(target_path);
        std::stop_source stop;
        stop.request_stop();
        const auto result = mg::apply(source, target, mg::plan(source, chunks(8)), {}, {}, stop.get_token());
        require(result.cancelled && !result.master_written, "cancel leaves no master");
        ps2hdd::apa::Reader reader(target);
        require(!reader.scan().ok(), "cancelled target is not an APA disk");
    }

    // Stale plan: the source master changes between plan and apply.
    const auto stale = mg::plan(source, chunks(8));
    {
        std::fstream io(source_path, std::ios::binary | std::ios::in | std::ios::out);
        io.seekp(0x200);
        io.put('X');
    }
    ps2hdd::FileBlockDevice changed(source_path);
    ps2hdd::WritableFileBlockDevice target(target_path);
    const auto refused = mg::apply(changed, target, stale);
    require(!refused.ok && refused.error.find("changed") != std::string::npos, "stale plan refused");
    require(!mg::plan(changed, chunks(8)).ok, "damaged source (checksum) is refused");
}

} // namespace

int main()
{
    try {
        migrate_to_bigger_image();
        refusals_and_cancellation();
    } catch (const std::exception& error) {
        std::cerr << "migration test failed: " << error.what() << '\n';
        return 1;
    }
    std::cout << "Migration tests passed\n";
    return 0;
}
