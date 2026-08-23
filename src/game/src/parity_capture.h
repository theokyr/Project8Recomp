// parity_capture.h - guest-output parity capture harness for ReXGlue recomps.
//
// The pixel oracle the
// residency work is graded against. It captures the PRESENTER's guest output
// image (rex::ui::Presenter::CaptureGuestOutput) - not the desktop, not the
// window - so overlays, cursors and window chrome can never contaminate a
// parity comparison.
//
// Wiring (two lines in the app, matching rexglue_script_input.h):
//
//   #include "parity_capture.h"
//   ...
//   void OnCreateDialogs(rex::ui::ImGuiDrawer*) override { parity_.RegisterBinds(); }
//   void OnPostSetup() override                          { parity_.Start(runtime()); }
//   void OnShutdown() override                           { parity_.Stop(); }
//   parity_capture::ParityCapture parity_;
//
// Flags (underscore cvar syntax, like every runtime flag):
//
//   --parity_capture_frames=1800,1830,3600   guest frame indices to capture
//   --parity_capture_dir=/path/to/parity     output dir override
//   --parity_selftest=true                   double-capture byte-equality test
//   --parity_frame_counter=0x82970A10        guest swap counter address
//   --parity_selftest_attempts=3             retries for the self-test pair
//   --parity_toggle_schedule=90,110,130      seconds at which to flip a boolean cvar
//   --parity_toggle_cvar=gpu_native_prefetch boolean cvar to flip (default residency)
//   --parity_toggle_lead_frames=2            frames between toggle and capture
//
// F10 captures on demand at whatever frame is current; F5 flips residency.
//
// Toggle pairs. The primary parity oracle is a
// mid-run pair: capture frame N, flip gpu_native_residency, capture the first
// frame that renders in the new mode. Driving that from outside the process is
// impossible - synthetic keypresses never reach the bind path on an unfocused
// XWayland window, which is what blocked the earlier keypress-driven
// gate - so --parity_toggle_schedule drives it in-process. Each entry is a
// number of seconds after the first presented guest frame; at that time the
// harness captures, waits --parity_toggle_lead_frames guest frames, writes the
// cvar through the same rex::cvar::SetFlagByName path the F5 bind uses (the
// bind stays, for humans), waits the same number of frames again, and captures
// the second half of the pair.
//
// A toggle run produces cross-mode pairs ONLY. The full protocol also needs
// same-mode control pairs, and parity_diff.py reports INCOMPLETE without them:
// take those from a second run (or a second --parity_capture_dir) with
// --parity_capture_frames=N,N+4 at the same scene point. They must not share a
// directory with toggle captures - parity_diff.py chunks every capture in the
// directory into adjacent pairs, so one control capture landing between a
// site's two halves would mispair every site after it.
//
// Lead default is 2, not 1. The cvar is latched by the command processor at a
// frame boundary, so the frame after the write already renders in the new mode
// - but the counter/output correspondence is ambiguous by exactly one frame
// (see below), so a capture one frame past the toggle could still be the last
// old-mode image. Two frames is the smallest lead that cannot straddle the
// latch in either direction, and it is symmetric on the "before" side.
//
// Frame indexing. There is no app-visible frame counter in the SDK, so the
// harness polls the GAME's swap-complete counter: the u64 at guest 0x82970A10,
// incremented only by sub_823975B8, the swap-complete callback the title
// registers with its Present path. It is bumped from the guest ISR dispatched
// synchronously on the GPU command-processor thread as part of the swap tail,
// i.e. once per presented guest frame. That address is title-specific, hence
// the cvar; point it elsewhere (or at 0) for another title. Because the
// interrupt packet and PM4_XE_SWAP are two separate packets in the same swap
// tail, an observed counter value of N may correspond to the guest output of
// frame N or N-1 depending on which the guest wrote first - the offset is
// CONSTANT across a run and therefore cancels in the toggle-pair protocol.
// Every capture records the counter value observed immediately before and
// after the readback, so the ambiguity is visible in the manifest.
//
// ------------------------------------------------------------------ marks
//
// RequestNamedCapture(label, t_ms) is the same capture path with a caller-chosen
// name instead of a frame index, and it exists for one consumer: replay marks.
// A marked fixture names the moments someone already decided are worth looking
// at, so a screenshot at each of them turns "the cutscene looked wrong" into a
// pair of PNGs that can be put side by side. The app wires it in
// (thps_p8_app.h: --mark_screenshot), so the harness itself stays a screenshot
// service and knows nothing about the input layer.
//
// Named captures go to their OWN directory (--mark_screenshot_dir, default
// <run dir>/marks) and their own marks.csv, never into the parity directory.
// That is not tidiness: parity_diff.py chunks every capture in a directory into
// adjacent pairs, so one mark screenshot landing between a toggle site's two
// halves would mispair every site after it.
//
// Cost when idle: zero. No thread is created and no guest memory is read
// unless --parity_capture_frames or --parity_toggle_schedule is non-empty, or
// F10 is pressed, or a mark fires.
//
// Output: <dir>/frame_<counter>_<mode>.png plus a captures.csv manifest.
// <mode> is read from parity_toggle_cvar by name (cross-module registry query),
// so this header does not need the target patch to build or run - an absent
// target reads as its off mode. Toggle-pair captures are named and manifested
// exactly like scheduled ones - only the manifest's trigger column distinguishes
// them (toggle_before / toggle_after) - because parity_diff.py forms its pairs
// by chunking captures in frame order, so any decoration that changed the
// parsed mode or the sort key would mispair the sites.
//
// PNG encoding is self-contained (stored-deflate blocks + CRC32/Adler32), so
// the harness pulls in no image library and no zlib link dependency. The
// files are ordinary lossless PNGs consumable by any image comparison tool.

