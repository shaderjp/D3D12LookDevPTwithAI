#include "pch.h"
#include "App.xaml.h"

#include <exception>

extern "C"
{
__declspec(dllexport) extern const UINT D3D12SDKVersion = 619;
__declspec(dllexport) extern const char* D3D12SDKPath = ".\\D3D12\\";
}

int WINAPI wWinMain(HINSTANCE, HINSTANCE, wchar_t*, int)
{
    try
    {
        winrt::init_apartment(winrt::apartment_type::single_threaded);
        winrt::Microsoft::UI::Xaml::Application::Start([](auto&&)
        {
            winrt::make<winrt::D3D12LookDevPTwithAI::implementation::App>();
        });
        winrt::uninit_apartment();
        return 0;
    }
    catch (winrt::hresult_error const& error)
    {
        MessageBoxW(nullptr, error.message().c_str(), L"D3D12LookDevPTwithAI startup failed", MB_OK | MB_ICONERROR);
        return static_cast<int>(error.code());
    }
    catch (std::exception const& error)
    {
        MessageBoxA(nullptr, error.what(), "D3D12LookDevPTwithAI startup failed", MB_OK | MB_ICONERROR);
        return 1;
    }
}
