#pragma once

#include "SceneImporter.h"

#include <atomic>
#include <functional>
#include <string>

namespace rb
{
struct SceneImportProgress
{
    enum class Stage
    {
        Parsing,
        LoadingAssets,
        Finalizing,
    };

    Stage stage = Stage::Parsing;
    std::uint64_t completed = 0;
    std::uint64_t total = 0;
    std::wstring currentAsset;
};

using SceneImportProgressCallback = std::function<void(const SceneImportProgress&)>;

SceneImportResult ImportPbrtScene(
    const std::wstring& path,
    const std::atomic_bool* cancelRequested = nullptr,
    const SceneImportProgressCallback& progress = {});
}
