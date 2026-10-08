// SecondSonPC — guest path namespace.
#include "runtime/filesystem/vfs.h"

#include <algorithm>
#include <mutex>

#include "base/log.h"

namespace SecondSon::Filesystem {

namespace {

std::string RemoveTrailingSlashes(std::string path) {
    while (path.ends_with('/')) {
        path.pop_back();
    }
    return path;
}

} // namespace

std::optional<std::string> SanitizeGuestPath(std::string_view path) {
    if (path.length() > 255) {
        return std::nullopt;
    }
    std::string corrected(path);
    size_t pos = corrected.find("//");
    while (pos != std::string::npos) {
        corrected.replace(pos, 2, "/");
        pos = corrected.find("//", pos + 1);
    }
    return corrected;
}

void Vfs::Mount(std::string guest, std::filesystem::path host, bool read_only) {
    std::unique_lock lock{mutex_};
    guest = RemoveTrailingSlashes(std::move(guest));
    SSPC_LOG(Filesystem, Info, "mount {} -> {}{}", guest, host.string(), read_only ? " (read-only)" : "");
    mounts_.push_back(MountPoint{std::move(guest), std::move(host), read_only});
}

void Vfs::Unmount(std::string_view guest) {
    std::unique_lock lock{mutex_};
    const auto g = RemoveTrailingSlashes(std::string(guest));
    const auto before = mounts_.size();
    std::erase_if(mounts_, [&](const MountPoint& m) { return m.guest == g; });
    SSPC_LOG(Filesystem, Info, "unmount {} ({} removed)", g, before - mounts_.size());
}

std::optional<Resolved> Vfs::Resolve(std::string_view guest) const {
    auto corrected = SanitizeGuestPath(guest);
    if (!corrected) {
        return std::nullopt;
    }
    std::shared_lock lock{mutex_};
    const auto it = std::find_if(mounts_.begin(), mounts_.end(), [&](const MountPoint& m) {
        return *corrected == m.guest || corrected->starts_with(m.guest + "/");
    });
    if (it == mounts_.end()) {
        return std::nullopt;
    }
    Resolved r;
    r.mount = it->guest;
    r.read_only = it->read_only;
    r.host = it->host;
    const auto sanitized = RemoveTrailingSlashes(*corrected);
    if (sanitized != it->guest && corrected->size() > it->guest.size() + 1) {
        std::string rel = corrected->substr(it->guest.size() + 1);
        std::replace(rel.begin(), rel.end(), '/', '\\');
        r.host /= std::filesystem::path(std::u8string(rel.begin(), rel.end()));
    }
    r.guest = std::move(*corrected);
    return r;
}

bool Vfs::IsMounted(std::string_view guest) const {
    std::shared_lock lock{mutex_};
    const auto g = RemoveTrailingSlashes(std::string(guest));
    return std::any_of(mounts_.begin(), mounts_.end(), [&](const MountPoint& m) { return m.guest == g; });
}

void Vfs::LogMounts() const {
    std::shared_lock lock{mutex_};
    for (const auto& m : mounts_) {
        SSPC_LOG(Filesystem, Info, "  {} -> {}{}", m.guest, m.host.string(), m.read_only ? " (ro)" : "");
    }
}

Vfs& GetVfs() {
    static Vfs vfs;
    return vfs;
}

} // namespace SecondSon::Filesystem
