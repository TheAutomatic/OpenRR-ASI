#pragma once

#include <cwchar>
#include <filesystem>

namespace openrr::pe {

// RE9 can re-load the host DLL from its _storage_ directory. The launcher
// installs the OpenRR payload beside that directory, in the game root.
inline std::filesystem::path installed_data_dir(const std::filesystem::path& host_dir, const wchar_t* entry) {
    const auto local = host_dir / L"openrr-amd";
    std::error_code ec;
    if (std::filesystem::exists(local / entry, ec)) return local;
    if (_wcsicmp(host_dir.filename().c_str(), L"_storage_") == 0) {
        const auto game = host_dir.parent_path() / L"openrr-amd";
        if (std::filesystem::exists(game / entry, ec)) return game;
    }
    return {};
}

}  // namespace openrr::pe
