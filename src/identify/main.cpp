// thps_p8_identify - the SDK-side worker for the launcher.
//
// Two jobs the GUI cannot do itself, because the GUI links no SDK at all and
// must never learn to read a disc image:
//
//   --identify_disc=<image>            answer "is this the right disc?" as JSON
//   --identify_disc=<image> --extract_to=<dir>   copy the game data out
//
// Why a sibling binary rather than a mode on `thps_p8`, which is the obvious
// shape: REX_DEFINE_APP only registers an app creator, and `main` lives
// inside the SDK runtime, so adding a flag to the game would mean taking over
// its entry point. That is a change to the thing most likely to break, for no
// gain the launcher can see. The property that matters is unchanged: identity is
// answered by something that links the SDK, and the GUI only spawns and parses.
// The game's own dump gate shares common/supported_dumps.h with this tool rather
// than carrying a second copy of the rule.
//
// Ordering matters and is the point: identity is answered from the *mounted*
// image in about 58 ms, before a single byte is copied. A disc that is not the
// supported release is refused without writing 4.7 GB first.

#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
// Not optional. windows.h defines min and max as macros, and the SDK's own
// headers call std::min - so without this the first SDK include fails with
// "expected unqualified-id" pointing into minwindef.h, which reads as a broken
// SDK rather than a macro collision.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <io.h>
#else
#include <sys/wait.h>
#include <unistd.h>
#endif

#include <rex/crypto/sha256.h>
#include <rex/filesystem/devices/disc_image_device.h>
#include <rex/memory/mapped_memory.h>
#include <rex/system/xio.h>

#include "common/supported_dumps.h"

