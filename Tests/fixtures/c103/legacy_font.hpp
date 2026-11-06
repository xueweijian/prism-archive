// C103 negative-control fixture: this file deliberately violates the card's
// rule (a hardcoded "Segoe UI" outside Typography.hpp). It lives under Tests/,
// never under Src/, and is only scanned by the mutation arm of
// core.c103_font_literals -- proof that the zero-hit assertion can go red.
#pragma once

inline const wchar_t* LegacyFontFamily() { return L"Segoe UI"; }
