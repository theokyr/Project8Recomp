// POSIX implementation of platform.h. Included by platform.cpp; not a
// standalone translation unit.

#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstring>

#include <fcntl.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

namespace thps::platform {
namespace {

// handles_[0] is the read end of the child's stdout pipe, +1 so that a
// default-constructed Child (all zeroes) is never mistaken for fd 0.
int ReadFd(const std::uint64_t* h) { return int(h[0]) - 1; }
void SetReadFd(std::uint64_t* h, int fd) { h[0] = std::uint64_t(fd + 1); }

}  // namespace

Child::~Child() {
  if (!valid_) return;
  const int fd = ReadFd(handles_);
  if (fd >= 0) ::close(fd);
  if (pid_ > 0) {
    // Reap, so a cancelled extraction does not leave a zombie behind for the
    // rest of the launcher's life. RequestStop has already been called in the
    // cancel path; in the normal path the child is long gone and this returns
    // immediately.
    int status = 0;
    while (::waitpid(pid_, &status, WNOHANG) < 0 && errno == EINTR) {
    }
  }
}

Child::Child(Child&& other) noexcept {
  *this = std::move(other);
}

Child& Child::operator=(Child&& other) noexcept {
  if (this == &other) return *this;
  valid_ = other.valid_;
  handles_[0] = other.handles_[0];
  handles_[1] = other.handles_[1];
  pid_ = other.pid_;
  other.valid_ = false;
  other.handles_[0] = other.handles_[1] = 0;
  other.pid_ = -1;
  return *this;
}

Child Start(const std::vector<std::string>& argv) {
  Child child;
  if (argv.empty()) return child;

  int fds[2] = {-1, -1};
  if (::pipe(fds) != 0) return child;

  // posix_spawn rather than fork+exec: fork in a process that has SDL, a GPU
  // context and RmlUi's allocators live is a good way to inherit a lock held by
  // a thread that does not exist in the child. posix_spawn is the vfork-shaped
  // path and touches none of it.
  posix_spawn_file_actions_t actions;
  posix_spawn_file_actions_init(&actions);
  posix_spawn_file_actions_addclose(&actions, fds[0]);
  posix_spawn_file_actions_adddup2(&actions, fds[1], STDOUT_FILENO);
  posix_spawn_file_actions_addclose(&actions, fds[1]);

  std::vector<char*> raw;
  raw.reserve(argv.size() + 1);
  for (const std::string& arg : argv) raw.push_back(const_cast<char*>(arg.c_str()));
  raw.push_back(nullptr);

  pid_t pid = -1;
  const int rc = ::posix_spawn(&pid, argv[0].c_str(), &actions, nullptr, raw.data(), environ);
  posix_spawn_file_actions_destroy(&actions);
  ::close(fds[1]);

  if (rc != 0) {
    ::close(fds[0]);
    std::fprintf(stderr, "thps_p8_gui: could not start %s: %s\n", argv[0].c_str(),
                 std::strerror(rc));
    return child;
  }

  // CLOEXEC on the read end so the *game* does not inherit it across the
  // handoff. The game must not inherit the launcher's descriptors - a GPU or
  // pipe fd held open by a process that has exited is a leak nobody can see.
  ::fcntl(fds[0], F_SETFD, FD_CLOEXEC);

  SetReadFd(child.handles_, fds[0]);
  child.pid_ = pid;
  child.valid_ = true;
  return child;
}

int Child::Drain(const std::function<void(const std::string&)>& on_line) {
  if (!valid_) return -1;
  const int fd = ReadFd(handles_);
  if (fd < 0) return -1;

  std::string pending;
  char buf[4096];
  for (;;) {
    const ssize_t n = ::read(fd, buf, sizeof(buf));
    if (n < 0) {
      if (errno == EINTR) continue;
      break;
    }
    if (n == 0) break;  // child closed stdout
    pending.append(buf, size_t(n));
    size_t start = 0;
    for (;;) {
      const size_t nl = pending.find('\n', start);
      if (nl == std::string::npos) break;
      on_line(pending.substr(start, nl - start));
      start = nl + 1;
    }
    pending.erase(0, start);
  }
  if (!pending.empty()) on_line(pending);

  int status = 0;
  while (::waitpid(pid_, &status, 0) < 0) {
    if (errno != EINTR) return -1;
  }
  pid_ = -1;  // reaped; the destructor must not try again
  if (WIFSIGNALED(status)) return -1;  // stopped, or died - both are "not a result"
  return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

void Child::RequestStop() {
  if (!valid_ || pid_ <= 0) return;
  ::kill(pid_, SIGTERM);
}

void RequestStopByPid(int pid) {
  if (pid > 0) ::kill(pid, SIGTERM);
}

bool OpenFolder(const std::filesystem::path& path) {
  // posix_spawn, not system("xdg-open ... &"): the shell's exit status is not
  // the opener's, so the old failure branch could never fire. Detached by
  // design - xdg-open returns quickly but the file manager it starts should
  // outlive this call, and this process is about to exit anyway.
  const std::string target = path.string();
  const char* argv[] = {"xdg-open", target.c_str(), nullptr};
  pid_t pid = -1;
  const int rc = ::posix_spawnp(&pid, "xdg-open", nullptr, nullptr,
                                const_cast<char* const*>(argv), environ);
  if (rc != 0) return false;
  // Do not block on it, but do not leave a zombie either: a second-long wait is
  // longer than xdg-open takes to hand off and shorter than a user notices.
  int status = 0;
  for (int i = 0; i < 100; ++i) {
    const pid_t done = ::waitpid(pid, &status, WNOHANG);
    if (done == pid) return WIFEXITED(status) && WEXITSTATUS(status) == 0;
    if (done < 0) return true;
    ::usleep(10 * 1000);
  }
  return true;  // still running after a second, which is not a failure
}

[[noreturn]] void ExecReplace(const std::vector<std::string>& argv) {
  std::vector<char*> raw;
  raw.reserve(argv.size() + 1);
  for (const std::string& arg : argv) raw.push_back(const_cast<char*>(arg.c_str()));
  raw.push_back(nullptr);
  ::execv(argv[0].c_str(), raw.data());
  std::fprintf(stderr, "thps_p8_gui: could not start %s: %s\n", argv[0].c_str(),
               std::strerror(errno));
  std::_Exit(1);
}

std::filesystem::path SiblingExe(const std::filesystem::path& dir, const char* stem) {
  return dir / stem;
}

}  // namespace thps::platform
