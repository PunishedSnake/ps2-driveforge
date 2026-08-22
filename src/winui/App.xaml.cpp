#include "pch.h"
#include "App.xaml.h"
#include "MainWindow.xaml.h"

namespace winrt::PS2DriveForge::WinUI::implementation
{
App::App()
{
    InitializeComponent();
}

void App::OnLaunched(Microsoft::UI::Xaml::LaunchActivatedEventArgs const&)
{
    window_ = winrt::make<MainWindow>();
    window_.Activate();
}
}