namespace {

constexpr const char* kMountPath = "\\Device\\Harddisk0\\Partition1";

// The SDK logs to stdout, not stderr - "[error] [fs] Failed to verify disc
// image header" lands in the middle of what is supposed to be a machine-
// readable channel, and any consumer doing a real JSON parse chokes on it.
//
// So stdout is taken away from the SDK before it can write there: fd 1 is
// pointed at stderr for the whole run, and this FILE* keeps the original. Every
// byte this program means as output goes through it; every byte the SDK emits
// goes to stderr where diagnostics belong. Nothing is lost, and the contract
// with the launcher becomes "stdout is JSON and PROGRESS lines, always".
FILE* g_out = stdout;

void ClaimStdout() {
#if defined(_WIN32)
  const int out_fd = _fileno(stdout);
  const int err_fd = _fileno(stderr);
  if (out_fd < 0 || err_fd < 0) return;
  const int real = _dup(out_fd);
  if (real < 0) return;
  if (_dup2(err_fd, out_fd) < 0) {
    _close(real);
    return;
  }
  if (FILE* f = _fdopen(real, "w")) {
    g_out = f;
  } else {
    _close(real);
  }
#else
  const int real = ::dup(STDOUT_FILENO);
  if (real < 0) return;  // nothing sane to do; leave the channel as it is
  if (::dup2(STDERR_FILENO, STDOUT_FILENO) < 0) {
    ::close(real);
    return;
  }
  if (FILE* f = ::fdopen(real, "w")) {
    g_out = f;
  } else {
    ::close(real);
  }
#endif
}

void EmitJson(const char* verdict, const std::string& detail) {
  std::fprintf(g_out, "{\n  \"verdict\": \"%s\",\n  \"detail\": \"%s\"\n}\n", verdict,
               detail.c_str());
  std::fflush(g_out);
}

// The same thing, plus what we actually saw.
//
// A rejected disc is the one case where the person holding it can tell us
// something we cannot find out ourselves: whether another regional release
// exists and what its executable hashes to. Printing the observed hash and size
// turns "it says no" into a complete report they can paste into an issue,
// without sending - or being asked for - a single byte of the game.
void EmitJsonObserved(const char* verdict, const std::string& detail, const std::string& sha256,
                      uint64_t size) {
  std::fprintf(g_out,
               "{\n"
               "  \"verdict\": \"%s\",\n"
               "  \"detail\": \"%s\",\n"
               "  \"observed_sha256\": \"%s\",\n"
               "  \"observed_size\": %llu\n"
               "}\n",
               verdict, detail.c_str(), sha256.c_str(), (unsigned long long)size);
  std::fflush(g_out);
}

bool IsDirectory(const rex::filesystem::Entry* e) {
  return (e->attributes() & rex::system::X_FILE_ATTRIBUTE_DIRECTORY) != 0;
}

// A disc image is untrusted input. An entry naming `..` or an absolute path
// would let a crafted image write outside the install directory, so the walk
// refuses rather than sanitising - there is no legitimate reason for either to
// appear in a retail disc.
bool IsUnsafeComponent(std::string_view name) {
  return name.empty() || name == "." || name == ".." || name.find('/') != std::string_view::npos ||
         name.find('\\') != std::string_view::npos;
}

uint64_t CountBytes(const rex::filesystem::Entry* dir) {
  uint64_t total = 0;
  for (const auto& child : dir->children()) {
    total += IsDirectory(child.get()) ? CountBytes(child.get()) : child->size();
  }
  return total;
}

bool ExtractTree(rex::filesystem::Entry* dir, const std::filesystem::path& out, uint64_t total,
                 uint64_t* done, std::string* error) {
  std::error_code ec;
  std::filesystem::create_directories(out, ec);
  if (ec) {
    *error = "could not create " + out.string();
    return false;
  }

  for (const auto& child : dir->children()) {
    const std::string name = child->name();
    if (IsUnsafeComponent(name)) {
      *error = "the disc image contains an unsafe path component";
      return false;
    }
    const std::filesystem::path target = out / name;

    if (IsDirectory(child.get())) {
      if (!ExtractTree(child.get(), target, total, done, error)) return false;
      continue;
    }

    auto mapped = child->OpenMapped(rex::memory::MappedMemory::Mode::kRead);
    if (!mapped) {
      *error = "could not read " + name + " from the disc image";
      return false;
    }
    std::ofstream file(target, std::ios::binary | std::ios::trunc);
    if (!file) {
      *error = "could not write " + target.string();
      return false;
    }
    const char* data = reinterpret_cast<const char*>(mapped->data());
    size_t remaining = mapped->size();
    size_t offset = 0;
    while (remaining > 0) {
      const size_t chunk = remaining < (4u << 20) ? remaining : (4u << 20);
      file.write(data + offset, static_cast<std::streamsize>(chunk));
      if (!file) {
        *error = "ran out of room writing " + target.string();
        return false;
      }
      offset += chunk;
      remaining -= chunk;
      *done += chunk;
      // One line per chunk on stdout. The launcher reads these to move a bar;
      // nothing else parses them, so the format stays trivially greppable.
      std::fprintf(g_out, "PROGRESS %.4f %llu %llu\n",
                   total ? double(*done) / double(total) : 1.0, (unsigned long long)*done,
                   (unsigned long long)total);
      std::fflush(g_out);
    }
  }
  return true;
}

}  // namespace

