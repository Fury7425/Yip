#pragma once

// Resolves design tokens out of App.xaml's theme dictionaries at runtime.
//
// The point is that no colour is written in C++. `Application.Current.Resources`
// resolves a theme-dictionary key against the app's current theme, so light and
// dark each get their own value from one place, and a token edit in App.xaml
// reaches the compositor without a code change.

#include <cstdint>
#include <string_view>
#include <vector>

#include <winrt/Microsoft.UI.Xaml.Media.h>
#include <winrt/Windows.UI.h>

namespace yip::theme {

// The brush behind `key`, or nullptr when the key is missing — a typo degrades
// to "unpainted" rather than taking the window down.
winrt::Microsoft::UI::Xaml::Media::Brush Brush(std::wstring_view key);

// The colour behind a SolidColorBrush token. Composition wants a Color rather
// than a Brush; this is how it borrows one without owning the value. A token
// that carries its translucency as brush Opacity has it folded into the alpha,
// so the compositor sees what XAML would have drawn.
winrt::Windows::UI::Color Color(std::wstring_view key, winrt::Windows::UI::Color fallback);

// `count` evenly spaced samples of the YipMeterGradientBrush ramp, so the
// waveform and the XAML meter cannot drift apart. Empty when the token is
// missing or is not a gradient — callers fall back to a flat rest colour
// rather than carrying a second copy of the ramp.
std::vector<winrt::Windows::UI::Color> SampleMeterRamp(uint32_t count);

} // namespace yip::theme