#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <deque>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <fmt/format.h>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/memory.h>
#include <rex/runtime.h>
#include <rex/system/interfaces/graphics.h>
#include <rex/types.h>
#include <rex/ui/keybinds.h>
#include <rex/ui/presenter.h>

REXCVAR_DEFINE_STRING(parity_capture_frames, "", "Parity",
                      "Comma-separated guest frame indices to capture the guest "
                      "output at (empty = manual F10 only)");
REXCVAR_DEFINE_STRING(parity_capture_dir, "", "Parity",
                      "Directory for parity captures (default: <run dir>/parity, "
                      "derived from --perf_log_csv or --log_file)");
REXCVAR_DEFINE_BOOL(parity_selftest, false, "Parity",
                    "Capture twice back-to-back at every trigger and log whether "
                    "the two readbacks are byte-equal");
REXCVAR_DEFINE_STRING(parity_frame_counter, "0x82970A10", "Parity",
                      "Guest address of the big-endian u64 swap-complete counter "
                      "used as the frame index (0 disables frame scheduling)");
REXCVAR_DEFINE_UINT32(parity_selftest_attempts, 3, "Parity",
                      "Self-test capture pairs to try before reporting a mismatch "
                      "(a guest frame landing between the two readbacks is a "
                      "legitimate difference)");
REXCVAR_DEFINE_STRING(parity_toggle_schedule, "", "Parity",
                      "Comma-separated seconds after the first presented guest "
                      "frame at which to flip parity_toggle_cvar, each toggle "
                      "bracketed by a capture pair (empty = no toggles)");
REXCVAR_DEFINE_STRING(parity_toggle_cvar, "gpu_native_residency", "Parity",
                      "Boolean cvar flipped by parity_toggle_schedule (default "
                      "gpu_native_residency; for example gpu_native_prefetch)");
REXCVAR_DEFINE_STRING(mark_screenshot_dir, "", "Parity",
                      "Directory for named (replay-mark) screenshots (default: "
                      "<run dir>/marks; never the parity directory)");
// The title's own clock, sampled at every mark next to the frame counter. Both
// are written by the same swap-complete callback once per presented frame, so a
// mark carries "how many frames" and "how much game-side real time" from the
// same instant - which is what separates "this run drew more frames" from "this
// run's clock ran fast". Zero disables, so this costs nothing on a title that
// has no such globals.
REXCVAR_DEFINE_STRING(mark_clock_ticks, "0x82970950", "Parity",
                      "Guest address of the big-endian u64 elapsed-timebase-tick "
                      "count stamped at the last present (0 disables)");
REXCVAR_DEFINE_STRING(mark_clock_scale, "0x829709F8", "Parity",
                      "Guest address of the big-endian double holding the "
                      "title's timebase frequency in MHz, i.e. ticks per "
                      "microsecond (0 disables)");
REXCVAR_DEFINE_STRING(mark_frame_delta, "0x82970924", "Parity",
                      "Guest address of the big-endian float per-frame delta "
                      "seconds the title feeds its simulation (0 disables)");
REXCVAR_DEFINE_UINT32(parity_toggle_lead_frames, 2, "Parity",
                      "Guest frames between a scheduled toggle and each half of "
                      "its capture pair (2 = the smallest lead that cannot "
                      "straddle the frame-boundary latch)");