int RunIdentify(int argc, char** argv) {
  // No ClaimStdout() here: main() already did it before forking, and doing it
  // twice would dup the *replaced* fd 1 - which is stderr by then - so every
  // byte of output would land on the diagnostic channel and stdout would be
  // empty. The child inherits g_out across fork, which is the whole point.
  std::string image;
  std::string extract_to;
  bool json = false;

  for (int i = 1; i < argc; ++i) {
    const std::string_view arg = argv[i];
    if (arg.rfind("--identify_disc=", 0) == 0) {
      image = std::string(arg.substr(std::strlen("--identify_disc=")));
    } else if (arg.rfind("--extract_to=", 0) == 0) {
      extract_to = std::string(arg.substr(std::strlen("--extract_to=")));
    } else if (arg == "--json") {
      json = true;
    }
  }
  (void)json;  // JSON is the only output shape; the flag exists for symmetry.

  if (image.empty()) {
    std::fprintf(stderr, "usage: thps_p8_identify --identify_disc=<image> [--extract_to=<dir>] --json\n");
    return 2;
  }

  std::error_code ec;
  if (!std::filesystem::exists(image, ec)) {
    EmitJson("IO_ERROR", "that file could not be found");
    return 1;
  }

  auto disc = std::make_unique<rex::filesystem::DiscImageDevice>(kMountPath, image);
  if (!disc->Initialize()) {
    EmitJson("NOT_A_DISC", "no Xbox 360 disc structure was found in that file");
    return 1;
  }

  rex::filesystem::Entry* xex = disc->ResolvePath("default.xex");
  if (!xex) {
    EmitJson("NOT_THIS_GAME", "that disc image has no game executable on it");
    return 1;
  }

  auto mapped = xex->OpenMapped(rex::memory::MappedMemory::Mode::kRead);
  if (!mapped) {
    EmitJson("IO_ERROR", "the game executable on that disc could not be read");
    return 1;
  }
  const std::string digest = rex::crypto::sha256(
      std::string_view(reinterpret_cast<const char*>(mapped->data()), mapped->size()));

  const thps::identify::SupportedDump* match = nullptr;
  for (const auto& row : thps::identify::kSupportedDumps) {
    if (row.sha256 == digest) {
      match = &row;
      break;
    }
  }

  if (!match) {
    // Same game, different release is a distinct outcome from a different game
    // entirely, and the two want different sentences on screen. Without a XEX
    // header parser here, size is the available proxy for "this is the same
    // title, built differently" - it is deliberately conservative.
    bool same_title_shape = false;
    for (const auto& row : thps::identify::kSupportedDumps) {
      if (xex->size() == row.xex_size) same_title_shape = true;
    }
    EmitJsonObserved(same_title_shape ? "SAME_TITLE_WRONG_BUILD" : "NOT_THIS_GAME",
                     "that disc is not the supported release", digest, xex->size());
    // Said on the diagnostic channel too, because the person who most needs to
    // read this is looking at a terminal, not parsing JSON.
    std::fprintf(stderr,
                 "\n"
                 "This disc is not a release this port knows about.\n"
                 "\n"
                 "  default.xex sha256: %s\n"
                 "  default.xex size:   %llu bytes\n"
                 "\n"
                 "If you believe this is a legitimate retail disc - a PAL or NTSC-J release,\n"
                 "say - those two lines are exactly what we need to add support for it.\n"
                 "Open an issue with them. Do not send the file itself.\n"
                 "\n",
                 digest.c_str(), (unsigned long long)xex->size());
    return 1;
  }

  if (extract_to.empty()) {
    std::fprintf(
        g_out,
        "{\n"
        "  \"verdict\": \"EXACT\",\n"
        "  \"release\": \"%s\",\n"
        "  \"files\": %llu,\n"
        "  \"bytes\": %llu\n"
        "}\n",
        std::string(match->release).c_str(), (unsigned long long)disc->file_count(),
        (unsigned long long)disc->total_file_size());
    return 0;
  }

  // Only ever reached on EXACT: nothing is copied until the disc is known to be
  // the right one.
  rex::filesystem::Entry* root = const_cast<rex::filesystem::Entry*>(disc->root());
  if (!root) {
    EmitJson("IO_ERROR", "that disc image could not be read");
    return 1;
  }
  const uint64_t total = CountBytes(root);
  uint64_t done = 0;
  std::string error;
  if (!ExtractTree(root, extract_to, total, &done, &error)) {
    EmitJson("IO_ERROR", error);
    return 1;
  }
  std::fprintf(
      g_out,
      "{\n"
      "  \"verdict\": \"EXACT\",\n"
      "  \"release\": \"%s\",\n"
      "  \"extracted_bytes\": %llu\n"
      "}\n",
      std::string(match->release).c_str(), (unsigned long long)done);
  return 0;
}

