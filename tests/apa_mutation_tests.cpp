#include "ps2hdd/apa_mutation.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <map>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

void check(bool condition, const char* message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}

class SparseWritableDevice final : public ps2hdd::WritableBlockDevice {
public:
    explicit SparseWritableDevice(std::uint64_t size) : size_(size) {}

    std::uint64_t size_bytes() const override { return size_; }
    std::string display_name() const override { return "apa-mutation-sparse"; }

    bool read(std::uint64_t offset, std::span<std::byte> out) override
    {
        const auto it = chunks_.find(offset);
        if (it == chunks_.end() || it->second.size() != out.size()) {
            return false;
        }
        std::memcpy(out.data(), it->second.data(), out.size());
        return true;
    }

    bool write(std::uint64_t offset, std::span<const std::byte> in) override
    {
        ++write_calls;
        if (offset > size_ || in.size() > size_ - offset) {
            return false;
        }
        chunks_[offset] = std::vector<std::byte>(in.begin(), in.end());
        if (corrupt_write_call != 0 && write_calls == corrupt_write_call && !in.empty()) {
            chunks_[offset][0] ^= std::byte{0x20};
        }
        return true;
    }

    bool flush() override
    {
        ++flush_calls;
        return true;
    }

    void put(std::uint64_t offset, std::span<const std::byte> bytes)
    {
        chunks_[offset] = std::vector<std::byte>(bytes.begin(), bytes.end());
    }

    template <typename T>
    void put_object(std::uint64_t offset, const T& value)
    {
        put(offset, std::as_bytes(std::span{&value, 1}));
    }

    std::size_t write_calls{};
    std::size_t flush_calls{};
    std::size_t corrupt_write_call{};

private:
    std::uint64_t size_{};
    std::map<std::uint64_t, std::vector<std::byte>> chunks_;
};

ps2hdd::apa::Header make_header(const char* id, std::uint16_t type,
                                std::uint32_t start, std::uint32_t length,
                                std::uint32_t prev, std::uint32_t next)
{
    ps2hdd::apa::Header header{};
    header.magic = ps2hdd::apa::kMagic;
    std::strncpy(header.id, id, sizeof(header.id) - 1);
    header.type = type;
    header.start = start;
    header.length = length;
    header.prev = prev;
    header.next = next;
    header.mbr.version = 2;
    header.checksum = ps2hdd::apa::checksum(header);
    return header;
}

struct Fixture {
    SparseWritableDevice device;
    ps2hdd::apa::AllocationPlan allocation;
    ps2hdd::apa::HdlHeaderPlan headers;
    ps2hdd::apa::Header original_mbr;
    ps2hdd::apa::Header original_tail;

    Fixture()
        : device(static_cast<std::uint64_t>(8) * ps2hdd::apa::kAllocationChunkSectors *
                 ps2hdd::apa::kSectorSize)
    {
        const auto chunk = ps2hdd::apa::kAllocationChunkSectors;
        original_mbr = make_header("__mbr", ps2hdd::apa::kTypeMbr, 0, chunk, chunk, chunk);
        constexpr char sony[] = "Sony Computer Entertainment Inc.";
        std::memcpy(original_mbr.mbr.magic, sony, sizeof(sony) - 1);
        original_mbr.checksum = ps2hdd::apa::checksum(original_mbr);
        original_tail = make_header("+OPL", ps2hdd::apa::kTypePfs, chunk, chunk, 0, 0);

        device.put_object(0, original_mbr);
        device.put_object(static_cast<std::uint64_t>(chunk) * ps2hdd::apa::kSectorSize,
                          original_tail);

        // Free target chunk still has arbitrary old bytes. Transaction staging
        // must capture them before writing the new header.
        std::vector<std::byte> free_header(ps2hdd::apa::kHeaderSize, std::byte{0xA5});
        device.put(static_cast<std::uint64_t>(2 * chunk) * ps2hdd::apa::kSectorSize,
                   free_header);

        allocation.ok = true;
        allocation.extents.push_back({2 * chunk, chunk, chunk, 0});
        allocation.existing_link_updates = {
            {0, chunk, chunk, 2 * chunk, chunk},
            {chunk, 0, 0, 0, 2 * chunk},
        };
        allocation.allocated_bytes = 128ULL * 1024ULL * 1024ULL;
        allocation.overhead_bytes = 4ULL * 1024ULL * 1024ULL;
        allocation.usable_payload_bytes = 124ULL * 1024ULL * 1024ULL;

        headers = ps2hdd::apa::build_hdl_headers(allocation, "PP.SLUS-12345..TEST", {});
        check(headers.ok, "fixture new HDL header build failed");
    }
};