namespace parity_capture {

// The residency master switch, addressed by name everywhere in this header:
// the app must still build and run against an SDK that has never heard of it.
inline constexpr const char* kResidencyCvar = "gpu_native_residency";

// The last value THIS PROCESS wrote to kResidencyCvar: -1 before any write,
// then 0 or 1. Published because the cvar has a second writer - the plugin's
// sticky auto-disable latch clears it from the command-processor thread - and
// from outside the plugin the two writes are indistinguishable without knowing
// what was intended. native_indicator.h reads it to decide between EMULATED
// and LATCHED; nothing here consumes it, and nothing branches on it, so the
// capture harness behaves identically whether or not anyone is looking.
//
// Written from the F5 bind (UI thread) and the capture worker; read from the
// UI thread. Release/acquire only so a reader that sees a write sees the value
// that went with it - it orders nothing else.
inline std::atomic<int> g_residency_last_written{-1};

// The single write path for that cvar, shared by the F5 bind and by the
// scheduled toggles so a fixture run and a human keypress are the same event.
// This only writes the cvar; the command processor latches it at the next
// frame boundary and nothing here invalidates or latches anything itself.
// Returns the new value, or nullopt when the cvar is absent or the write
// failed - in which case the mode is unchanged and the paired capture will
// (correctly) read as a same-mode control pair.
//
// Called from the UI thread (F5) and from the capture worker (scheduled): a
// cvar write from a thread other than the CP thread that reads it, which is
// what the F5 path has always done. No new cross-thread hazard is introduced;
// no flag is registered or unregistered here, so the registry never moves.
inline std::optional<bool> ToggleResidency(const char* source) {
  if (!rex::cvar::GetFlagInfo(kResidencyCvar)) {
    REXLOG_WARN("native-residency: {} is not registered in this build; {} ignored", kResidencyCvar,
                source);
    return std::nullopt;
  }
  const bool next = !rex::cvar::Query<bool>(kResidencyCvar);
  if (!rex::cvar::SetFlagByName(kResidencyCvar, next ? "true" : "false")) {
    REXLOG_WARN("native-residency: could not set {}; mode unchanged", kResidencyCvar);
    return std::nullopt;
  }
  g_residency_last_written.store(next ? 1 : 0, std::memory_order_release);
  REXLOG_INFO("native-residency: {} -> {} (latches at the next frame boundary)", kResidencyCvar,
              next ? "ON" : "OFF");
  return next;
}

// The scheduled parity oracle may isolate a sub-lever while residency stays
// enabled. Keep F5's residency-specific intent tracking above, but let the
// schedule flip any named boolean cvar through the same registry write path.
inline std::optional<bool> ToggleBoolCvar(const std::string& name, const char* source) {
  if (name == kResidencyCvar) {
    return ToggleResidency(source);
  }
  if (name.empty() || !rex::cvar::GetFlagInfo(name)) {
    REXLOG_WARN("parity-toggle: {} is not registered in this build; {} ignored", name, source);
    return std::nullopt;
  }
  const bool next = !rex::cvar::Query<bool>(name);
  if (!rex::cvar::SetFlagByName(name, next ? "true" : "false")) {
    REXLOG_WARN("parity-toggle: could not set {}; mode unchanged", name);
    return std::nullopt;
  }
  REXLOG_INFO("parity-toggle: {} -> {} (latches according to the target cvar)", name,
              next ? "ON" : "OFF");
  return next;
}

namespace detail {

// ---------------------------------------------------------------- PNG writer

inline uint32_t Crc32(const uint8_t* data, size_t size, uint32_t crc) {
  static const auto table = [] {
    std::array<uint32_t, 256> t{};
    for (uint32_t n = 0; n < 256; ++n) {
      uint32_t c = n;
      for (int k = 0; k < 8; ++k) {
        c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
      }
      t[n] = c;
    }
    return t;
  }();
  for (size_t i = 0; i < size; ++i) {
    crc = table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
  }
  return crc;
}

inline void PushBe32(std::vector<uint8_t>& out, uint32_t v) {
  out.push_back(uint8_t(v >> 24));
  out.push_back(uint8_t(v >> 16));
  out.push_back(uint8_t(v >> 8));
  out.push_back(uint8_t(v));
}

inline void PushChunk(std::vector<uint8_t>& out, const char type[4],
                      const std::vector<uint8_t>& payload) {
  PushBe32(out, uint32_t(payload.size()));
  size_t crc_begin = out.size();
  out.insert(out.end(), type, type + 4);
  out.insert(out.end(), payload.begin(), payload.end());
  uint32_t crc = Crc32(out.data() + crc_begin, out.size() - crc_begin, 0xFFFFFFFFu) ^ 0xFFFFFFFFu;
  PushBe32(out, crc);
}

// Deflate "stored" (uncompressed) blocks wrapped in a zlib stream. Lossless
// and dependency-free; the file is ~1.33x the raw RGB size, which is fine for
// the handful of frames a parity run captures.
inline std::vector<uint8_t> ZlibStore(const std::vector<uint8_t>& raw) {
  std::vector<uint8_t> out;
  out.reserve(raw.size() + raw.size() / 8192 * 5 + 16);
  out.push_back(0x78);  // CMF: deflate, 32 KiB window
  out.push_back(0x01);  // FLG: no dict, fastest
  const size_t kMaxBlock = 65535;
  size_t offset = 0;
  do {
    size_t block = std::min(kMaxBlock, raw.size() - offset);
    bool final_block = (offset + block) >= raw.size();
    out.push_back(final_block ? 1 : 0);
    out.push_back(uint8_t(block & 0xFF));
    out.push_back(uint8_t(block >> 8));
    out.push_back(uint8_t(~block & 0xFF));
    out.push_back(uint8_t((~block >> 8) & 0xFF));
    out.insert(out.end(), raw.begin() + offset, raw.begin() + offset + block);
    offset += block;
  } while (offset < raw.size());
  uint32_t s1 = 1, s2 = 0;
  for (uint8_t b : raw) {
    s1 = (s1 + b) % 65521;
    s2 = (s2 + s1) % 65521;
  }
  PushBe32(out, (s2 << 16) | s1);
  return out;
}

// RawImage is R8 G8 B8 X8 with an explicit stride; the last row is not
// required to be padded to the stride.
inline bool WritePng(const std::filesystem::path& path, const rex::ui::RawImage& image) {
  if (!image.width || !image.height) {
    return false;
  }
  const size_t row_bytes = size_t(image.width) * 3;
  std::vector<uint8_t> raw;
  raw.reserve((row_bytes + 1) * image.height);
  for (uint32_t y = 0; y < image.height; ++y) {
    size_t src = size_t(y) * image.stride;
    if (src + size_t(image.width) * 4 > image.data.size()) {
      return false;
    }
    raw.push_back(0);  // filter type 0 (None)
    const uint8_t* p = image.data.data() + src;
    for (uint32_t x = 0; x < image.width; ++x, p += 4) {
      raw.push_back(p[0]);
      raw.push_back(p[1]);
      raw.push_back(p[2]);
    }
  }

  std::vector<uint8_t> png = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
  std::vector<uint8_t> ihdr;
  PushBe32(ihdr, image.width);
  PushBe32(ihdr, image.height);
  ihdr.push_back(8);  // bit depth
  ihdr.push_back(2);  // colour type: truecolour RGB
  ihdr.push_back(0);  // deflate
  ihdr.push_back(0);  // adaptive filtering
  ihdr.push_back(0);  // no interlace
  PushChunk(png, "IHDR", ihdr);
  PushChunk(png, "IDAT", ZlibStore(raw));
  PushChunk(png, "IEND", {});

  std::ofstream f(path, std::ios::binary | std::ios::trunc);
  if (!f) {
    return false;
  }
  f.write(reinterpret_cast<const char*>(png.data()), std::streamsize(png.size()));
  return bool(f);
}

// ------------------------------------------------------------------- parsing

inline std::vector<uint64_t> ParseFrameList(const std::string& text) {
  std::vector<uint64_t> frames;
  size_t i = 0;
  while (i < text.size()) {
    while (i < text.size() && !std::isdigit(uint8_t(text[i]))) ++i;
    size_t start = i;
    while (i < text.size() && std::isdigit(uint8_t(text[i]))) ++i;
    if (i > start) {
      frames.push_back(std::strtoull(text.substr(start, i - start).c_str(), nullptr, 10));
    }
  }
  std::sort(frames.begin(), frames.end());
  frames.erase(std::unique(frames.begin(), frames.end()), frames.end());
  return frames;
}

// Same shape as ParseFrameList, for the toggle schedule: anything that is not
// part of a number separates entries, fractional seconds are allowed. Times
// are relative to the first presented guest frame, so negatives are meaningless
// and a leading '-' simply reads as a separator.
inline std::vector<double> ParseTimeList(const std::string& text) {
  std::vector<double> times;
  size_t i = 0;
  while (i < text.size()) {
    while (i < text.size() && !std::isdigit(uint8_t(text[i]))) ++i;
    size_t start = i;
    while (i < text.size() && (std::isdigit(uint8_t(text[i])) || text[i] == '.')) ++i;
    if (i > start) {
      times.push_back(std::strtod(text.substr(start, i - start).c_str(), nullptr));
    }
  }
  std::sort(times.begin(), times.end());
  times.erase(std::unique(times.begin(), times.end()), times.end());
  return times;
}

inline uint32_t ParseAddress(const std::string& text) {
  if (text.empty()) {
    return 0;
  }
  return uint32_t(std::strtoull(text.c_str(), nullptr, 0));
}

inline std::string Stamp() {
  std::time_t now = std::time(nullptr);
  std::tm tm{};
#ifdef _WIN32
  localtime_s(&tm, &now);
#else
  localtime_r(&now, &tm);
#endif
  char buf[32];
  std::strftime(buf, sizeof(buf), "%Y%m%d-%H%M%S", &tm);
  return buf;
}

// The launcher never tells the app its run directory, but it always points
// --perf_log_csv (and --log_file) into it, so derive from those before
// falling back to a fresh tmp/runs/<stamp> under the working directory.
inline std::filesystem::path ResolveRunDir() {
  for (const char* cvar : {"perf_log_csv", "log_file"}) {
    std::string value = rex::cvar::Query<std::string>(cvar);
    if (value.empty()) {
      continue;
    }
    std::filesystem::path parent = std::filesystem::path(value).parent_path();
    if (!parent.empty()) {
      return parent;
    }
  }
  return std::filesystem::path("tmp") / "runs" / (Stamp() + "-parity");
}

inline std::filesystem::path ResolveOutputDir() {
  std::string explicit_dir = REXCVAR_QUERY(std::string, parity_capture_dir);
  if (!explicit_dir.empty()) {
    return std::filesystem::path(explicit_dir);
  }
  return ResolveRunDir() / "parity";
}

// Deliberately a sibling of the parity directory rather than a subdirectory of
// it: parity_diff.py walks a directory's captures and pairs them by adjacency,
// so mark screenshots must not be reachable from a parity run's output path.
inline std::filesystem::path ResolveMarksDir() {
  std::string explicit_dir = REXCVAR_QUERY(std::string, mark_screenshot_dir);
  if (!explicit_dir.empty()) {
    return std::filesystem::path(explicit_dir);
  }
  return ResolveRunDir() / "marks";
}

// "native" / "emu" for the historical residency target, or "on" / "off"
// for an isolated sub-lever. Read by name so the harness builds and runs
// against an SDK that has never heard of the target.
inline std::string ModeTag(const std::string& cvar = kResidencyCvar) {
  if (!rex::cvar::GetFlagInfo(cvar)) {
    return cvar == kResidencyCvar ? "emu" : "off";
  }
  const bool enabled = rex::cvar::Query<bool>(cvar);
  if (cvar == kResidencyCvar) {
    return enabled ? "native" : "emu";
  }
  return enabled ? "on" : "off";
}

inline bool SameImage(const rex::ui::RawImage& a, const rex::ui::RawImage& b) {
  return a.width == b.width && a.height == b.height && a.stride == b.stride &&
         a.data.size() == b.data.size() &&
         std::memcmp(a.data.data(), b.data.data(), a.data.size()) == 0;
}

}  // namespace detail

class ParityCapture {
 public:
  ParityCapture() = default;
  ~ParityCapture() { Stop(); }

