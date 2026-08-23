#pragma once

#include "SceneImporter.h"

#include <string>

namespace rb
{
SceneImportResult ImportGltfScene(const std::wstring& path);
}
