#ifdef _WIN32

#include "ps2hdd/physical_drive.hpp"

#include <winioctl.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <limits>

namespace ps2hdd {
namespace {

class ScopedEvent final {
public:
    ScopedEvent() : handle_(CreateEventW(nullptr, TRUE, FALSE, nullptr)) {}
    ~ScopedEvent()
    {
        if (handle_ != nullptr) {
            CloseHandle(handle_);
        }
    }

    ScopedEvent(const ScopedEvent&) = delete;
    ScopedEvent& operator=(const ScopedEvent&) = delete;

    [[nodiscard]] HANDLE get() const noexcept { return handle_; }
    [[nodiscard]] bool valid() const noexcept { return handle_ != nullptr; }

private:
    HANDLE handle_{};
};

void set_overlapped_offset(OVERLAPPED& overlapped, std::uint64_t offset) noexcept
{
    overlapped.Offset = static_cast<DWORD>(offset & 0xFFFFFFFFULL);
    overlapped.OffsetHigh = static_cast<DWORD>(offset >> 32U);
}

bool finish_overlapped(HANDLE handle, OVERLAPPED& overlapped, BOOL started,
                       DWORD& transferred) noexcept
{
    if (!started) {
        const DWORD error = GetLastError();
        if (error != ERROR_IO_PENDING) {
            return false;
        }
    }
    return GetOverlappedResult(handle, &overlapped, &transferred, TRUE) != FALSE;
}

template <typename Descriptor>
bool query_storage_property(HANDLE handle, STORAGE_PROPERTY_ID property_id,
                            Descriptor& descriptor) noexcept
{
    ScopedEvent event;
    if (!event.valid()) {
        return false;
    }

    STORAGE_PROPERTY_QUERY query{};
    query.PropertyId = property_id;
    query.QueryType = PropertyStandardQuery;

    OVERLAPPED overlapped{};
    overlapped.hEvent = event.get();
    DWORD returned = 0;
    const BOOL started = DeviceIoControl(handle, IOCTL_STORAGE_QUERY_PROPERTY,
                                         &query, sizeof(query),
                                         &descriptor, sizeof(descriptor),
                                         nullptr, &overlapped);
    return finish_overlapped(handle, overlapped, started, returned) &&
           returned >= sizeof(descriptor);
}

struct AtaIdentifyPacket {
    ATA_PASS_THROUGH_EX pass{};
    std::array<std::byte, 512> identify{};
};

bool query_ata_rotation_rate(HANDLE handle, std::uint16_t& rate) noexcept
{
    AtaIdentifyPacket packet{};
    packet.pass.Length = sizeof(ATA_PASS_THROUGH_EX);
    packet.pass.AtaFlags = ATA_FLAGS_DATA_IN | ATA_FLAGS_DRDY_REQUIRED;
    packet.pass.DataTransferLength = static_cast<ULONG>(packet.identify.size());
    packet.pass.TimeOutValue = 2;
    packet.pass.DataBufferOffset = offsetof(AtaIdentifyPacket, identify);
    packet.pass.CurrentTaskFile[5] = 0xA0; // master/device register, ignored by most modern stacks
    packet.pass.CurrentTaskFile[6] = 0xEC; // ATA IDENTIFY DEVICE (read-only)

    ScopedEvent event;
    if (!event.valid()) {
        return false;
    }
    OVERLAPPED overlapped{};
    overlapped.hEvent = event.get();
    DWORD returned = 0;
    const BOOL started = DeviceIoControl(handle, IOCTL_ATA_PASS_THROUGH,
                                         &packet, sizeof(packet),
                                         &packet, sizeof(packet),
                                         nullptr, &overlapped);
    if (!finish_overlapped(handle, overlapped, started, returned)) {
        return false;
    }

    constexpr std::size_t word = 217;
    constexpr std::size_t byte_offset = word * sizeof(std::uint16_t);
    const auto low = std::to_integer<std::uint16_t>(packet.identify[byte_offset]);
    const auto high = std::to_integer<std::uint16_t>(packet.identify[byte_offset + 1]);
    const auto value = static_cast<std::uint16_t>(low | static_cast<std::uint16_t>(high << 8U));

    // ACS: 0 = not reported, 1 = non-rotating, 0x0401..0xFFFE = nominal RPM.
    if (value == 1 || (value >= 0x0401U && value <= 0xFFFEU)) {
        rate = value;
        return true;
    }
    return false;
}

StorageCharacteristics query_characteristics(HANDLE handle) noexcept
{
    StorageCharacteristics result;

    // Seek-penalty is the strongest Windows-provided hint for choosing between
    // rotational and non-rotational defaults. It is still only a hint: bridges
    // are allowed to omit or misreport it, so runtime latency remains a second
    // independent input to Emilia's adaptive policy.
    DEVICE_SEEK_PENALTY_DESCRIPTOR seek{};
    if (query_storage_property(handle, StorageDeviceSeekPenaltyProperty, seek)) {
        result.seek_penalty_known = true;
        result.incurs_seek_penalty = seek.IncursSeekPenalty != FALSE;
        result.media_class = result.incurs_seek_penalty
                                 ? StorageMediaClass::rotational
                                 : StorageMediaClass::solid_state;
    }

    // TRIM is diagnostic/corroborating information only. Some bridges do not
    // forward it and some non-SSD media can support deallocation semantics, so
    // it must never override an unknown/contradictory seek-penalty result.
    DEVICE_TRIM_DESCRIPTOR trim{};
    if (query_storage_property(handle, StorageDeviceTrimProperty, trim)) {
        result.trim_known = true;
        result.trim_enabled = trim.TrimEnabled != FALSE;
    }

    // Adapter descriptors have a fixed portion containing BusType and avoid the
    // variable vendor/product strings present in STORAGE_DEVICE_DESCRIPTOR.
    STORAGE_ADAPTER_DESCRIPTOR adapter{};
    if (query_storage_property(handle, StorageAdapterProperty, adapter)) {
        result.bus_type_known = true;
        result.bus_type = static_cast<std::uint32_t>(adapter.BusType);
    }

    // Some storage stacks (including real SATA setups) do not expose the seek
    // penalty property. ATA IDENTIFY word 217 is a safe read-only fallback and
    // also gives us a useful corroborating RPM value when the class is already
    // known. Bridges/controllers are free to reject pass-through; unknown remains
    // a fully supported result in that case.
    const bool ata_like = !result.bus_type_known ||
                          result.bus_type == static_cast<std::uint32_t>(BusTypeAta) ||
                          result.bus_type == static_cast<std::uint32_t>(BusTypeSata) ||
                          result.bus_type == static_cast<std::uint32_t>(BusTypeUsb);
    if (ata_like) {
        std::uint16_t rotation_rate = 0;
        if (query_ata_rotation_rate(handle, rotation_rate)) {
            result.nominal_rotation_rate_known = true;
            result.nominal_rotation_rate = rotation_rate;
            if (result.media_class == StorageMediaClass::unknown) {
                result.media_class = rotation_rate == 1
                                         ? StorageMediaClass::solid_state
                                         : StorageMediaClass::rotational;
            }
        }
    }

    return result;
}

} // namespace

PhysicalDrive::PhysicalDrive(unsigned index) : index_(index)
{
    const std::wstring path = L"\\\\.\\PhysicalDrive" + std::to_wstring(index_);

    // Read-only is a structural invariant. Emilia only changes how reads are
    // scheduled: the raw disk handle still requests GENERIC_READ and no source
    // write capability is introduced.
    handle_ = CreateFileW(path.c_str(), GENERIC_READ,
                          FILE_SHARE_READ | FILE_SHARE_WRITE,
                          nullptr, OPEN_EXISTING,
                          FILE_ATTRIBUTE_NORMAL | FILE_FLAG_RANDOM_ACCESS | FILE_FLAG_OVERLAPPED,
                          nullptr);
    if (handle_ == INVALID_HANDLE_VALUE) {
        open_error_ = GetLastError();
        return;
    }

    ScopedEvent event;
    if (!event.valid()) {
        open_error_ = GetLastError();
        return;
    }

    GET_LENGTH_INFORMATION length{};
    OVERLAPPED overlapped{};
    overlapped.hEvent = event.get();
    DWORD returned = 0;
    const BOOL started = DeviceIoControl(handle_, IOCTL_DISK_GET_LENGTH_INFO,
                                         nullptr, 0, &length, sizeof(length),
                                         nullptr, &overlapped);
    if (finish_overlapped(handle_, overlapped, started, returned)) {
        size_ = static_cast<std::uint64_t>(length.Length.QuadPart);
        open_error_ = ERROR_SUCCESS;
        characteristics_ = query_characteristics(handle_);
    } else {
        open_error_ = GetLastError();
    }
}

PhysicalDrive::~PhysicalDrive()
{
    if (handle_ != INVALID_HANDLE_VALUE) {
        CancelIoEx(handle_, nullptr);
        CloseHandle(handle_);
    }
}

bool PhysicalDrive::is_open() const noexcept
{
    return handle_ != INVALID_HANDLE_VALUE && size_ != 0;
}

DWORD PhysicalDrive::open_error() const noexcept
{
    return open_error_;
}

unsigned PhysicalDrive::index() const noexcept
{
    return index_;
}

std::uint64_t PhysicalDrive::size_bytes() const
{
    return size_;
}

std::string PhysicalDrive::display_name() const
{
    return "\\\\.\\PhysicalDrive" + std::to_string(index_);
}

bool PhysicalDrive::read(std::uint64_t offset, std::span<std::byte> out)
{
    if (!is_open() || offset > size_ || out.size() > size_ - offset) {
        return false;
    }
    if (out.empty()) {
        return true;
    }

    ScopedEvent event;
    if (!event.valid()) {
        return false;
    }

    std::size_t done = 0;
    while (done < out.size()) {
        const auto remaining = out.size() - done;
        const DWORD chunk = static_cast<DWORD>(
            std::min<std::size_t>(remaining, std::numeric_limits<DWORD>::max()));

        if (!ResetEvent(event.get())) {
            return false;
        }

        OVERLAPPED overlapped{};
        overlapped.hEvent = event.get();
        set_overlapped_offset(overlapped, offset + done);

        const BOOL started = ReadFile(handle_, out.data() + done, chunk, nullptr, &overlapped);
        DWORD read_bytes = 0;
        if (!finish_overlapped(handle_, overlapped, started, read_bytes) || read_bytes == 0) {
            return false;
        }
        done += read_bytes;
    }
    return true;
}

} // namespace ps2hdd

#endif