  ParityCapture(const ParityCapture&) = delete;
  ParityCapture& operator=(const ParityCapture&) = delete;

  // Call from OnCreateDialogs. The runtime does not exist yet at this point,
  // so this only registers the key - nothing is read and no thread starts.
  void RegisterBinds() {
    rex::ui::RegisterBind(kBindName, "F10", "Capture guest output for parity comparison",
                          [this] { RequestManualCapture(); });
    binds_registered_ = true;
  }

  // A screenshot of the guest output, named rather than indexed, written to the
  // marks directory. Safe to call from any thread including a guest input poll:
  // it only queues, and the worker does every presenter touch.
  //
  // `label` is used verbatim in the file name, so the caller owns the ordering
  // convention (thps_p8_app.h prefixes the mark ordinal). `tag_ms` is recorded
  // in the manifest and is the caller's own clock - for marks, the input
  // timebase - so a screenshot can be lined up against the recording that asked
  // for it. Pass a negative tag when there is no such clock.
  void RequestNamedCapture(std::string label, int64_t tag_ms) {
    if (!runtime_) {
      REXLOG_WARN("parity-capture: named capture '{}' requested before the runtime "
                  "came up; dropped",
                  label);
      return;
    }
    EnsureWorker();
    {
      std::lock_guard<std::mutex> lock(mutex_);
      named_requests_.push_back(NamedRequest{std::move(label), tag_ms});
    }
    wake_.notify_all();
  }

