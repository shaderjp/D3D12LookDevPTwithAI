#pragma once

#include <string_view>

#include <winrt/Microsoft.UI.Xaml.Controls.h>

namespace lookdevpt::ui
{
void RenderAssistantMarkdown(
    winrt::Microsoft::UI::Xaml::Controls::RichTextBlock const& target,
    std::wstring_view markdown);

void RenderAssistantPlainText(
    winrt::Microsoft::UI::Xaml::Controls::RichTextBlock const& target,
    std::wstring_view text);
}
