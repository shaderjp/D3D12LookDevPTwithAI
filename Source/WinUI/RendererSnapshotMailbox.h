#pragma once

#include "WinUIEditor.h"

#include <atomic>
#include <memory>

namespace lookdevpt::winui
{
class RendererSnapshotMailbox
{
public:
    RendererSnapshotMailbox()
        : m_latest(std::make_shared<EditorSnapshot>())
    {
    }

    void Publish(EditorSnapshotPtr snapshot) noexcept
    {
        m_latest.store(std::move(snapshot), std::memory_order_release);
    }

    [[nodiscard]] EditorSnapshotPtr Latest() const noexcept
    {
        return m_latest.load(std::memory_order_acquire);
    }

private:
    std::atomic<EditorSnapshotPtr> m_latest;
};
}
