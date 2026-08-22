#include "pch.h"
#include "MainWindow.xaml.h"
#if __has_include("MainWindow.g.cpp")
#include "MainWindow.g.cpp"
#endif

namespace winrt::PS2DriveForge::WinUI::implementation
{
MainWindow::MainWindow()
{
    InitializeComponent();
    ExtendsContentIntoTitleBar(true);
    SetTitleBar(TitleBarDragRegion());
}
}
