#include "pch.h"
#include "App.xaml.h"
#include "MainWindow.xaml.h"

namespace winrt::D3D12LookDevPTWinUI::implementation
{
App::App()
{
    InitializeComponent();
}

void App::OnLaunched(Microsoft::UI::Xaml::LaunchActivatedEventArgs const&)
{
    m_window = winrt::make<MainWindow>();
    m_window.Activate();
    m_window.AppWindow().ResizeClient({ 1600, 1000 });
}
}
