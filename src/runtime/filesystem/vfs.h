// SecondSonPC — guest path namespace (/app0, /savedata0, /temp0, /download0, ...).
// Resolution follows the console/oracle rules: "//" collapses to "/", paths longer than 255 bytes
// are rejected, the first mount whose name equals the path or prefixes it with "/" wins.
#pragma once

#include <filesystem>
#include <optional>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <vector>

namespace SecondSon::Filesystem {

struct Resolved {
    std::string guest;          // sanitized guest path
    std::string mount;          // mount point it belongs to
    std::filesystem::path host; // host path (may not exist)
    bool read_only = false;
};

class Vfs {
public:
    void Mount(std::string guest, std::filesystem::path host, bool read_only);
    void Unmount(std::string_view guest);
    std::optional<Resolved> Resolve(std::string_view guest) const;
    bool IsMounted(std::string_view guest) const;
    void LogMounts() const;

private:
    struct MountPoint {
        std::string guest;
        std::filesystem::path host;
        bool read_only;
    };
    mutable std::shared_mutex mutex_;
    std::vector<MountPoint> mounts_;
};

Vfs& GetVfs();

/// Collapses "//" and checks the 255-byte limit; nullopt if too long.
std::optional<std::string> SanitizeGuestPath(std::string_view path);

} // namespace SecondSon::Filesystem
