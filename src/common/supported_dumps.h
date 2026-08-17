// The supported-dump table.
//
// This started as a single hash compiled into the gate. It is a table because the
// owner intends to add other regional releases later, and sourcing them legally
// takes time - so the code that will accept them should already exist, and
// adding a row should be a data edit reviewed against a dump someone actually
// holds rather than a change to the gate.
//
// Rules that do not change with the table:
//   - The XEX sha256 is the only thing that decides EXACT.
//   - title_id alone identifies the near-miss class ("right game, wrong build").
//   - Region is NOT a discriminator: this dump reports region_raw 0xFFFFFFFF.
//   - There is no override. An unknown hash is a refusal, not a prompt.
//
// This header lives in recomp/common/ rather than beside either consumer
// because there are two gates and they must never drift: `thps_p8_identify`
// guards the launcher's first-run path, and the game's own OnConfigurePaths
// gate guards every other way in - a bare
// `thps_p8`, a `thps_p8_launch` passthrough. A second copy of this table would
// mean only one of them was ever tested.

#pragma once

#include <cstdint>
#include <string_view>

namespace thps::identify {

struct SupportedDump {
  std::string_view sha256;       // of default.xex, lowercase hex
  std::string_view title_id;     // as printed by xex-tools xex-info
  std::string_view release;      // shown to a player, in disc vocabulary
  uint64_t xex_size;             // bytes; sharpens the "truncated" message only
};

// v0.1.0 ships exactly one row, and the launcher says so before the file picker.
inline constexpr SupportedDump kSupportedDumps[] = {
    {"cfc732340e55defda400e25f03231aa9bb65fd9545b618212f69a4952384a5dd", "0x415607DD",
     "Tony Hawk's Project 8, 2006 retail disc", 8237056},
};

// Every title id in the table above. A disc whose title matches one of these but
// whose hash matches none is the near-miss: same game, different release.
inline constexpr std::string_view kSupportedTitleIds[] = {"0x415607DD"};

// The releases known to exist, so that "we do not support this" can eventually
// become "we know exactly what this is and do not support it yet". None of the
// rows below are in the accepted table: a release is only added once its
// executable hash has been confirmed against a disc someone actually holds.
//
//   World v1.0   USA/Europe   title 415607DD  media 2CB96AE4  region-free
//                             ^ the supported one, and the only one hashed here
//   Japan v1.0   NTSC-J       title 41560810  media 05728741
//                             ^ a different title id, so a different executable
//   Korea        unknown      a physical release is confirmed to exist; its
//                             disc identity is unresolved and appears to be
//                             unpublished anywhere
//   Demo v1.0    disc label AV202950W0X11, media 2932D558
//
// The Japanese title id is worth having written down even though nothing reads
// it yet: it is the evidence that NTSC-J is a separate build rather than a
// regional variant of the same executable, which is what a size comparison
// alone could never establish.

}  // namespace thps::identify
