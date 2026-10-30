// PrismArchive -- PrismWin
// Windows implementation of the PrismCore charset transcoder interface:
// legacy codepage bytes -> UTF-8 through Win32 NLS (no ICU dependency).
#pragma once

#include <windows.h>

#include <optional>
#include <string>
#include <string_view>

#include "../PrismCore/CharsetRepair.hpp"
#include "PrismEngineBridge.hpp"

namespace prism::win {

class Win32Transcoder final : public prism::ITranscoder {
 public:
  std::optional<std::string> ToUtf8(Encoding from, std::string_view raw) override {
    if (from == Encoding::Utf8) return std::string(raw);
    const UINT cp = CodepageOf(from);
    if (cp == 0) return std::nullopt;

    // MB_ERR_INVALID_CHARS: a wrong codepage must fail loudly instead of
    // silently substituting U+FFFD and destroying the filename.
    const int wideLen = MultiByteToWideChar(cp, MB_ERR_INVALID_CHARS, raw.data(),
                                            static_cast<int>(raw.size()), nullptr, 0);
    if (wideLen <= 0) return std::nullopt;
    std::wstring wide(static_cast<size_t>(wideLen), L'\0');
    if (MultiByteToWideChar(cp, MB_ERR_INVALID_CHARS, raw.data(),
                            static_cast<int>(raw.size()), wide.data(), wideLen) <= 0) {
      return std::nullopt;
    }
    return WideToUtf8(wide);
  }

  const char* BackendName() const override { return "win32-nls"; }

  static UINT CodepageOf(Encoding enc) {
    switch (enc) {
      case Encoding::Utf8: return CP_UTF8;
      case Encoding::Gb18030: return 54936;  // CP936 fallback below
      case Encoding::Big5: return 950;
      case Encoding::ShiftJis: return 932;
      case Encoding::EucKr: return 949;
      case Encoding::Latin1: return 28591;
      default: return 0;
    }
  }
};

}  // namespace prism::win
