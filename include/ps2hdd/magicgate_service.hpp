#pragma once

#include "ps2hdd/magicgate_keyset.hpp"
#include "ps2hdd/magicgate_payload.hpp"
#include "ps2hdd/magicgate_sign.hpp"
#include "ps2hdd/magicgate_verify.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ps2hdd::magicgate {

struct HostServiceLimits {
    std::size_t max_keyset_bytes{64U * 1024U};
    std::size_t max_kelf_bytes{64U * 1024U * 1024U};
    std::size_t max_plaintext_bytes{64U * 1024U * 1024U};
};

struct HostServiceKeysetState {
    bool loaded{};
    std::string provenance;
};

struct HostKelfInspection {
    bool ok{};
    std::string error;
    KelfLayout layout;
    bool keyset_loaded{};
    bool uses_icvps2{};
};

struct HostKelfVerification {
    bool ok{};
    std::string error;
    KelfHeaderVerifyResult envelope;
    KelfPayloadVerifyResult payload;

    [[nodiscard]] std::span<const std::byte> plaintext() const noexcept
    {
        return payload.plaintext;
    }
};

struct HostKelfSignResult {
    bool ok{};
    std::string error;
    std::vector<std::byte> file;
    HostKelfVerification verification;
};

class MagicGateHostService final {
public:
    explicit MagicGateHostService(HostServiceLimits limits = {})
        : limits_(limits)
    {
    }

    MagicGateHostService(const MagicGateHostService&) = delete;
    MagicGateHostService& operator=(const MagicGateHostService&) = delete;

    ~MagicGateHostService() { clear_keyset(); }

    [[nodiscard]] const HostServiceLimits& limits() const noexcept { return limits_; }

    [[nodiscard]] HostServiceKeysetState keyset_state() const
    {
        return {keyset_.has_value(), keyset_provenance_};
    }

    [[nodiscard]] bool load_keyset_text(std::string_view text,
                                        std::string provenance,
                                        std::string& error)
    {
        clear_keyset();
        if (text.empty()) {
            error = "MagicGate keyset text is empty";
            return false;
        }
        if (text.size() > limits_.max_keyset_bytes) {
            error = "MagicGate keyset exceeds the host-service size limit";
            return false;
        }
        if (provenance.empty()) {
            error = "MagicGate keyset provenance must be explicit";
            return false;
        }

        const auto parsed = parse_magicgate_keyset(text);
        if (!parsed.ok) {
            error = parsed.error;
            if (parsed.line != 0U) error += " at line " + std::to_string(parsed.line);
            return false;
        }

        keyset_ = parsed.keyset;
        keyset_provenance_ = std::move(provenance);
        error.clear();
        return true;
    }

    [[nodiscard]] bool load_keyset(MagicGateKeyset keyset,
                                   std::string provenance,
                                   std::string& error)
    {
        clear_keyset();
        if (provenance.empty()) {
            error = "MagicGate keyset provenance must be explicit";
            return false;
        }
        keyset_ = std::move(keyset);
        keyset_provenance_ = std::move(provenance);
        error.clear();
        return true;
    }

    void clear_keyset() noexcept
    {
        if (keyset_) {
            auto* bytes = reinterpret_cast<volatile unsigned char*>(&*keyset_);
            for (std::size_t i = 0; i < sizeof(MagicGateKeyset); ++i) bytes[i] = 0;
            keyset_.reset();
        }
        keyset_provenance_.clear();
    }

    [[nodiscard]] HostKelfInspection inspect(std::span<const std::byte> file) const
    {
        HostKelfInspection result;
        result.keyset_loaded = keyset_.has_value();
        if (!bounded_kelf(file, result.error)) return result;

        result.layout = inspect_kelf(file);
        if (!result.layout.ok) {
            result.error = result.layout.error;
            return result;
        }
        result.uses_icvps2 = (result.layout.header.flags & 0x0002U) != 0U;
        result.ok = true;
        return result;
    }

    [[nodiscard]] HostKelfVerification verify(std::span<const std::byte> file) const
    {
        HostKelfVerification result;
        if (!bounded_kelf(file, result.error)) return result;
        if (!keyset_) {
            result.error = "MagicGate verification requires a loaded local keyset capability";
            return result;
        }

        result.envelope = verify_disk_kelf_header(file, *keyset_);
        if (!result.envelope.ok) {
            result.error = result.envelope.error;
            return result;
        }
        if (result.envelope.signed_flag_mapping == SignedFlagMapping::ambiguous ||
            result.envelope.signed_flag_mapping == SignedFlagMapping::unresolved) {
            result.error = "MagicGate KELF signed/encrypted BIT flag semantics are unresolved";
            return result;
        }

        result.payload = verify_and_decrypt_disk_kelf_payload(file, result.envelope, *keyset_);
        if (!result.payload.ok) {
            result.error = result.payload.error;
            return result;
        }
        if (result.payload.plaintext.size() > limits_.max_plaintext_bytes) {
            result.error = "MagicGate verified plaintext exceeds the host-service size limit";
            result.payload = {};
            return result;
        }

        result.ok = true;
        return result;
    }

    [[nodiscard]] HostKelfSignResult sign(const DiskKelfSignPlan& plan,
                                          std::span<const std::byte> plaintext) const
    {
        HostKelfSignResult result;
        if (!keyset_) {
            result.error = "MagicGate signing requires a loaded local keyset capability";
            return result;
        }
        if (plaintext.empty()) {
            result.error = "MagicGate signing plaintext is empty";
            return result;
        }
        if (plaintext.size() > limits_.max_plaintext_bytes ||
            plaintext.size() > std::numeric_limits<std::uint32_t>::max()) {
            result.error = "MagicGate signing plaintext exceeds the host-service size limit";
            return result;
        }

        auto signed_kelf = sign_low_layout_disk_kelf(plan, plaintext, *keyset_);
        if (!signed_kelf.ok) {
            result.error = signed_kelf.error;
            return result;
        }
        if (signed_kelf.file.size() > limits_.max_kelf_bytes) {
            result.error = "MagicGate generated KELF exceeds the host-service size limit";
            return result;
        }

        result.verification = verify(signed_kelf.file);
        if (!result.verification.ok) {
            result.error = "MagicGate host service rejected its generated KELF: " +
                           result.verification.error;
            return result;
        }
        if (result.verification.payload.plaintext.size() != plaintext.size() ||
            !std::equal(result.verification.payload.plaintext.begin(),
                        result.verification.payload.plaintext.end(),
                        plaintext.begin())) {
            result.error = "MagicGate generated KELF did not recover the exact source plaintext";
            return result;
        }

        result.file = std::move(signed_kelf.file);
        result.ok = true;
        return result;
    }

private:
    [[nodiscard]] bool bounded_kelf(std::span<const std::byte> file,
                                    std::string& error) const
    {
        if (file.empty()) {
            error = "MagicGate KELF input is empty";
            return false;
        }
        if (file.size() > limits_.max_kelf_bytes) {
            error = "MagicGate KELF exceeds the host-service size limit";
            return false;
        }
        return true;
    }

    HostServiceLimits limits_{};
    std::optional<MagicGateKeyset> keyset_;
    std::string keyset_provenance_;
};

} // namespace ps2hdd::magicgate
