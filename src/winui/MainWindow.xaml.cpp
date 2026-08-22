#include "pch.h"
#include "MainWindow.xaml.h"
#include "StartupLog.hpp"
#if __has_include("MainWindow.g.cpp")
#include "MainWindow.g.cpp"
#endif

#include <exception>
#include <string>

namespace winrt::PS2DriveForge::WinUI::implementation
{
MainWindow::MainWindow()
{
    ps2df::winui::diag::log("MainWindow::MainWindow entered");

    try {
        ps2df::winui::diag::log("MainWindow::InitializeComponent begin");
        InitializeComponent();
        ps2df::winui::diag::log("MainWindow::InitializeComponent complete");

        ps2df::winui::diag::log("MainWindow::ExtendsContentIntoTitleBar begin");
        ExtendsContentIntoTitleBar(true);
        ps2df::winui::diag::log("MainWindow::ExtendsContentIntoTitleBar complete");

        ps2df::winui::diag::log("MainWindow::SetTitleBar begin");
        SetTitleBar(TitleBarDragRegion());
        ps2df::winui::diag::log("MainWindow::SetTitleBar complete");
    } catch (winrt::hresult_error const& error) {
        const auto message = error.message();
        ps2df::winui::diag::log_hresult(
            "MainWindow construction failed",
            error.code().value,
            std::wstring_view(message.c_str(), message.size()));
        throw;
    } catch (std::exception const& error) {
        ps2df::winui::diag::log(std::string("MainWindow construction std::exception: ") + error.what());
        throw;
    } catch (...) {
        ps2df::winui::diag::log("MainWindow construction failed with unknown exception");
        throw;
    }

    ps2df::winui::diag::log("MainWindow::MainWindow complete");
}
}
