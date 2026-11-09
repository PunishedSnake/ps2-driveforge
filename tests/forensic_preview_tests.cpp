#include "apa_fixture.hpp"

#include "ps2hdd/apa_recovery_apply.hpp"
#include "ps2hdd/file_block_device.hpp"
#include "ps2hdd/forensic_preview.hpp"

#include <iostream>

namespace {

using fixture::require;

void flip_next_bit(const std::filesystem::path& image, std::uint32_t lba)
{
    std::fstream io(image, std::ios::binary | std::ios::in | std::ios::out);
    const auto offset = static_cast<std::streamoff>(std::uint64_t{lba} * 512U + 8U);  // Header::next
    io.seekg(offset);
    char byte = 0;
    io.read(&byte, 1);
    byte = static_cast<char>(byte ^ 0x01);
    io.seekp(offset);
    io.write(&byte, 1);
}

} // namespace

int main()
{
    try {
        const auto root = std::filesystem::temp_directory_path() / "ps2df-forensic-preview";
        std::filesystem::remove_all(root);
        std::filesystem::create_directories(root);
        const auto image = root / "disk.img";
        const auto lbas = fixture::build(image, {{"__sysconf", "__system", "+OPL"}, 6});
        flip_next_bit(image, lbas[1]);  // __sysconf.next now points nowhere; checksum stale

        ps2hdd::FileBlockDevice disk(image);
        require(disk.is_open(), "open image");
        {
            ps2hdd::apa::Reader reader(disk);
            require(!reader.scan().ok(), "normal reader refuses the broken chain");
        }
        const auto scan = ps2hdd::forensic::scan_apa(disk);
        require(scan.ok && !scan.maps.empty(), "forensic maps found");

        bool found_safe = false;
        for (std::size_t i = 0; i < scan.maps.size(); ++i) {
            const auto preview = ps2hdd::forensic::preview(scan, i);
            std::cout << ps2hdd::forensic::render_preview(preview);
            if (preview.safety != "automatic-safe") continue;
            found_safe = true;
            require(preview.rows.size() == 4 && preview.pfs_partitions == 3, "full chain previewed");
            require(preview.rows[1].id == "__sysconf" && !preview.rows[1].change.empty() &&
                    preview.rows[1].change.find("next") != std::string::npos, "the broken link is the shown change");
            require(preview.orphaned.empty(), "nothing is left unreachable");
        }
        require(found_safe, "a single-bit link fault yields an automatic-safe map");
        require(ps2hdd::forensic::preview(scan, 999).rows.empty(), "out-of-range map is empty");
    } catch (const std::exception& error) {
        std::cerr << "forensic preview test failed: " << error.what() << '\n';
        return 1;
    }
    std::cout << "Forensic preview tests passed\n";
    return 0;
}
