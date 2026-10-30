// PrismArchive -- PrismCore umbrella header.
//
// PrismCore is the platform-neutral brain of PrismArchive:
//   * it never includes <windows.h> and never touches 7-Zip headers
//   * it is header-only so it builds on Linux/macOS CI in seconds
//   * every rule in here is unit-tested (see Tests/)
#pragma once

#include "ArchiveCatalog.hpp"
#include "CharsetRepair.hpp"
#include "PasswordVault.hpp"
#include "PathSafety.hpp"
#include "SecurityGuard.hpp"
#include "SmartExtractor.hpp"
#include "VirtualTree.hpp"

namespace prism {
// Bumped whenever the on-disk vault / plan format changes incompatibly.
constexpr int kCoreVersion = 1;
}  // namespace prism
