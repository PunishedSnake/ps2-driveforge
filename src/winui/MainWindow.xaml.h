#pragma once

#include "pch.h"
#include "MainWindow.g.h"

namespace winrt::PS2DriveForge::WinUI::implementation
{
struct MainWindow : MainWindowT<MainWindow>
{
    MainWindow();
};
}

namespace winrt::PS2DriveForge::WinUI::factory_implementation
{
struct MainWindow : MainWindowT<MainWindow, implementation::MainWindow>
{
};
}
