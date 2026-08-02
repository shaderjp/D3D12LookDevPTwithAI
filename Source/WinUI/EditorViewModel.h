#pragma once

#include "WinUIEditor.h"

#include <winrt/Microsoft.UI.Xaml.Data.h>
#include <winrt/Windows.Foundation.Collections.h>

namespace lookdevpt::winui
{
struct EditorViewModel : winrt::implements<
    EditorViewModel,
    winrt::Microsoft::UI::Xaml::Data::INotifyPropertyChanged>
{
    EditorViewModel();

    winrt::event_token PropertyChanged(
        winrt::Microsoft::UI::Xaml::Data::PropertyChangedEventHandler const& handler);
    void PropertyChanged(winrt::event_token const& token) noexcept;

    void Apply(EditorSnapshotPtr snapshot);
    void Reset();
    void SetMaterialFilter(std::wstring filter);
    [[nodiscard]] EditorSnapshotPtr Snapshot() const noexcept;
    [[nodiscard]] std::int32_t MaterialSourceIndex(
        std::int32_t displayIndex) const noexcept;
    [[nodiscard]] std::int32_t MaterialDisplayIndex(
        std::int32_t sourceIndex) const noexcept;

    [[nodiscard]] winrt::Windows::Foundation::Collections::IObservableVector<winrt::hstring>
        Materials() const noexcept;
    [[nodiscard]] winrt::Windows::Foundation::Collections::IObservableVector<winrt::hstring>
        Variants() const noexcept;
    [[nodiscard]] winrt::Windows::Foundation::Collections::IObservableVector<winrt::hstring>
        Presets() const noexcept;
    [[nodiscard]] winrt::Windows::Foundation::Collections::IObservableVector<winrt::hstring>
        Approvals() const noexcept;
    [[nodiscard]] winrt::Windows::Foundation::Collections::IObservableVector<winrt::hstring>
        RecentRequests() const noexcept;

private:
    void Raise(std::wstring_view property);
    static void Replace(
        winrt::Windows::Foundation::Collections::IObservableVector<winrt::hstring> const& target,
        std::vector<winrt::hstring> const& values);
    void RebuildMaterials();

    EditorSnapshotPtr m_snapshot;
    std::wstring m_materialFilter;
    std::vector<std::int32_t> m_materialIndices;
    winrt::Windows::Foundation::Collections::IObservableVector<winrt::hstring> m_materials;
    winrt::Windows::Foundation::Collections::IObservableVector<winrt::hstring> m_variants;
    winrt::Windows::Foundation::Collections::IObservableVector<winrt::hstring> m_presets;
    winrt::Windows::Foundation::Collections::IObservableVector<winrt::hstring> m_approvals;
    winrt::Windows::Foundation::Collections::IObservableVector<winrt::hstring> m_recentRequests;
    winrt::event<winrt::Microsoft::UI::Xaml::Data::PropertyChangedEventHandler> m_propertyChanged;
};
}
