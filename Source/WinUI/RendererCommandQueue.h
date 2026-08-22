#pragma once

#include "WinUIEditor.h"

#include <cstddef>
#include <deque>

namespace lookdevpt::winui
{
class RendererCommandQueue
{
public:
    void Enqueue(EditorCommand command)
    {
        if (IsCoalescible(command))
        {
            for (auto it = m_commands.rbegin();
                 it != m_commands.rend();
                 ++it)
            {
                // An action, save, approval, key transition, or pointer-button
                // event is an ordering barrier. Never move a continuous edit
                // across one of those commands.
                if (!IsCoalescible(*it))
                {
                    break;
                }
                if (SameCoalescingKey(*it, command))
                {
                    *it = std::move(command);
                    return;
                }
            }
        }
        m_commands.emplace_back(std::move(command));
    }

    [[nodiscard]] std::deque<EditorCommand> TakeAll()
    {
        std::deque<EditorCommand> result;
        result.swap(m_commands);
        return result;
    }

    [[nodiscard]] std::size_t Size() const noexcept
    {
        return m_commands.size();
    }

private:
    static bool IsCoalescible(EditorCommand const& command) noexcept
    {
        return command.type == EditorCommandType::SetValue ||
            (command.type == EditorCommandType::Pointer &&
             command.pointer.type == PointerEventType::Moved);
    }

    static bool SameCoalescingKey(
        EditorCommand const& left,
        EditorCommand const& right) noexcept
    {
        if (left.type != right.type)
        {
            return false;
        }
        if (right.type == EditorCommandType::SetValue)
        {
            return left.property == right.property &&
                left.index == right.index;
        }
        return left.pointer.type == PointerEventType::Moved &&
            right.pointer.type == PointerEventType::Moved;
    }

    std::deque<EditorCommand> m_commands;
};
}