  // Call from OnPostSetup (runtime, graphics system and presenter are live).
  void Start(rex::Runtime* runtime) {
    runtime_ = runtime;
    selftest_ = REXCVAR_QUERY(bool, parity_selftest);
    selftest_attempts_ = std::max<uint32_t>(1, REXCVAR_QUERY(uint32_t, parity_selftest_attempts));
    counter_address_ = detail::ParseAddress(REXCVAR_QUERY(std::string, parity_frame_counter));
    clock_ticks_address_ = detail::ParseAddress(REXCVAR_QUERY(std::string, mark_clock_ticks));
    clock_scale_address_ = detail::ParseAddress(REXCVAR_QUERY(std::string, mark_clock_scale));
    frame_delta_address_ = detail::ParseAddress(REXCVAR_QUERY(std::string, mark_frame_delta));
    schedule_ = detail::ParseFrameList(REXCVAR_QUERY(std::string, parity_capture_frames));
    toggle_schedule_ = detail::ParseTimeList(REXCVAR_QUERY(std::string, parity_toggle_schedule));
    toggle_cvar_ = REXCVAR_QUERY(std::string, parity_toggle_cvar);
    if (toggle_cvar_.empty()) {
      toggle_cvar_ = kResidencyCvar;
    }
    toggle_lead_frames_ = std::max<uint64_t>(1, REXCVAR_QUERY(uint32_t, parity_toggle_lead_frames));
    output_dir_ = detail::ResolveOutputDir();
    marks_dir_ = detail::ResolveMarksDir();
    if (!schedule_.empty() || !toggle_schedule_.empty()) {
      if (!counter_address_) {
        REXLOG_ERROR(
            "parity-capture: --parity_capture_frames/--parity_toggle_schedule need "
            "--parity_frame_counter; scheduling disabled (F10 still works)");
        schedule_.clear();
        toggle_schedule_.clear();
        return;
      }
      REXLOG_INFO("parity-capture: {} scheduled frame(s), self-test {}, output {}",
                  schedule_.size(), selftest_ ? "on" : "off", output_dir_.string());
      if (!toggle_schedule_.empty()) {
        REXLOG_INFO("parity-toggle: {} scheduled toggle(s) of {}, capture pair +/-{} frame(s), "
                    "clock starts at the first presented guest frame",
                    toggle_schedule_.size(), toggle_cvar_, toggle_lead_frames_);
      }
      EnsureWorker();
    }
  }

  // Call from OnShutdown.
  void Stop() {
    if (binds_registered_) {
      rex::ui::UnregisterBind(kBindName);
      binds_registered_ = false;
    }
    {
      std::lock_guard<std::mutex> lock(mutex_);
      stopping_ = true;
    }
    wake_.notify_all();
    if (worker_.joinable()) {
      worker_.join();
    }
    // The writer parks on write_wake_, which is otherwise only ever notified by
    // the worker thread - without this it would never observe stopping_ and the
    // join below would deadlock. Notified after the worker join so any capture
    // the worker queued on its way out is still drained rather than dropped.
    write_wake_.notify_all();
    if (writer_.joinable()) {
      writer_.join();
    }
    if (manifest_.is_open()) {
      manifest_.close();
    }
    if (marks_manifest_.is_open()) {
      marks_manifest_.close();
    }
  }

 private:
  struct Pending {
    uint64_t frame_before = 0;
    uint64_t frame_after = 0;
    uint64_t requested = 0;  // scheduled index, or 0 for a manual capture
    bool manual = false;
    // Manifest provenance only; never part of the file name (see the header
    // note on parity_diff.py pairing). Always a string literal.
    const char* trigger = "scheduled";
    bool selftest_run = false;
    bool selftest_equal = false;
    uint32_t selftest_attempt = 0;
    std::string mode;
    // Non-empty for a named (mark) capture: routes the write to the marks
    // directory and its own manifest instead of the parity one.
    std::string label;
    int64_t tag_ms = -1;
    // The title's own timing state at this instant. Zero when the address is
    // unset or unmapped, which the manifest reports as-is rather than hiding.
    uint64_t clock_ticks = 0;
    double clock_seconds = 0.0;
    float frame_delta = 0.0f;
    rex::ui::RawImage image;
    rex::ui::RawImage image_b;
  };

  struct NamedRequest {
    std::string label;
    int64_t tag_ms = -1;
  };

  static constexpr const char* kBindName = "bind_parity_capture";

  // One toggle site is a three-step sequence over the guest frame counter.
  enum class TogglePhase { kWaitTime, kWaitToggleFrame, kWaitAfterFrame };

  void RequestManualCapture() {
    if (!runtime_) {
      REXLOG_WARN("parity-capture: F10 pressed before the runtime came up; ignored");
      return;
    }
    EnsureWorker();
    {
      std::lock_guard<std::mutex> lock(mutex_);
      ++manual_requests_;
    }
    wake_.notify_all();
  }

  void EnsureWorker() {
    std::call_once(worker_once_, [this] {
      worker_ = std::thread([this] { WorkerMain(); });
      writer_ = std::thread([this] { WriterMain(); });
    });
  }

