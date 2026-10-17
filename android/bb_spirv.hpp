// Bloodframe: the SPIR-V shader sets of the player's Lossless.dll.
//
// A recent Lossless.dll carries every shader of frame generation three times: as DXBC (resource id N,
// the set the framegen can translate to SPIR-V), as SPIR-V for half-precision arithmetic (N + 49) and as
// SPIR-V for full precision (N + 98). This reads one of the two SPIR-V sets, checks it needs nothing the
// framegen's device does not enable, and hands each module over ready to use. Nothing of the dll is
// kept on disk or shipped. Both sets can be prepared side by side.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace BbSpirv {

enum class Set { Half, Full };

/// "FP16" / "FP32".
const char* Name(Set set);

/// Reads the SPIR-V of `set` for every shader the framegen uses (both models) from the loaded
/// Lossless.dll (Extract::extractShaders) and checks it. Returns an empty string when the set is
/// complete and usable, else why it is not (an old Lossless.dll, a module that needs more than the
/// device features Bloodframe enables; the full-precision set must not need Float16).
std::string Prepare(Set set);

/// The prepared SPIR-V of a shader (the framegen's own name for it, e.g. "alpha[0]", "p_generate").
/// Throws std::runtime_error when Prepare(set) did not succeed or the name is unknown.
std::vector<uint8_t> Get(Set set, const std::string& name);

/// The SPIR-V capabilities the prepared set uses, e.g. "Float16 ImageQuery Shader ..." (for the log).
std::string Capabilities(Set set);

} // namespace BbSpirv
