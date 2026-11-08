// In-process .tar.xz unpacking for the bundled components: XZ Embedded (LZMA2 + x86 BCJ) feeding a small tar
// reader (ustar, pax and GNU long names; links are refused). Nothing from the system is needed, so it works the
// same on every Windows build (Windows' own tar.exe only reads .xz on recent builds).
// SPDX-License-Identifier: MIT
#pragma once
#include <atomic>
#include <cstdint>
#include <string>

namespace setup {

// Unpacks the .tar.xz stored at [offset, offset + size) of `src` into `dest` (created if needed). `done` advances
// by the compressed bytes consumed. Paths that would leave `dest` are refused. On failure `err` says why.
bool UnpackTarXz(const std::wstring& src, uint64_t offset, uint64_t size, const std::wstring& dest, std::atomic<uint64_t>& done,
                 const std::atomic<bool>& cancel, std::string& err);

}  // namespace setup
