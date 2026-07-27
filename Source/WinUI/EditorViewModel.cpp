#include "pch.h"
#include "EditorViewModel.h"

#include <cwctype>

using namespace winrt;
using namespace winrt::Microsoft::UI::Xaml::Data;
using namespace winrt::Windows::Foundation::Collections;

namespace lookdevpt::winui
{
EditorViewModel::EditorViewModel() :
    m_materials(single_threaded_observable_vector<hstring>()),
    m_variants(single_threaded_observable_vector<hstring>()),
    m_presets(single_threaded_observable_vector<hstring>()),
    m_approvals(single_threaded_observable_vector<hstring>())
{
}

event_token EditorViewModel::PropertyChanged(PropertyChangedEventHandler const& handler)
{
    return m_propertyChanged.add(handler);
}

void EditorViewModel::PropertyChanged(event_token const& token) noexcept
{
    m_propertyChanged.remove(token);
}

void EditorViewModel::Replace(
    IObservableVector<hstring> const& target,
    std::vector<hstring> const& values)
{
    if (target.Size() == values.size())
    {
        bool equal = true;
        for (uint32_t index = 0;
             index < target.Size();
             ++index)
        {
            if (target.GetAt(index) != values[index])
            {
                equal = false;
                break;
            }
        }
        if (equal)
        {
            return;
        }
    }
    target.Clear();
    for (hstring const& value : values)
    {
        target.Append(value);
    }
}

void EditorViewModel::Apply(EditorSnapshotPtr snapshot)
{
    m_snapshot = std::move(snapshot);
    std::vector<hstring> variants;
    std::vector<hstring> presets;
    std::vector<hstring> approvals;
    if (m_snapshot)
    {
        variants.reserve(m_snapshot->variants.size());
        for (NamedItem const& item : m_snapshot->variants)
        {
            variants.emplace_back(
                item.detail.empty()
                    ? item.name
                    : item.name + L" — " + item.detail);
        }
        presets.reserve(m_snapshot->presets.size());
        for (NamedItem const& item : m_snapshot->presets)
        {
            presets.emplace_back(
                item.detail.empty()
                    ? item.name
                    : item.detail + L" / " + item.name);
        }
        approvals.reserve(m_snapshot->approvals.size());
        for (ApprovalItem const& item : m_snapshot->approvals)
        {
            approvals.emplace_back(
                L"#" + std::to_wstring(item.id) + L" " +
                item.tool + L" — " + item.summary + L" (" +
                std::to_wstring(
                    (std::max)(item.secondsRemaining, 0)) +
                L" s)");
        }
    }
    RebuildMaterials();
    Replace(m_variants, variants);
    Replace(m_presets, presets);
    Replace(m_approvals, approvals);
    Raise(L"Snapshot");
}

void EditorViewModel::SetMaterialFilter(std::wstring filter)
{
    std::transform(
        filter.begin(), filter.end(), filter.begin(),
        [](wchar_t character)
        {
            return static_cast<wchar_t>(
                std::towlower(character));
        });
    if (filter == m_materialFilter)
    {
        return;
    }
    m_materialFilter = std::move(filter);
    RebuildMaterials();
}

std::int32_t EditorViewModel::MaterialSourceIndex(
    std::int32_t displayIndex) const noexcept
{
    return displayIndex >= 0 &&
        static_cast<size_t>(displayIndex) < m_materialIndices.size()
        ? m_materialIndices[static_cast<size_t>(displayIndex)]
        : -1;
}

std::int32_t EditorViewModel::MaterialDisplayIndex(
    std::int32_t sourceIndex) const noexcept
{
    auto found = std::find(
        m_materialIndices.begin(),
        m_materialIndices.end(),
        sourceIndex);
    return found == m_materialIndices.end()
        ? -1
        : static_cast<std::int32_t>(
            std::distance(m_materialIndices.begin(), found));
}

void EditorViewModel::RebuildMaterials()
{
    std::vector<hstring> materials;
    m_materialIndices.clear();
    if (m_snapshot)
    {
        for (size_t index = 0;
             index < m_snapshot->materials.size();
             ++index)
        {
            std::wstring searchable =
                m_snapshot->materials[index].name;
            std::transform(
                searchable.begin(), searchable.end(),
                searchable.begin(),
                [](wchar_t character)
                {
                    return static_cast<wchar_t>(
                        std::towlower(character));
                });
            if (m_materialFilter.empty() ||
                searchable.find(m_materialFilter) !=
                    std::wstring::npos)
            {
                materials.emplace_back(
                    m_snapshot->materials[index].name);
                m_materialIndices.push_back(
                    static_cast<std::int32_t>(index));
            }
        }
    }
    Replace(m_materials, materials);
}

void EditorViewModel::Reset()
{
    Apply({});
}

EditorSnapshotPtr EditorViewModel::Snapshot() const noexcept
{
    return m_snapshot;
}

IObservableVector<hstring> EditorViewModel::Materials() const noexcept
{
    return m_materials;
}

IObservableVector<hstring> EditorViewModel::Variants() const noexcept
{
    return m_variants;
}

IObservableVector<hstring> EditorViewModel::Presets() const noexcept
{
    return m_presets;
}

IObservableVector<hstring> EditorViewModel::Approvals() const noexcept
{
    return m_approvals;
}

void EditorViewModel::Raise(std::wstring_view property)
{
    m_propertyChanged(
        *this,
        PropertyChangedEventArgs(hstring(property)));
}
}
