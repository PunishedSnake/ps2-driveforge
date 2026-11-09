// Power-cut fault injection for every DriveForge write path that touches APA
// metadata. For an operation that performs W device writes in a clean run, the
// harness replays it W times per mode, cutting power at write k (k = 1..W):
//
//   lost: write k and everything after it never reach the media;
//   torn: the first half (rounded up, sector-granular) of write k persists.
//
// Writes are sector-atomic, matching the guarantee real drives give. After
// every cut the disk must be in a recoverable state (each touched structure is
// exactly its before or after image, or a journal explains the mix), and
// re-running the operation from that state must finish the job.

#include "apa_fixture.hpp"

#include "ps2hdd/apa_forensic.hpp"
#include "ps2hdd/apa_migrate.hpp"
#include "ps2hdd/apa_recovery_apply.hpp"
#include "ps2hdd/fhdb_restore.hpp"
#include "ps2hdd/hdl_image_install.hpp"
#include "ps2hdd/mutation_journal.hpp"
#include "ps2hdd/structure_health.hpp"

#include <chrono>
#include <functional>
#include <iostream>
#include <map>
#include <memory>

namespace {

using fixture::require;
constexpr std::size_t kSector = 512;
using Sector = std::array<std::byte, kSector>;

class PowerCutDisk final : public ps2hdd::WritableBlockDevice {
public:
    explicit PowerCutDisk(std::uint64_t size) : size_(size) {}

    std::uint64_t size_bytes() const override { return size_; }
    std::string display_name() const override { return "power-cut.img"; }

    bool read(std::uint64_t offset, std::span<std::byte> out) override
    {
        if (offset > size_ || out.size() > size_ - offset) return false;
        // Zero-fill, then overlay only the sectors that were ever written:
        // O(stored sectors in range) instead of one map lookup per sector.
        std::fill(out.begin(), out.end(), std::byte{0});
        const auto end = offset + out.size();
        for (auto it = sectors_.lower_bound(offset / kSector); it != sectors_.end() && it->first * kSector < end; ++it) {
            const auto sector_start = it->first * kSector;
            const auto from = std::max(offset, sector_start);
            const auto to = std::min(end, sector_start + kSector);
            std::copy_n(it->second.begin() + static_cast<std::ptrdiff_t>(from - sector_start), to - from,
                        out.begin() + static_cast<std::ptrdiff_t>(from - offset));
        }
        return true;
    }

    bool write(std::uint64_t offset, std::span<const std::byte> in) override
    {
        if (dead_ || offset > size_ || in.size() > size_ - offset) return false;
        ++writes;
        std::size_t limit = in.size();
        bool cut = false;
        if (cut_at != 0 && writes == cut_at) {
            cut = true;
            const auto total_sectors = (in.size() + kSector - 1) / kSector;
            limit = torn ? std::min(in.size(), ((total_sectors + 1) / 2) * kSector) : 0;
        }
        persist(offset, in.first(limit));
        if (cut) {
            dead_ = true;
            return false;
        }
        return true;
    }

    bool flush() override { return !dead_; }

    void put(std::uint64_t offset, std::span<const std::byte> bytes) { persist(offset, bytes); }

    void arm(std::size_t at, bool tear)
    {
        writes = 0;
        cut_at = at;
        torn = tear;
        dead_ = false;
    }
    void revive()
    {
        dead_ = false;
        cut_at = 0;
    }

    std::size_t writes{};
    std::size_t cut_at{};
    bool torn{};

private:
    void persist(std::uint64_t offset, std::span<const std::byte> in)
    {
        std::size_t done = 0;
        while (done < in.size()) {
            const auto position = offset + done;
            const auto sector = position / kSector;
            const auto within = static_cast<std::size_t>(position % kSector);
            const auto take = std::min(kSector - within, in.size() - done);
            auto& data = sectors_[sector];  // zero-initialised on first touch
            std::copy_n(in.begin() + static_cast<std::ptrdiff_t>(done), take,
                        data.begin() + static_cast<std::ptrdiff_t>(within));
            done += take;
        }
    }

