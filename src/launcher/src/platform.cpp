// The one translation unit that is allowed to know what OS this is.
//
// Split as a header with the contract, one .inc per platform, and a .cpp that
// picks. The alternative -
// #ifdef inside each function - was tried in the launcher supervisor first and
// is what that file's own comments warn against, because the shared half stops
// being readable as either platform's code.

#include "platform.h"

#include <SDL3/SDL.h>

#include <utility>

#if defined(_WIN32)
#include "platform_windows.inc"
#else
#include "platform_posix.inc"
#endif

namespace thps::platform {

// Portable across both sides, so it lives here rather than being written twice.

RunResult Run(const std::vector<std::string>& argv) {
  RunResult result;
  Child child = Start(argv);
  if (!child.valid()) return result;
  result.ran = true;
  result.exit_code = child.Drain([&result](const std::string& line) {
    result.out += line;
    result.out += '\n';
  });
  return result;
}

std::uint64_t FreeSpaceBytes(const std::filesystem::path& path) {
  // The extraction target does not exist yet, and neither may its parent on a
  // fresh portable directory. Walk up to something that does; the answer is a
  // property of the volume, not of the leaf.
  std::error_code ec;
  std::filesystem::path probe = path;
  for (int guard = 0; guard < 64; ++guard) {
    if (std::filesystem::exists(probe, ec)) {
      const std::filesystem::space_info space = std::filesystem::space(probe, ec);
      if (ec) return 0;
      // `available` (to an unprivileged process), not `free` (which includes
      // the root reserve). Getting this wrong on a nearly-full ext4 means
      // promising ~5% of the disk that this user cannot actually write.
      return std::uint64_t(space.available);
    }
    const std::filesystem::path parent = probe.parent_path();
    if (parent.empty() || parent == probe) break;
    probe = parent;
  }
  return 0;  // "do not know" - callers must not read this as "no room"
}

std::filesystem::path SelfDir() {
  if (const char* base = SDL_GetBasePath()) {
    return std::filesystem::path(base);
  }
  std::error_code ec;
  const std::filesystem::path cwd = std::filesystem::current_path(ec);
  return ec ? std::filesystem::path(".") : cwd;
}

}  // namespace thps::platform