void test_staging_is_read_only_and_commit_forms_valid_chain()
{
    Fixture fixture;
    ps2hdd::WriteTransaction transaction(fixture.device);
    const auto stage_error = ps2hdd::apa::stage_hdl_header_publication(
        fixture.device, fixture.allocation, fixture.headers, transaction);
    check(stage_error.empty(), "APA publication staging should succeed");
    check(fixture.device.write_calls == 0, "staging must not write the device");
    check(transaction.staged_writes().size() == 3,
          "expected new header plus two existing link rewrites");

    const auto commit = transaction.commit([&]() -> std::string {
        ps2hdd::apa::Reader reader(fixture.device);
        const auto scan = reader.scan();
        if (!scan.ok()) {
            return "published APA chain did not re-scan cleanly";
        }
        if (scan.partitions.size() != 3 || scan.partitions.back().type != ps2hdd::apa::kTypeHdl) {
            return "published HDL partition was not visible in the expected chain";
        }
        return {};
    });
    check(commit.ok, "staged APA publication should commit and verify");
    check(fixture.device.write_calls == 3, "publication should write exactly three staged headers");
    check(fixture.device.flush_calls == 1, "publication should flush once");
}

void test_failed_publication_restores_old_chain()
{
    Fixture fixture;
    ps2hdd::WriteTransaction transaction(fixture.device);
    const auto stage_error = ps2hdd::apa::stage_hdl_header_publication(
        fixture.device, fixture.allocation, fixture.headers, transaction);
    check(stage_error.empty(), "APA publication staging should succeed");

    // Corrupt the final forward write (old tail link update). Read-back then
    // fails and WriteTransaction restores all three before-images.
    fixture.device.corrupt_write_call = 3;
    const auto commit = transaction.commit();
    check(!commit.ok && commit.rollback_attempted && commit.rollback_ok,
          "corrupted APA publication should roll back every staged header");

    ps2hdd::apa::Reader reader(fixture.device);
    const auto scan = reader.scan();
    check(scan.ok(), "old APA chain should remain valid after rollback");
    check(scan.partitions.size() == 2, "rolled-back chain should not expose the new HDL partition");
}

void test_stale_neighbour_links_are_refused_before_write()
{
    Fixture fixture;
    auto changed = fixture.original_tail;
    changed.next = 7 * ps2hdd::apa::kAllocationChunkSectors;
    changed.checksum = ps2hdd::apa::checksum(changed);
    fixture.device.put_object(
        static_cast<std::uint64_t>(ps2hdd::apa::kAllocationChunkSectors) * ps2hdd::apa::kSectorSize,
        changed);

    ps2hdd::WriteTransaction transaction(fixture.device);
    const auto error = ps2hdd::apa::stage_hdl_header_publication(
        fixture.device, fixture.allocation, fixture.headers, transaction);
    check(!error.empty(), "stale APA neighbour should invalidate publication staging");
    check(fixture.device.write_calls == 0, "stale-plan refusal must not write");
}

} // namespace

int main()
{
    try {
        test_staging_is_read_only_and_commit_forms_valid_chain();
        test_failed_publication_restores_old_chain();
        test_stale_neighbour_links_are_refused_before_write();
        std::cout << "APA mutation tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "APA mutation tests failed: " << error.what() << '\n';
        return 1;
    }
}
