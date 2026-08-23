#pragma once

#include "pch.h"
#include "MainWindow.g.h"
#include "NativeSessionController.hpp"
#include "WinUIBootstrapProvider.hpp"
#include "WinUIRecoveryActions.hpp"

#include <commdlg.h>

#include <array>
#include <atomic>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace winrt::PS2DriveForge::WinUI::implementation
{
struct MainWindow : MainWindowT<MainWindow>
{
    MainWindow();
    ~MainWindow();

    // XAML-generated glue must be able to take the address of declarative event
    // handlers. Keep only that thin UI surface public; storage state and helper
    // methods remain private.
    void on_hdl_iso_path_changed(IInspectable const&, Microsoft::UI::Xaml::Controls::TextChangedEventArgs const&)
    {
        if (HdlIsoPathText().Text().empty() || hdl_iso_path_.empty()) return;
        start_hdl_metadata_prepare(false);
    }

    void on_refresh_hdl_online_metadata(IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&)
    {
        if (hdl_iso_path_.empty()) {
            show_error(L"Refresh game metadata", L"Choose a PS2 ISO first.");
            return;
        }
        start_hdl_metadata_prepare(true);
    }

    void on_capture_rescue(IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&)
    {
        if (mutation_busy_.load()) return;
        if (!current_physical_) {
            show_error(L"Create Rescue Capsule", L"Open a physical PS2 HDD first. Disk images do not have a live FHDB bootstrap pointer to capture.");
            return;
        }
        if (hdl_artifact_directory_.empty()) {
            show_error(L"Create Rescue Capsule", L"Choose a safety/artifact directory first.");
            return;
        }
        const auto result = controller_.capture_rescue_capsule(hdl_artifact_directory_);
        if (!result.ok) {
            show_error(L"Create Rescue Capsule", std::wstring(result.error.begin(), result.error.end()));
            return;
        }
        show_recovery_success(L"Rescue Capsule created", result.artifact_path.wstring());
        set_status(L"Canonical PS2HBRC Rescue Capsule captured read-only");
    }

    void on_restore_rescue(IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&)
    {
        if (mutation_busy_.load()) return;
        if (!current_physical_) {
            show_error(L"Restore Rescue Capsule", L"Bootstrap restore is available only for a physical PS2 HDD.");
            return;
        }
        if (hdl_artifact_directory_.empty()) {
            show_error(L"Restore Rescue Capsule", L"Choose the directory containing HDDRESCUE/HDDMBR/FHDBMBR inputs first.");
            return;
        }
        confirm_physical_action(L"restore bootstrap/recovery artifacts", [this] {
            mutation_busy_.store(true);
            const auto result = controller_.restore_bootstrap(hdl_artifact_directory_, hdl_artifact_directory_, true);
            mutation_busy_.store(false);
            if (!result.ok) {
                show_error(L"Restore Rescue Capsule", std::wstring(result.error.begin(), result.error.end()));
                return;
            }
            show_recovery_success(
                L"Bootstrap restored",
                result.cold_payload_verified
                    ? L"Payload and master pointer were cold-verified through a fresh read-only PhysicalDrive."
                    : L"Master pointer was cold-verified through a fresh read-only PhysicalDrive.");
            refresh_all_from_snapshot();
            refresh_hdl_tools();
            set_status(L"Bootstrap recovery completed and cold-verified");
        });
    }

    void on_forensic_analyze(IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&)
    {
        if (!current_physical_) {
            show_error(L"Forensic APA analysis", L"Open a physical PS2 HDD first.");
            return;
        }
        if (hdl_artifact_directory_.empty()) {
            show_error(L"Forensic APA analysis", L"Choose a safety/artifact directory first.");
            return;
        }
        const auto result = ps2df::winui::analyze_physical_apa(*current_physical_, hdl_artifact_directory_);
        if (!result.ok) {
            show_error(L"Forensic APA analysis", std::wstring(result.error.begin(), result.error.end()));
            return;
        }
        std::wstring message = L"FORENSIC.TXT: " + result.report_path.wstring();
        if (result.automatic_safe) message += L"\nAutomatic-safe topology repair evidence is available.";
        else message += L"\nNo automatic-safe topology repair was found; no write is offered.";
        show_recovery_success(L"Forensic analysis complete", message);
        set_status(L"Forensic APA analysis completed read-only");
    }

    void on_repair_master(IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&)
    {
        if (!current_physical_ || hdl_artifact_directory_.empty()) {
            show_error(L"Repair APA master", L"Open a physical PS2 HDD and choose a safety/artifact directory first.");
            return;
        }
        confirm_physical_action(L"apply conservative APA master repair", [this] {
            mutation_busy_.store(true);
            const auto result = ps2df::winui::repair_physical_master(*current_physical_, hdl_artifact_directory_);
            mutation_busy_.store(false);
            if (!result.ok) {
                show_error(L"Repair APA master", std::wstring(result.error.begin(), result.error.end()));
                return;
            }
            show_recovery_success(L"APA master repaired", L"The conservative repair was cold-verified through a fresh read-only scan.");
            std::string reopen_error;
            if (!controller_.open_physical(*current_physical_, reopen_error)) {
                show_error(L"Repair APA master", L"Repair committed, but the WinUI read-only session could not reopen the disk.");
                return;
            }
            refresh_all_from_snapshot();
            refresh_hdl_tools();
        });
    }

    void on_repair_forensic(IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&)
    {
        if (!current_physical_ || hdl_artifact_directory_.empty()) {
            show_error(L"Forensic APA repair", L"Open a physical PS2 HDD and choose a safety/artifact directory first.");
            return;
        }
        confirm_physical_action(L"apply automatic-safe forensic APA topology repair", [this] {
            mutation_busy_.store(true);
            const auto result = ps2df::winui::repair_physical_apa_automatic(*current_physical_, hdl_artifact_directory_);
            mutation_busy_.store(false);
            if (!result.ok) {
                show_error(L"Forensic APA repair", std::wstring(result.error.begin(), result.error.end()));
                return;
            }
            show_recovery_success(L"Forensic APA repair complete", L"Only an automatic-safe evidence map was accepted; touched headers and the final APA scan were cold-verified.");
            std::string reopen_error;
            if (!controller_.open_physical(*current_physical_, reopen_error)) {
                show_error(L"Forensic APA repair", L"Repair committed, but the WinUI read-only session could not reopen the disk.");
                return;
            }
            refresh_all_from_snapshot();
            refresh_hdl_tools();
        });
    }

    void on_choose_provider_manifest(IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&)
    {
        std::array<wchar_t, 32768> path{};
        OPENFILENAMEW dialog{};
        dialog.lStructSize = sizeof(dialog);
        dialog.hwndOwner = native_hwnd();
        dialog.lpstrFilter = L"DriveForge provider manifests (*.txt;*.ini;*.manifest)\0*.txt;*.ini;*.manifest\0All files\0*.*\0";
        dialog.lpstrFile = path.data();
        dialog.nMaxFile = static_cast<DWORD>(path.size());
        dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_EXPLORER;
        if (!GetOpenFileNameW(&dialog)) return;
        provider_manifest_path_ = std::filesystem::path(path.data());
        ProviderManifestPathText().Text(provider_manifest_path_.wstring());
        provider_preview_ = {};
        ProviderInstallButton().IsEnabled(false);
        ProviderPreviewText().Text(L"Manifest selected; stage it before installation.");
    }

    void on_choose_provider_keyset(IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&)
    {
        std::array<wchar_t, 32768> path{};
        OPENFILENAMEW dialog{};
        dialog.lStructSize = sizeof(dialog);
        dialog.hwndOwner = native_hwnd();
        dialog.lpstrFilter = L"MagicGate keysets (*.txt;*.ini)\0*.txt;*.ini\0All files\0*.*\0";
        dialog.lpstrFile = path.data();
        dialog.nMaxFile = static_cast<DWORD>(path.size());
        dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_EXPLORER;
        if (!GetOpenFileNameW(&dialog)) return;
        provider_keyset_path_ = std::filesystem::path(path.data());
        ProviderKeysetPathText().Text(provider_keyset_path_.wstring());
        provider_preview_ = {};
        ProviderInstallButton().IsEnabled(false);
    }

    void on_stage_provider(IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&)
    {
        if (mutation_busy_.exchange(true)) return;
        if (!current_physical_) {
            mutation_busy_.store(false);
            show_error(L"Stage bootstrap provider", L"Provider bootstrap installation requires an open physical PS2 HDD.");
            return;
        }
        if (provider_manifest_path_.empty()) {
            mutation_busy_.store(false);
            show_error(L"Stage bootstrap provider", L"Choose a local provider manifest first.");
            return;
        }
        ProviderStageButton().IsEnabled(false);
        ProviderInstallButton().IsEnabled(false);
        ProviderPreviewText().Text(L"Resolving immutable provider bytes, hashing and verifying KELF...");
        set_status(L"Staging bootstrap provider entirely on the host...");

        const auto index = *current_physical_;
        const auto manifest = provider_manifest_path_;
        const auto keyset = provider_keyset_path_;
        const std::wstring icv = ProviderIcvps2Box().Text().c_str();
        const auto provenance = winrt::to_string(ProviderIcvps2ProvenanceBox().Text());
        auto dispatcher = DispatcherQueue();
        auto weak = get_weak();
        mutation_thread_ = std::jthread([dispatcher, weak, index, manifest, keyset, icv, provenance](std::stop_token) mutable {
            auto preview = ps2df::winui::stage_bootstrap_provider(index, manifest, keyset, icv, std::move(provenance));
            dispatcher.TryEnqueue([weak, preview = std::move(preview)]() mutable {
                if (auto self = weak.get()) {
                    self->mutation_busy_.store(false);
                    self->ProviderStageButton().IsEnabled(true);
                    self->provider_preview_ = std::move(preview);
                    if (!self->provider_preview_.ok) {
                        self->ProviderPreviewText().Text(L"Staging refused: " + utf8_wide(self->provider_preview_.error));
                        self->ProviderInstallButton().IsEnabled(false);
                        self->set_status(L"Bootstrap provider staging refused before any target write");
                        return;
                    }
                    const auto& p = self->provider_preview_;
                    self->ProviderPreviewText().Text(
                        utf8_wide(p.product) + L" / " + utf8_wide(p.version) +
                        L"\nProvider: " + utf8_wide(p.provider_id) +
                        L"\nSource SHA-256: " + utf8_wide(p.source_sha256) +
                        L"\nPayload SHA-256: " + utf8_wide(p.payload_sha256) +
                        L"\nPayload: " + std::to_wstring(p.payload_bytes) + L" bytes / " + std::to_wstring(p.payload_sectors) +
                        L" sectors\nTarget fingerprint: " + utf8_wide(p.target_fingerprint));
                    self->ProviderInstallButton().IsEnabled(true);
                    self->set_status(L"Bootstrap provider staged, verified and frozen; no target writes performed");
                }
            });
        });
    }

    void on_install_provider(IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&)
    {
        if (mutation_busy_.load()) return;
        if (!current_physical_ || !provider_preview_.ok) {
            show_error(L"Install bootstrap provider", L"Stage and verify a provider against the current physical target first.");
            return;
        }
        if (hdl_artifact_directory_.empty()) {
            show_error(L"Install bootstrap provider", L"Choose the safety/artifact directory first. Provider install requires Rescue Capsule and HDDMBR evidence.");
            return;
        }
        confirm_physical_action(L"install the staged bootstrap provider", [this] {
            if (mutation_busy_.exchange(true)) return;
            const auto index = *current_physical_;
            const auto frozen = provider_preview_.frozen;
            const auto safety = hdl_artifact_directory_;
            ProviderInstallButton().IsEnabled(false);
            ProviderPreviewText().Text(ProviderPreviewText().Text() + L"\nInstalling with guarded physical lease...");
            set_status(L"Installing frozen bootstrap payload with Rescue Capsule/HDDMBR protection...");
            auto dispatcher = DispatcherQueue();
            auto weak = get_weak();
            mutation_thread_ = std::jthread([dispatcher, weak, index, frozen, safety](std::stop_token) mutable {
                auto result = ps2df::winui::commit_bootstrap_provider(index, frozen, safety);
                dispatcher.TryEnqueue([weak, result = std::move(result)]() mutable {
                    if (auto self = weak.get()) {
                        self->mutation_busy_.store(false);
                        if (!result.ok) {
                            self->show_error(L"Install bootstrap provider", utf8_wide(result.error));
                            self->ProviderInstallButton().IsEnabled(self->provider_preview_.ok);
                            self->set_status(result.partial ? L"Bootstrap provider install failed after partial mutation; recovery evidence preserved" : L"Bootstrap provider install refused/failed before commit");
                            return;
                        }
                        self->show_recovery_success(L"Bootstrap provider installed", L"Pre-install Rescue Capsule and HDDMBR were persisted. Payload, pointer and final APA state were cold-verified read-only.");
                        self->ProviderPreviewText().Text(self->ProviderPreviewText().Text() + L"\nRescue: " + result.rescue_path.wstring() + L"\nHDDMBR: " + result.hddmbr_path.wstring());
                        self->provider_preview_ = {};
                        self->ProviderInstallButton().IsEnabled(false);
                        std::string reopen_error;
                        if (!self->controller_.open_physical(*self->current_physical_, reopen_error)) {
                            self->show_error(L"Bootstrap provider", L"Install was cold-verified by the endpoint, but the management session could not reopen afterward.");
                            return;
                        }
                        self->refresh_all_from_snapshot();
                        self->refresh_hdl_tools();
                        self->set_status(L"Bootstrap provider install complete and cold-verified");
                    }
                });
            });
        });
    }

private:
    static std::wstring utf8_wide(std::string_view value)
    {
        if (value.empty()) return {};
        const int needed = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0);
        if (needed <= 0) return std::wstring(value.begin(), value.end());
        std::wstring wide(static_cast<std::size_t>(needed), L'\0');
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), wide.data(), needed);
        return wide;
    }

    void start_hdl_metadata_prepare(bool refresh_catalog)
    {
        if (hdl_iso_path_.empty()) return;
        if (hdl_prepare_busy_.exchange(true)) {
            set_status(L"Game metadata is already being prepared...");
            return;
        }
        if (hdl_prepare_thread_.joinable()) hdl_prepare_thread_.join();

        std::array<wchar_t, 32768> local_data{};
        const DWORD chars = GetEnvironmentVariableW(L"LOCALAPPDATA", local_data.data(), static_cast<DWORD>(local_data.size()));
        std::filesystem::path app_root = chars > 0 && chars < local_data.size()
            ? std::filesystem::path(local_data.data()) / L"PS2 DriveForge"
            : std::filesystem::temp_directory_path() / L"PS2 DriveForge";
        const auto cache_root = app_root / L"OPL Cache";

        if (hdl_artifact_directory_.empty()) {
            hdl_artifact_directory_ = app_root / L"Recovery";
            std::error_code ec;
            std::filesystem::create_directories(hdl_artifact_directory_, ec);
            HdlArtifactPathText().Text(hdl_artifact_directory_.wstring());
        }

        const auto iso_path = hdl_iso_path_;
        HdlInstallButton().IsEnabled(false);
        HdlMetadataSummaryText().Text(refresh_catalog ? L"Refreshing game databases and OPL metadata..." : L"Reading SYSTEM.CNF and preparing game metadata...");
        HdlDetectedMediaText().Text(L"Startup / media: detecting...");
        HdlMetadataDetailsText().Text(L"Checking HDL Batch Installer / OPL databases, CFG, cheats and artwork...");
        HdlPreviewStateText().Text(L"Preparing automatic install plan...");
        set_status(L"Preparing ISO metadata and OPL assets on the host...");

        auto dispatcher = DispatcherQueue();
        auto weak = get_weak();
        hdl_prepare_thread_ = std::jthread([dispatcher, weak, iso_path, cache_root, refresh_catalog](std::stop_token) mutable {
            auto self = weak.get();
            if (!self) return;
            auto profile = self->controller_.prepare_hdl_iso(iso_path, cache_root, refresh_catalog);
            dispatcher.TryEnqueue([weak, iso_path, profile = std::move(profile)]() mutable {
                if (auto self2 = weak.get()) {
                    self2->hdl_prepare_busy_.store(false);
                    if (self2->hdl_iso_path_ != iso_path) return;
                    self2->hdl_iso_profile_ = std::move(profile);
                    if (!self2->hdl_iso_profile_.ok) {
                        self2->HdlMetadataSummaryText().Text(L"Could not prepare this ISO automatically");
                        self2->HdlMetadataDetailsText().Text(utf8_wide(self2->hdl_iso_profile_.error));
                        self2->HdlPreviewStateText().Text(L"Automatic plan unavailable");
                        self2->HdlInstallButton().IsEnabled(false);
                        self2->set_status(L"ISO metadata preparation failed before any target write");
                        return;
                    }

                    const auto& p = self2->hdl_iso_profile_;
                    self2->HdlTitleBox().Text(utf8_wide(p.title));
                    self2->HdlMediaPicker().SelectedIndex(p.media == ps2hdd::hdl::MediaType::cd ? 1 : 0);
                    const auto media = p.media == ps2hdd::hdl::MediaType::cd ? L"CD" : L"DVD";
                    const auto mib = p.image_bytes / (1024ULL * 1024ULL);
                    self2->HdlDetectedMediaText().Text(L"Startup: " + utf8_wide(p.game_id) + L"  |  " + media + L" auto-detected  |  " + std::to_wstring(mib) + L" MiB");

                    std::wstring summary = p.title_from_database
                        ? L"Title resolved from the HDL Batch Installer game database"
                        : L"Title resolved from ISO/OPL metadata";
                    summary += L". " + std::to_wstring(p.opl_assets) + L" OPL asset(s) staged automatically.";
                    self2->HdlMetadataSummaryText().Text(summary);

                    std::wstring details;
                    if (!p.developer.empty()) details += L"Developer: " + utf8_wide(p.developer) + L"  ";
                    if (!p.genre.empty()) details += L"Genre: " + utf8_wide(p.genre) + L"  ";
                    if (!p.release_date.empty()) details += L"Release: " + utf8_wide(p.release_date) + L"  ";
                    if (!p.players.empty()) details += L"Players: " + utf8_wide(p.players) + L"  ";
                    if (!p.rating.empty()) details += L"Rating: " + utf8_wide(p.rating) + L"  ";
                    details += L"Cache/catalog assets: " + std::to_wstring(p.cache_assets);
                    if (!p.warnings.empty()) details += L"  |  Optional provider issues: " + std::to_wstring(p.warnings.size());
                    self2->HdlMetadataDetailsText().Text(details);

                    if (self2->snapshot_.open) {
                        self2->preview_hdl_install();
                        if (self2->hdl_preview_.ok && p.opl_assets != 0) {
                            auto current = std::wstring(self2->HdlMetadataDetailsText().Text());
                            if (self2->hdl_preview_.assets_will_install) {
                                current += L"  |  Disk assets -> " + utf8_wide(self2->hdl_preview_.opl_partition_id);
                            } else {
                                current += L"  |  OPL assets kept host-side: " + utf8_wide(self2->hdl_preview_.warning);
                            }
                            self2->HdlMetadataDetailsText().Text(current);
                        }
                    } else {
                        self2->HdlPreviewStateText().Text(L"Metadata ready; open a target to build the install plan");
                    }
                    self2->set_status(L"Game metadata and OPL assets prepared automatically");
                }
            });
        });
    }

    void wire_events();
    void load_theme_preference();
    void save_theme_preference(int index);
    void apply_theme_index(int index);
    void show_page(std::wstring_view tag, bool settings = false);

    void start_discovery(bool user_requested);
    void handle_discovery(std::vector<ps2hdd::PhysicalDriveProbe> probes, bool user_requested);
    void open_physical_async(unsigned index);
    void open_image_async(std::filesystem::path path);
    void finish_source_open(bool ok, std::string error, std::optional<unsigned> physical, std::filesystem::path image);

    void refresh_all_from_snapshot();
    void refresh_partition_list();
    void refresh_pfs_partitions();
    void refresh_performance();
    void start_enrichment(bool reset);

    void refresh_hdl_tools();
    void choose_hdl_iso();
    void choose_hdl_artifact_directory();
    void preview_hdl_install();
    void start_hdl_install(bool physical_confirmed);
    void start_hdl_remove(bool physical_confirmed);
    void run_physical_preflight();
    void confirm_physical_action(std::wstring_view action, std::function<void()> continuation);
    [[nodiscard]] ps2df::winui::HdlInstallRequest current_hdl_request() const;

    void browse_current_pfs();
    void refresh_files_list();
    void export_selected_entry();
    void start_mount();
    void request_unmount(bool interactive = true);
    void poll_mount_state();
    void open_mounted_volume();
    void restart_as_administrator();
    void open_startup_log();
    void open_legacy_frontend();
    void show_error(std::wstring_view title, std::wstring_view message) const;
    void set_status(std::wstring_view text);

    [[nodiscard]] HWND native_hwnd() const;
    [[nodiscard]] std::filesystem::path executable_path() const;
    [[nodiscard]] std::filesystem::path package_root() const;
    [[nodiscard]] std::filesystem::path mount_helper_path() const;
    [[nodiscard]] std::filesystem::path launcher_path() const;
    [[nodiscard]] std::filesystem::path startup_log_path() const;
    [[nodiscard]] std::filesystem::path settings_path() const;

    void on_navigation_invoked(Microsoft::UI::Xaml::Controls::NavigationView const&, Microsoft::UI::Xaml::Controls::NavigationViewItemInvokedEventArgs const& args);
    void on_open_image(IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
    void on_rescan(IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
    void on_open_drive(IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
    void on_filter_changed(IInspectable const&, Microsoft::UI::Xaml::Controls::TextChangedEventArgs const&);
    void on_partition_filter_changed(IInspectable const&, Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const&);
    void on_show_subs_toggled(IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
    void on_refresh_metadata(IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
    void on_cancel_enrichment(IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);

    void on_hdl_choose_iso(IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
    void on_hdl_choose_artifacts(IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
    void on_hdl_preview(IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
    void on_hdl_install(IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
    void on_hdl_game_changed(IInspectable const&, Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const&);
    void on_hdl_delete(IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
    void on_physical_preflight(IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);

    void show_recovery_success(std::wstring_view title, std::wstring_view message)
    {
        HdlToolsInfoBar().Severity(Microsoft::UI::Xaml::Controls::InfoBarSeverity::Success);
        HdlToolsInfoBar().Title(title);
        HdlToolsInfoBar().Message(message);
        HdlToolsInfoBar().IsOpen(true);
    }

    void on_pfs_partition_changed(IInspectable const&, Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const&);
    void on_files_selection_changed(IInspectable const&, Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const&);
    void on_files_double_tapped(IInspectable const&, Microsoft::UI::Xaml::Input::DoubleTappedRoutedEventArgs const&);
    void on_files_up(IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
    void on_export(IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
    void on_mount(IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
    void on_open_explorer(IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
    void on_unmount(IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
    void on_reset_stats(IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
    void on_theme_changed(IInspectable const&, Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const&);
    void on_restart_admin(IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
    void on_open_log(IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
    void on_open_legacy(IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
    void on_telemetry_tick(IInspectable const&, IInspectable const&);
    void on_mount_tick(IInspectable const&, IInspectable const&);
    void on_closed(IInspectable const&, Microsoft::UI::Xaml::WindowEventArgs const&);

    ps2df::winui::NativeSessionController controller_;
    ps2df::winui::SessionSnapshot snapshot_;
    std::vector<ps2hdd::PhysicalDriveProbe> detected_drives_;
    std::vector<ps2df::winui::PartitionRowSnapshot> visible_partition_rows_;
    std::vector<ps2df::winui::PartitionRowSnapshot> hdl_game_rows_;
    std::vector<std::string> pfs_partitions_;
    std::vector<ps2df::winui::BrowseEntrySnapshot> visible_file_entries_;

    std::optional<unsigned> current_physical_;
    std::filesystem::path current_image_;
    std::string current_pfs_partition_;
    std::string current_pfs_path_;

    std::filesystem::path hdl_iso_path_;
    std::filesystem::path hdl_artifact_directory_;
    ps2df::winui::HdlInstallPreviewSnapshot hdl_preview_;
    ps2df::winui::HdlIsoPreparationSnapshot hdl_iso_profile_;

    std::filesystem::path provider_manifest_path_;
    std::filesystem::path provider_keyset_path_;
    ps2df::winui::BootstrapProviderPreview provider_preview_;

    std::jthread discovery_thread_;
    std::jthread source_thread_;
    std::jthread enrichment_thread_;
    std::jthread export_thread_;
    std::jthread mutation_thread_;
    std::jthread hdl_prepare_thread_;
    std::atomic_bool source_busy_{false};
    std::atomic_bool mutation_busy_{false};
    std::atomic_bool hdl_prepare_busy_{false};
    bool suppress_theme_event_{false};

    Microsoft::UI::Xaml::DispatcherTimer telemetry_timer_;
    Microsoft::UI::Xaml::DispatcherTimer mount_timer_;
    HANDLE mount_process_{nullptr};
    std::wstring mount_point_;
};
}

namespace winrt::PS2DriveForge::WinUI::factory_implementation
{
struct MainWindow : MainWindowT<MainWindow, implementation::MainWindow>
{
};
}
