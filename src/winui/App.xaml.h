#pragma once

#include "pch.h"
#include "App.xaml.g.h"

namespace winrt::PS2DriveForge::WinUI::implementation
{
struct App : AppT<App>
{
    App();
    void OnLaunched(Microsoft::UI::Xaml::LaunchActivatedEventArgs const&);

private:
    Microsoft::UI::Xaml::Window window_{nullptr};
};
}