  rex::ui::Presenter* GetPresenter() const {
    if (!runtime_) {
      return nullptr;
    }
    auto* graphics = runtime_->graphics_system();
    return graphics ? graphics->presenter() : nullptr;
  }

  // Same guard as ResolveCounter, for the timing globals. Resolved lazily on
  // every read rather than cached: these are read a handful of times per run
  // (once per mark), so the lookup cost is irrelevant and a null result stays
  // correct if the heap is not up yet.
  const uint8_t* ResolveGuest(uint32_t address) const {
    if (!address || !runtime_ || !runtime_->memory()) {
      return nullptr;
    }
    auto* memory = runtime_->memory();
    if (!memory->LookupHeap(address)) {
      return nullptr;
    }
    return memory->TranslateVirtual<const uint8_t*>(address);
  }

  static double ReadGuestDouble(const uint8_t* p) {
    if (!p) return 0.0;
    uint64_t raw = 0;
    std::memcpy(&raw, p, sizeof(raw));
    raw = rex::byte_swap(raw);
    double out = 0.0;
    std::memcpy(&out, &raw, sizeof(out));
    return out;
  }

  static float ReadGuestFloat(const uint8_t* p) {
    if (!p) return 0.0f;
    uint32_t raw = 0;
    std::memcpy(&raw, p, sizeof(raw));
    raw = rex::byte_swap(raw);
    float out = 0.0f;
    std::memcpy(&out, &raw, sizeof(out));
    return out;
  }

  const uint8_t* ResolveCounter() const {
    if (!counter_address_ || !runtime_ || !runtime_->memory()) {
      return nullptr;
    }
    auto* memory = runtime_->memory();
    if (!memory->LookupHeap(counter_address_)) {
      return nullptr;
    }
    return memory->TranslateVirtual<const uint8_t*>(counter_address_);
  }

  static uint64_t ReadCounter(const uint8_t* p) {
    if (!p) {
      return 0;
    }
    uint64_t raw = 0;
    std::memcpy(&raw, p, sizeof(raw));
    return rex::byte_swap(raw);
  }

  // Poll thread: owns all CaptureGuestOutput calls. Encoding happens on the
  // writer thread so two scheduled frames can be adjacent.
  void WorkerMain() {
    const uint8_t* counter = nullptr;
    size_t next = 0;
    bool warned_no_counter = false;
    for (;;) {
      uint64_t manual = 0;
      std::vector<NamedRequest> named;
      {
        std::unique_lock<std::mutex> lock(mutex_);
        bool idle = (schedule_.empty() || next >= schedule_.size()) &&
                    (toggle_schedule_.empty() || toggle_next_ >= toggle_schedule_.size());
        if (idle) {
          wake_.wait(lock, [this] {
            return stopping_ || manual_requests_ > 0 || !named_requests_.empty();
          });
        }
        if (stopping_) {
          break;
        }
        manual = manual_requests_;
        manual_requests_ = 0;
        named.assign(named_requests_.begin(), named_requests_.end());
        named_requests_.clear();
      }

      if (!counter && counter_address_) {
        counter = ResolveCounter();
        if (!counter && !warned_no_counter) {
          warned_no_counter = true;
          REXLOG_WARN("parity-capture: guest address {:#010x} is not mapped; frame scheduling off",
                      counter_address_);
          // Do not spin on an address that will never resolve; F10 still works.
          std::lock_guard<std::mutex> lock(mutex_);
          schedule_.clear();
          toggle_schedule_.clear();
        }
      }

      for (uint64_t i = 0; i < manual; ++i) {
        DoCapture(counter, /*requested=*/0, /*manual=*/true, "f10");
      }

      // Marks arrive on their own clock, so they are serviced before the frame
      // schedule is consulted: a named capture must land as close as this
      // thread can get it to the moment the mark fired.
      for (const NamedRequest& request : named) {
        DoCapture(counter, /*requested=*/0, /*manual=*/false, "mark", request.label,
                  request.tag_ms);
      }

      if (next < schedule_.size() && counter) {
        uint64_t now = ReadCounter(counter);
        if (now >= schedule_[next]) {
          uint64_t requested = schedule_[next];
          // Skip any targets the guest already ran past (a long PNG write or a
          // load-screen frame burst); they are reported, never silently lost.
          while (next < schedule_.size() && schedule_[next] <= now) {
            if (schedule_[next] != requested) {
              REXLOG_WARN("parity-capture: frame {} missed (counter already at {})",
                          schedule_[next], now);
            }
            ++next;
          }
          DoCapture(counter, requested, /*manual=*/false, "scheduled");
          continue;
        }
      }

      ServiceToggles(counter);

      if (next < schedule_.size() || toggle_next_ < toggle_schedule_.size()) {
        std::this_thread::sleep_for(std::chrono::microseconds(250));
      }
    }
  }