    std::uint64_t size_;
    bool dead_{};
    std::map<std::uint64_t, Sector> sectors_;
};

// Loads a sparse fixture image (non-zero sectors only).
std::unique_ptr<PowerCutDisk> load(const std::filesystem::path& image)
{
    auto disk = std::make_unique<PowerCutDisk>(std::filesystem::file_size(image));
    std::ifstream in(image, std::ios::binary);
    std::vector<std::byte> buffer(1U << 20);
    std::uint64_t offset = 0;
    while (in) {
        in.read(reinterpret_cast<char*>(buffer.data()), static_cast<std::streamsize>(buffer.size()));
        const auto got = static_cast<std::size_t>(in.gcount());
        for (std::size_t s = 0; s + kSector <= got; s += kSector) {
            const auto sector = std::span(buffer).subspan(s, kSector);
            if (std::any_of(sector.begin(), sector.end(), [](std::byte b) { return b != std::byte{0}; }))
                disk->put(offset + s, sector);
        }
        offset += got;
    }
    return disk;
}

std::filesystem::path fresh(const std::string& name)
{
    const auto path = std::filesystem::temp_directory_path() / ("ps2df-fault-" + name);
    std::filesystem::remove_all(path);
    std::filesystem::create_directories(path);
    return path;
}

std::array<std::byte, 1024> master(PowerCutDisk& disk)
{
    std::array<std::byte, 1024> out{};
    require(disk.read(0, out), "read master");
    return out;
}

bool apa_clean(PowerCutDisk& disk)
{
    ps2hdd::apa::Reader reader(disk);
    return reader.scan().ok();
}

struct Tally {
    std::size_t cuts{};
};

// Runs `operation` once cleanly on a copy to count its writes, then replays it
// with a cut at every write position in both modes.
void sweep(const char* name, const PowerCutDisk& pristine,
           const std::function<void(PowerCutDisk&, const std::filesystem::path&)>& operation,
           const std::function<void(PowerCutDisk&, const std::filesystem::path&, std::size_t, bool)>& verify,
           Tally& tally)
{
    const auto started = std::chrono::steady_clock::now();
    auto clean = pristine;
    clean.arm(0, false);
    operation(clean, fresh(std::string(name) + "-clean"));
    const auto writes = clean.writes;
    require(writes > 0, "operation performs writes");
    for (const bool tear : {false, true}) {
        for (std::size_t k = 1; k <= writes; ++k) {
            auto disk = pristine;
            const auto artifacts = fresh(std::string(name) + "-" + std::to_string(k) + (tear ? "t" : "l"));
            disk.arm(k, tear);
            operation(disk, artifacts);
            disk.revive();
            verify(disk, artifacts, k, tear);
            ++tally.cuts;
        }
    }
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
    std::cout << "  " << name << ": " << writes << " write(s), " << writes * 2 << " cut(s) verified (" << ms << " ms)\n";
}

void bootstrap_disable_and_restore(Tally& tally)
{
    const auto root = fresh("boot-src");
    fixture::build(root / "disk.img", {{"__sysconf"}, 3});
    const auto pristine = load(root / "disk.img");
    const auto before = master(*pristine);

    auto disabled_disk = *pristine;
    const auto disable_dir = fresh("boot-disable-ref");
    require(ps2hdd::fhdb::disable_bootstrap(disabled_disk, disable_dir).ok, "reference disable");
    const auto after = master(disabled_disk);

    sweep("disable-bootstrap", *pristine,
          [](PowerCutDisk& disk, const std::filesystem::path& dir) { (void)ps2hdd::fhdb::disable_bootstrap(disk, dir); },
          [&](PowerCutDisk& disk, const std::filesystem::path& dir, std::size_t, bool) {
              const auto now = master(disk);
              require(now == before || now == after, "disable: master is exactly before or after");
              require(std::filesystem::exists(dir / "HDDMBR.BIN"), "disable: HDDMBR exists before any device write");
              const auto rerun = ps2hdd::fhdb::disable_bootstrap(disk, dir);
              require(rerun.ok && master(disk) == after, "disable: rerun completes");
          },
          tally);

    // Restore from the reference HDDRESCUE (valid KELF) back onto the disabled disk.
    sweep("restore-bootstrap", disabled_disk,
          [&](PowerCutDisk& disk, const std::filesystem::path& dir) {
              (void)ps2hdd::fhdb::restore_bootstrap_from_directory_to_image(disk, disable_dir, dir);
          },
          [&](PowerCutDisk& disk, const std::filesystem::path& dir, std::size_t, bool) {
              const auto now = master(disk);
              require(now == after || now == before, "restore: master is exactly disabled or restored");
              const auto rerun = ps2hdd::fhdb::restore_bootstrap_from_directory_to_image(disk, disable_dir, dir);
              require(rerun.ok && master(disk) == before, "restore: rerun republishes the pointer");
              require(ps2hdd::health::assess(disk).verdict == ps2hdd::health::Verdict::healthy, "restore: healthy");
          },
          tally);
}

void master_repair(Tally& tally)
{
    const auto root = fresh("master-src");
    fixture::build(root / "disk.img", {{"__sysconf"}, 3});
    auto pristine = load(root / "disk.img");
    const auto good = master(*pristine);
    auto damaged = good;
    damaged[4] = std::byte{'B'};  // single-field damage, checksum stale
    pristine->put(0, damaged);

    sweep("repair-master", *pristine,
          [](PowerCutDisk& disk, const std::filesystem::path& dir) { (void)ps2hdd::recovery::repair_master_header(disk, dir); },
          [&](PowerCutDisk& disk, const std::filesystem::path& dir, std::size_t, bool) {
              const auto now = master(disk);
              require(now == damaged || now == good, "master repair: exactly damaged or repaired");
              if (now == damaged)
                  require(ps2hdd::recovery::repair_master_header(disk, dir).ok, "master repair: rerun completes");
              require(master(disk) == good && apa_clean(disk), "master repair: final disk is clean");
          },
          tally);
}

void topology_repair(Tally& tally)
{
    const auto root = fresh("topo-src");
    const auto lbas = fixture::build(root / "disk.img", {{"__sysconf", "__system", "+OPL"}, 5});
    auto pristine = load(root / "disk.img");
    // Break two links so the repair needs more than one header write.
    for (const auto lba : {lbas[1], lbas[2]}) {
        std::array<std::byte, 1024> header{};
        require(pristine->read(std::uint64_t{lba} * kSector, header), "read header");
        header[8] ^= std::byte{0x01};  // Header::next low bit, checksum left stale
        pristine->put(std::uint64_t{lba} * kSector, header);
    }
    auto reference = *pristine;
    const auto plan_for = [](PowerCutDisk& disk, ps2hdd::forensic::ScanResult& scan) {
        scan = ps2hdd::forensic::scan_apa(disk);
        for (std::size_t i = 0; i < scan.maps.size(); ++i) {
            auto plan = ps2hdd::forensic::build_repair_plan(scan, i);
            if (plan.automatic_safe) return std::optional(plan);
        }
        return std::optional<ps2hdd::forensic::RepairPlan>{};
    };
    ps2hdd::forensic::ScanResult reference_scan;
    const auto reference_plan = plan_for(reference, reference_scan);
    require(reference_plan && reference_plan->patches.size() == 2, "two-link fault yields a two-patch automatic plan");
    require(ps2hdd::recovery::repair_forensic_topology(reference, reference_scan, *reference_plan, fresh("topo-ref")).ok,
            "reference topology repair");
    std::vector<std::pair<std::uint32_t, std::array<std::byte, 1024>>> before, after;
    for (const auto& patch : reference_plan->patches) {
        std::array<std::byte, 1024> b{}, a{};
        require(pristine->read(std::uint64_t{patch.lba} * kSector, b) && reference.read(std::uint64_t{patch.lba} * kSector, a),
                "read touched header");
        before.emplace_back(patch.lba, b);
        after.emplace_back(patch.lba, a);
    }

    sweep("repair-topology", *pristine,
          [&](PowerCutDisk& disk, const std::filesystem::path& dir) {
              ps2hdd::forensic::ScanResult scan;
              if (const auto plan = plan_for(disk, scan)) (void)ps2hdd::recovery::repair_forensic_topology(disk, scan, *plan, dir);
          },
          [&](PowerCutDisk& disk, const std::filesystem::path& dir, std::size_t, bool) {
              for (std::size_t i = 0; i < before.size(); ++i) {
                  std::array<std::byte, 1024> now{};
                  require(disk.read(std::uint64_t{before[i].first} * kSector, now), "read touched header");
                  require(now == before[i].second || now == after[i].second, "topology: each header exactly before or after");
              }
              if (!apa_clean(disk)) {
                  ps2hdd::forensic::ScanResult scan;
                  const auto plan = plan_for(disk, scan);
                  require(plan.has_value(), "topology: a partially repaired chain is still automatically repairable");
                  require(ps2hdd::recovery::repair_forensic_topology(disk, scan, *plan, dir).ok, "topology: rerun completes");
              }
              require(apa_clean(disk), "topology: final chain is clean");
          },
          tally);
}

void migration(Tally& tally)
{
    const auto root = fresh("migrate-src");
    fixture::build(root / "disk.img", {{"+OPL"}, 2});
    fixture::put_files(root / "disk.img", "+OPL", {{"/CFG/A.cfg", "x"}});
    auto source = load(root / "disk.img");
    const auto size = source->size_bytes();
    const auto plan = ps2hdd::migrate::plan(*source, size * 2);
    require(plan.ok, "migration plan");
    ps2hdd::migrate::Options options;
    options.chunk_bytes = 64U * 1024U * 1024U;  // keep the write count reviewable
    options.target_is_zeroed = true;            // blank device reads zeros; skip them like a sparse image
    options.verify = false;  // read-only re-hash is covered by migration_tests; cuts only affect writes
    const PowerCutDisk blank(size * 2);

    sweep("migrate", blank,
          [&](PowerCutDisk& disk, const std::filesystem::path&) { (void)ps2hdd::migrate::apply(*source, disk, plan, options); },
          [&](PowerCutDisk& disk, const std::filesystem::path&, std::size_t, bool) {
              const auto target_master = master(disk);
              if (target_master == plan.master) {
                  require(apa_clean(disk), "migration: a target with a master is complete");
              } else {
                  require(!apa_clean(disk), "migration: an incomplete target never looks like an APA disk");
                  require(ps2hdd::migrate::apply(*source, disk, plan, options).ok, "migration: rerun completes");
              }
              require(master(disk) == plan.master && apa_clean(disk), "migration: final target is clean");
          },
          tally);
}

// --- HDL install through the PS2DFRC1 mutation journal -----------------------

class MemoryIso final : public ps2hdd::BlockDevice {
public:
    explicit MemoryIso(std::size_t bytes) : bytes_(bytes) {}
    std::uint64_t size_bytes() const override { return bytes_.size(); }
    std::string display_name() const override { return "fault.iso"; }
    bool read(std::uint64_t offset, std::span<std::byte> out) override
    {
        if (offset > bytes_.size() || out.size() > bytes_.size() - offset) return false;
        std::copy_n(bytes_.begin() + static_cast<std::ptrdiff_t>(offset), out.size(), out.begin());
        return true;
    }
    std::vector<std::byte> bytes_;
};

void both_endian(std::byte* p, std::uint32_t v)
{
    for (int i = 0; i < 4; ++i) {
        p[i] = static_cast<std::byte>((v >> (8 * i)) & 0xffU);
        p[7 - i] = p[i];
    }
}

void record(std::span<std::byte> t, std::uint32_t lba, std::uint32_t bytes, std::uint8_t flags, std::string_view id)
{
    const std::size_t length = 33 + id.size() + (id.size() % 2 == 0 ? 1 : 0);
    t[0] = static_cast<std::byte>(length);
    both_endian(t.data() + 2, lba);
    both_endian(t.data() + 10, bytes);
    t[25] = static_cast<std::byte>(flags);
    t[28] = std::byte{1};
    t[31] = std::byte{1};
    t[32] = static_cast<std::byte>(id.size());
    std::memcpy(t.data() + 33, id.data(), id.size());
}

MemoryIso game_iso()
{
    constexpr std::string_view cnf = "BOOT2 = cdrom0:\\SLUS_123.45;1\r\nVER = 1.00\r\n";
    MemoryIso iso(24 * 2048);
    for (std::size_t i = 0; i < iso.bytes_.size(); ++i) iso.bytes_[i] = static_cast<std::byte>((i * 17U + 3U) & 0xffU);
    auto pvd = std::span(iso.bytes_).subspan(16 * 2048, 2048);
    std::fill(pvd.begin(), pvd.end(), std::byte{0});
    pvd[0] = std::byte{1};
    std::memcpy(pvd.data() + 1, "CD001", 5);
    pvd[6] = std::byte{1};
    record(pvd.subspan(156), 20, 2048, 0x02, std::string(1, '\0'));
    auto dir = std::span(iso.bytes_).subspan(20 * 2048, 2048);
    std::fill(dir.begin(), dir.end(), std::byte{0});
    record(dir, 20, 2048, 0x02, std::string(1, '\0'));
    record(dir.subspan(std::to_integer<unsigned>(dir[0])), 21, static_cast<std::uint32_t>(cnf.size()), 0, "SYSTEM.CNF;1");
    std::memcpy(iso.bytes_.data() + 21 * 2048, cnf.data(), cnf.size());
    return iso;
}

bool has_game(PowerCutDisk& disk)
{
    ps2hdd::apa::Reader reader(disk);
    const auto scan = reader.scan();
    return std::any_of(scan.partitions.begin(), scan.partitions.end(),
                       [](const ps2hdd::apa::Partition& p) { return p.type == ps2hdd::apa::kTypeHdl; });
}

void hdl_install(Tally& tally)
{
    const auto root = fresh("hdl-src");
    fixture::build(root / "disk.img", {{"+OPL"}, 6});
    const auto pristine = load(root / "disk.img");
    auto iso = game_iso();
    const auto install = [&](PowerCutDisk& disk, const std::filesystem::path& dir) {
        ps2hdd::hdl::ImageInstallOptions options;
        options.title = "Fault Injection";
        options.copy_buffer_bytes = 16 * 2048;
        options.mutation_journal_path = dir / "install.ps2df-journal";
        return ps2hdd::hdl::install_to_image(disk, iso, options);
    };

    sweep("hdl-install", *pristine,
          [&](PowerCutDisk& disk, const std::filesystem::path& dir) { (void)install(disk, dir); },
          [&](PowerCutDisk& disk, const std::filesystem::path& dir, std::size_t, bool) {
              const auto journal = dir / "install.ps2df-journal";
              if (std::filesystem::exists(journal)) {
                  const auto inspection = ps2hdd::inspect_mutation_journal(journal, disk);
                  require(inspection.ok && inspection.device_state != ps2hdd::MutationDeviceState::foreign_or_corrupt,
                          "hdl: journal explains every protected range");
                  if (inspection.journal_state == ps2hdd::MutationJournalState::prepared) {
                      require(ps2hdd::restore_prepared_mutation_journal(journal, disk).ok, "hdl: journal rollback");
                  }
              }
              require(apa_clean(disk), "hdl: APA chain clean after a cut (and rollback)");
              if (!has_game(disk)) {
                  std::filesystem::remove(journal);
                  require(install(disk, dir).ok, "hdl: rerun installs the game");
              }
              require(has_game(disk) && apa_clean(disk), "hdl: final disk has the game");
          },
          tally);
}

} // namespace

int main()
{
    try {
        Tally tally;
        bootstrap_disable_and_restore(tally);
        master_repair(tally);
        topology_repair(tally);
        migration(tally);
        hdl_install(tally);
        std::cout << "Fault injection tests passed (" << tally.cuts << " power cuts)\n";
    } catch (const std::exception& error) {
        std::cerr << "fault injection test failed: " << error.what() << '\n';
        return 1;
    }
    return 0;
}