// Set in the contained child so it knows to do the work rather than spawn
// another one. Only meaningful on Windows, which has no fork.
constexpr const char* kChildMarker = "THPS_P8_IDENTIFY_CONTAINED";

// The SDK's XDVDFS parser is not hardened against malformed input: a disc image
// truncated *after* its header but before its data walks entries that point past
// the end of the mapping and dies on SIGSEGV, with no chance for this program to
// say anything. A truncated dump has to fail closed, and "segfault" is not
// failing closed - it is indistinguishable from a bug in the launcher.
//
// So the parse runs in a child process. Anything that kills it becomes a verdict
// instead of a crash. This contains every malformed-input crash in that parser,
// not just the one truncation case that exposed it.
//
// Two mechanisms, one property. POSIX forks, which is free and needs no
// argument marshalling. Windows has no fork, so it re-runs this executable with
// a marker in the environment and the original command line handed straight
// back - GetCommandLineA rather than a rebuilt argv, so quoting cannot drift
// between the two.

int main(int argc, char** argv) {
#if defined(_WIN32)
  if (::GetEnvironmentVariableA(kChildMarker, nullptr, 0) != 0) {
    ClaimStdout();
    return RunIdentify(argc, argv);
  }

  if (!::SetEnvironmentVariableA(kChildMarker, "1")) {
    // Without the marker the child would spawn its own child forever. Refusing
    // is the only safe answer.
    EmitJson("IO_ERROR", "could not check that file");
    return 1;
  }

  STARTUPINFOA si{};
  si.cb = sizeof(si);
  PROCESS_INFORMATION pi{};
  std::string command = ::GetCommandLineA();
  if (!::CreateProcessA(nullptr, command.data(), nullptr, nullptr,
                        /*bInheritHandles=*/TRUE, /*flags=*/0, nullptr, nullptr, &si, &pi)) {
    EmitJson("IO_ERROR", "could not check that file");
    return 1;
  }
  ::CloseHandle(pi.hThread);
  ::WaitForSingleObject(pi.hProcess, INFINITE);
  DWORD code = 0;
  const BOOL got = ::GetExitCodeProcess(pi.hProcess, &code);
  ::CloseHandle(pi.hProcess);
  if (!got) {
    EmitJson("IO_ERROR", "could not check that file");
    return 1;
  }
  // Structured-exception codes are 0xC0000000-flagged. An access violation from
  // the parser lands here exactly as a SIGSEGV does on POSIX.
  if ((code & 0xF0000000u) == 0xC0000000u) {
    std::fprintf(stderr, "thps_p8_identify: disc parse died with exception 0x%08lX\n",
                 (unsigned long)code);
    EmitJson("NOT_A_DISC",
             "that disc image looks incomplete - it may not have finished copying");
    return 1;
  }
  return int(code);
#else
  ClaimStdout();
  const pid_t pid = ::fork();
  if (pid < 0) {
    EmitJson("IO_ERROR", "could not check that file");
    return 1;
  }
  if (pid == 0) {
    const int rc = RunIdentify(argc, argv);
    std::fflush(g_out);
    std::fflush(stdout);
    std::fflush(stderr);
    ::_exit(rc);
  }

  int status = 0;
  while (::waitpid(pid, &status, 0) < 0) {
    if (errno != EINTR) {
      EmitJson("IO_ERROR", "could not check that file");
      return 1;
    }
  }
  if (WIFSIGNALED(status)) {
    std::fprintf(stderr, "thps_p8_identify: disc parse died on signal %d\n", WTERMSIG(status));
    EmitJson("NOT_A_DISC",
             "that disc image looks incomplete - it may not have finished copying");
    return 1;
  }
  return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
#endif
}