  // Toggle-pair protocol, driven in-process because
  // synthetic keypresses never reach the bind path on an unfocused window
  // an unfocused window. Per site: capture, wait lead frames, flip the
  // cvar, wait lead frames, capture. Sites are strictly sequential, so the two
  // captures of a site are always adjacent in frame order and a later site
  // never interleaves with an earlier one - which is exactly what
  // parity_diff.py's adjacent-pair chunking needs to see.
  void ServiceToggles(const uint8_t* counter) {
    if (!counter || toggle_next_ >= toggle_schedule_.size()) {
      return;
    }
    const uint64_t now = ReadCounter(counter);
    if (!toggle_clock_started_) {
      if (!now) {
        return;  // nothing presented yet: the schedule's origin does not exist
      }
      toggle_clock_started_ = true;
      toggle_t0_ = std::chrono::steady_clock::now();
      REXLOG_INFO("parity-toggle: clock started at guest frame {}; {} site(s) queued", now,
                  toggle_schedule_.size());
    }

    switch (toggle_phase_) {
      case TogglePhase::kWaitTime: {
        double elapsed =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - toggle_t0_).count();
        if (elapsed < toggle_schedule_[toggle_next_]) {
          return;
        }
        DoCapture(counter, now, /*manual=*/false, "toggle_before");
        toggle_target_ = ReadCounter(counter) + toggle_lead_frames_;
        toggle_phase_ = TogglePhase::kWaitToggleFrame;
        return;
      }
      case TogglePhase::kWaitToggleFrame: {
        if (now < toggle_target_) {
          return;
        }
        std::optional<bool> next_mode = ToggleBoolCvar(toggle_cvar_, "scheduled toggle");
        const bool residency_target = toggle_cvar_ == kResidencyCvar;
        REXLOG_INFO(
            "parity-toggle: site {}/{} (t={:.1f}s) fired at guest frame {}: {} {}",
            toggle_next_ + 1, toggle_schedule_.size(), toggle_schedule_[toggle_next_], now,
            toggle_cvar_, next_mode ? (*next_mode ? (residency_target ? "-> native" : "-> on")
                                                     : (residency_target ? "-> emu" : "-> off"))
                                    : "UNCHANGED (write failed)");
        toggle_target_ = now + toggle_lead_frames_;
        toggle_phase_ = TogglePhase::kWaitAfterFrame;
        return;
      }
      case TogglePhase::kWaitAfterFrame: {
        if (now < toggle_target_) {
          return;
        }
        DoCapture(counter, now, /*manual=*/false, "toggle_after");
        ++toggle_next_;
        toggle_phase_ = TogglePhase::kWaitTime;
        return;
      }
    }
  }

  void DoCapture(const uint8_t* counter, uint64_t requested, bool manual, const char* trigger,
                 std::string label = {}, int64_t tag_ms = -1) {
    auto* presenter = GetPresenter();
    if (!presenter) {
      REXLOG_WARN("parity-capture: no presenter (detached mode?); capture dropped");
      return;
    }
    Pending pending;
    pending.requested = requested;
    pending.manual = manual;
    pending.trigger = trigger;
    pending.label = std::move(label);
    pending.tag_ms = tag_ms;
    pending.mode = detail::ModeTag(toggle_cvar_);
    pending.frame_before = ReadCounter(counter);
    // Only named captures pay for this - the parity manifest has no column for
    // it and the toggle protocol does not want the extra guest reads.
    if (!pending.label.empty()) {
      pending.clock_ticks = ReadCounter(ResolveGuest(clock_ticks_address_));
      // The stored value is the timebase frequency in MHz (49.875 on Xenon),
      // not a tick->seconds scale: seconds = ticks / (MHz * 1e6). Guard the
      // divide so an unmapped address reports 0 rather than an infinity.
      const double mhz = ReadGuestDouble(ResolveGuest(clock_scale_address_));
      pending.clock_seconds =
          mhz > 0.0 ? double(pending.clock_ticks) / (mhz * 1.0e6) : 0.0;
      pending.frame_delta = ReadGuestFloat(ResolveGuest(frame_delta_address_));
    }

    if (selftest_) {
      for (uint32_t attempt = 1; attempt <= selftest_attempts_; ++attempt) {
        rex::ui::RawImage a;
        rex::ui::RawImage b;
        if (!presenter->CaptureGuestOutput(a) || !presenter->CaptureGuestOutput(b)) {
          REXLOG_WARN("parity-capture: CaptureGuestOutput failed (no guest output yet?)");
          return;
        }
        pending.selftest_run = true;
        pending.selftest_attempt = attempt;
        pending.selftest_equal = detail::SameImage(a, b);
        pending.image = std::move(a);
        pending.image_b = std::move(b);
        if (pending.selftest_equal) {
          break;
        }
      }
      REXLOG_INFO(
          "parity-capture: self-test frame {} attempt {}/{}: two back-to-back readbacks are {}",
          pending.frame_before, pending.selftest_attempt, selftest_attempts_,
          pending.selftest_equal ? "BYTE-EQUAL" : "DIFFERENT (a guest frame landed between them)");
    } else {
      if (!presenter->CaptureGuestOutput(pending.image)) {
        REXLOG_WARN("parity-capture: CaptureGuestOutput failed (no guest output yet?)");
        return;
      }
    }
    pending.frame_after = ReadCounter(counter);

    {
      std::lock_guard<std::mutex> lock(mutex_);
      queue_.push_back(std::move(pending));
    }
    write_wake_.notify_all();
  }

  // Writer thread: PNG encoding and manifest bookkeeping, off the poll loop.
  void WriterMain() {
    for (;;) {
      Pending pending;
      {
        std::unique_lock<std::mutex> lock(mutex_);
        write_wake_.wait(lock, [this] { return stopping_ || !queue_.empty(); });
        if (queue_.empty()) {
          if (stopping_) {
            break;
          }
          continue;
        }
        pending = std::move(queue_.front());
        queue_.pop_front();
      }
      WriteCapture(pending);
    }
  }

