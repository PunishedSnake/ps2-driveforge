#include "pch.h"
#include "MainWindow.xaml.h"

namespace winrt::PS2DriveForge::WinUI::implementation
{
MainWindow::MainWindow()
{
    InitializeComponent();
    ExtendsContentIntoTitleBar(true);
    SetTitleBar(TitleBarDragRegion());
}
}
