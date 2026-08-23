#include "WinUI/RendererCommandQueue.h"
#include "WinUI/RendererSnapshotMailbox.h"

#include <atomic>
#include <stdexcept>
#include <thread>
#include <vector>

using namespace lookdevpt::winui;

namespace
{
void Require(bool condition, char const* message)
{
    if (!condition)
    {
        throw std::runtime_error(message);
    }
}

double Number(EditorCommand const& command)
{
    return std::get<double>(command.value);
}
}

int main()
{
    RendererCommandQueue queue;
    queue.Enqueue({
        .type = EditorCommandType::SetValue,
        .property = L"material.roughness",
        .value = 0.2,
        .index = 4,
    });
    queue.Enqueue({
        .type = EditorCommandType::SetValue,
        .property = L"material.metallic",
        .value = 0.4,
        .index = 4,
    });
    queue.Enqueue({
        .type = EditorCommandType::SetValue,
        .property = L"material.roughness",
        .value = 0.8,
        .index = 4,
    });
    Require(queue.Size() == 2, "continuous setting was not coalesced");
    auto first = queue.TakeAll();
    Require(first[0].property == L"material.roughness" &&
            Number(first[0]) == 0.8,
        "latest coalesced value was not retained");
    Require(first[1].property == L"material.metallic",
        "unrelated continuous setting changed order");

    queue.Enqueue({
        .type = EditorCommandType::SetValue,
        .property = L"material.roughness",
        .value = 0.1,
        .index = 1,
    });
    queue.Enqueue({
        .type = EditorCommandType::Action,
        .property = L"project.save",
    });
    queue.Enqueue({
        .type = EditorCommandType::SetValue,
        .property = L"material.roughness",
        .value = 0.9,
        .index = 1,
    });
    auto barrier = queue.TakeAll();
    Require(barrier.size() == 3,
        "continuous setting crossed an order-sensitive command");
    Require(Number(barrier[0]) == 0.1 &&
            barrier[1].property == L"project.save" &&
            Number(barrier[2]) == 0.9,
        "FIFO command ordering was not preserved");

    queue.Enqueue({
        .type = EditorCommandType::SetValue,
        .property = L"material.roughness",
        .value = 0.3,
        .index = 2,
    });
    queue.Enqueue({
        .type = EditorCommandType::SetValue,
        .property = L"material.roughness",
        .value = 0.7,
        .index = 3,
    });
    Require(queue.Size() == 2,
        "material/slot index was omitted from the coalescing key");
    (void)queue.TakeAll();

    queue.Enqueue({
        .type = EditorCommandType::Pointer,
        .pointer = {
            .type = PointerEventType::Moved,
            .x = 10.0f,
        },
    });
    queue.Enqueue({
        .type = EditorCommandType::Pointer,
        .pointer = {
            .type = PointerEventType::Moved,
            .x = 25.0f,
        },
    });
    auto pointer = queue.TakeAll();
    Require(pointer.size() == 1 &&
            pointer[0].pointer.x == 25.0f,
        "pointer motion did not coalesce to the latest position");

    RendererSnapshotMailbox mailbox;
    std::atomic_bool writerFinished = false;
    std::atomic_bool consistent = true;
    std::thread writer([&]
    {
        for (std::uint64_t revision = 1;
             revision <= 5000;
             ++revision)
        {
            auto snapshot = std::make_shared<EditorSnapshot>();
            snapshot->revision = revision;
            snapshot->values[L"revision"] =
                static_cast<std::int64_t>(revision);
            mailbox.Publish(std::move(snapshot));
        }
        writerFinished.store(true, std::memory_order_release);
    });
    std::vector<std::thread> readers;
    for (int reader = 0; reader < 4; ++reader)
    {
        readers.emplace_back([&]
        {
            while (!writerFinished.load(std::memory_order_acquire))
            {
                EditorSnapshotPtr snapshot = mailbox.Latest();
                auto found = snapshot->values.find(L"revision");
                if (snapshot->revision != 0 &&
                    (found == snapshot->values.end() ||
                     std::get<std::int64_t>(found->second) !=
                        static_cast<std::int64_t>(
                            snapshot->revision)))
                {
                    consistent.store(false, std::memory_order_release);
                }
            }
        });
    }
    writer.join();
    for (std::thread& reader : readers)
    {
        reader.join();
    }
    Require(consistent.load(std::memory_order_acquire),
        "snapshot readers observed a torn publication");
    return 0;
}