  void WriteCapture(const Pending& pending) {
    const bool named = !pending.label.empty();
    const std::filesystem::path& dir = named ? marks_dir_ : output_dir_;
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    if (ec) {
      REXLOG_ERROR("parity-capture: cannot create {}: {}", dir.string(), ec.message());
      return;
    }
    // A named capture is identified by its label and its mode, never by a frame
    // index: two runs of one fixture do not agree on frame numbers, and lining
    // the same mark up across runs is the entire reason these files exist.
    std::string base = named ? fmt::format("mark_{}_{}", pending.label, pending.mode)
                             : fmt::format("frame_{}_{}", pending.frame_before, pending.mode);
    if (pending.manual) {
      base += "_manual";
    }
    // Two triggers can land on the same counter value (F10 spam, or a guest
    // frame that never advanced); never silently overwrite a capture.
    if (std::filesystem::exists(dir / (base + ".png"))) {
      for (uint32_t n = 2;; ++n) {
        std::string candidate = fmt::format("{}_{}", base, n);
        if (!std::filesystem::exists(dir / (candidate + ".png"))) {
          base = candidate;
          break;
        }
      }
    }
    std::filesystem::path png = dir / (base + ".png");
    if (!detail::WritePng(png, pending.image)) {
      REXLOG_ERROR("parity-capture: failed to write {}", png.string());
      return;
    }
    std::filesystem::path png_b;
    if (pending.selftest_run) {
      png_b = dir / (base + "_b.png");
      if (!detail::WritePng(png_b, pending.image_b)) {
        REXLOG_ERROR("parity-capture: failed to write {}", png_b.string());
        png_b.clear();
      }
    }
    REXLOG_INFO("parity-capture: wrote {} ({}x{}, counter {}..{})", png.string(),
                pending.image.width, pending.image.height, pending.frame_before,
                pending.frame_after);
    if (named) {
      AppendMarkManifest(pending, png);
    } else {
      AppendManifest(pending, png, png_b);
    }
  }

  // Named captures get their own manifest, in their own directory, with the
  // guest frame counter alongside the mark's input-clock time. That pairing is
  // the measurement: two runs of one fixture reach the same mark at the same
  // input millisecond by construction, so the counter difference between them
  // is how many more guest frames one run drew to get there.
  void AppendMarkManifest(const Pending& pending, const std::filesystem::path& png) {
    if (!marks_manifest_.is_open()) {
      std::filesystem::path path = marks_dir_ / "marks.csv";
      bool fresh = !std::filesystem::exists(path);
      marks_manifest_.open(path, std::ios::app);
      if (!marks_manifest_) {
        return;
      }
      if (fresh) {
        marks_manifest_ << "label,mark_t_ms,frame_before,frame_after,guest_ticks,"
                           "guest_seconds,frame_delta_s,mode,width,height,file\n";
      }
    }
    marks_manifest_ << pending.label << ',' << pending.tag_ms << ',' << pending.frame_before
                    << ',' << pending.frame_after << ',' << pending.clock_ticks << ','
                    << fmt::format("{:.6f}", pending.clock_seconds) << ','
                    << fmt::format("{:.6f}", pending.frame_delta) << ',' << pending.mode << ','
                    << pending.image.width << ',' << pending.image.height << ','
                    << png.filename().string() << '\n';
    marks_manifest_.flush();
  }

  void AppendManifest(const Pending& pending, const std::filesystem::path& png,
                      const std::filesystem::path& png_b) {
    if (!manifest_.is_open()) {
      std::filesystem::path path = output_dir_ / "captures.csv";
      bool fresh = !std::filesystem::exists(path);
      manifest_.open(path, std::ios::app);
      if (!manifest_) {
        return;
      }
      if (fresh) {
        manifest_ << "requested_frame,frame_before,frame_after,mode,trigger,width,height,"
                     "selftest,selftest_attempt,selftest_equal,file,file_b\n";
      }
    }
    manifest_ << pending.requested << ',' << pending.frame_before << ',' << pending.frame_after
              << ',' << pending.mode << ',' << pending.trigger << ','
              << pending.image.width << ',' << pending.image.height << ','
              << (pending.selftest_run ? 1 : 0) << ',' << pending.selftest_attempt << ','
              << (pending.selftest_run && pending.selftest_equal ? 1 : 0) << ','
              << png.filename().string() << ',' << png_b.filename().string() << '\n';
    manifest_.flush();
  }

  rex::Runtime* runtime_ = nullptr;
  std::vector<uint64_t> schedule_;
  std::filesystem::path output_dir_;
  std::filesystem::path marks_dir_;
  uint32_t counter_address_ = 0;
  uint32_t clock_ticks_address_ = 0;
  uint32_t clock_scale_address_ = 0;
  uint32_t frame_delta_address_ = 0;
  uint32_t selftest_attempts_ = 3;
  bool selftest_ = false;
  bool binds_registered_ = false;

  // Toggle schedule: written once in Start(), then owned by the worker thread
  // (the only other writer is the worker's own not-mapped bail-out, which takes
  // the lock like the frame schedule's does).
  std::vector<double> toggle_schedule_;
  std::string toggle_cvar_ = kResidencyCvar;
  uint64_t toggle_lead_frames_ = 2;
  size_t toggle_next_ = 0;
  uint64_t toggle_target_ = 0;
  TogglePhase toggle_phase_ = TogglePhase::kWaitTime;
  bool toggle_clock_started_ = false;
  std::chrono::steady_clock::time_point toggle_t0_;

  std::once_flag worker_once_;
  std::thread worker_;
  std::thread writer_;
  std::mutex mutex_;
  std::condition_variable wake_;
  std::condition_variable write_wake_;
  std::deque<Pending> queue_;
  uint64_t manual_requests_ = 0;
  std::deque<NamedRequest> named_requests_;
  bool stopping_ = false;
  std::ofstream manifest_;
  std::ofstream marks_manifest_;
};

}  // namespace parity_capture
