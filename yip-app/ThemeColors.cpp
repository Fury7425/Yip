#include "pch.h"
#include "ThemeColors.h"

#include <winrt/Microsoft.UI.Xaml.h>

#include <algorithm>
#include <cmath>

namespace mux = winrt::Microsoft::UI::Xaml;
namespace muxm = winrt::Microsoft::UI::Xaml::Media;

namespace {

// ResourceDictionary::Lookup throws on a missing key, and whether HasKey even
// sees inside ThemeDictionaries is not worth relying on — catch instead.
winrt::Windows::Foundation::IInspectable LookupRaw(std::wstring_view key) noexcept
{
    try {
        auto app = mux::Application::Current();
        if (!app) return nullptr;
        auto resources = app.Resources();
        if (!resources) return nullptr;
        return resources.Lookup(winrt::box_value(winrt::hstring{key}));
    } catch (...) {
        return nullptr;
    }
}

uint8_t Scale(uint8_t channel, double factor) noexcept
{
    const double v = std::lround(static_cast<double>(channel) * factor);
    return static_cast<uint8_t>(std::clamp(v, 0.0, 255.0));
}

winrt::Windows::UI::Color Lerp(winrt::Windows::UI::Color const& a, winrt::Windows::UI::Color const& b,
                               double t) noexcept
{
    const auto mix = [t](uint8_t x, uint8_t y) {
        return static_cast<uint8_t>(std::lround(x + (static_cast<double>(y) - x) * t));
    };
    return {mix(a.A, b.A), mix(a.R, b.R), mix(a.G, b.G), mix(a.B, b.B)};
}

} // namespace

namespace yip::theme {

muxm::Brush Brush(std::wstring_view key)
{
    auto value = LookupRaw(key);
    return value ? value.try_as<muxm::Brush>() : nullptr;
}

winrt::Windows::UI::Color Color(std::wstring_view key, winrt::Windows::UI::Color fallback)
{
    auto value = LookupRaw(key);
    if (!value) return fallback;

    if (auto solid = value.try_as<muxm::SolidColorBrush>()) {
        auto color = solid.Color();
        const double opacity = solid.Opacity();
        if (opacity < 1.0) color.A = Scale(color.A, opacity);
        return color;
    }
    if (auto boxed = value.try_as<winrt::Windows::Foundation::IReference<winrt::Windows::UI::Color>>()) {
        return boxed.Value();
    }
    return fallback;
}

std::vector<winrt::Windows::UI::Color> SampleMeterRamp(uint32_t count)
{
    std::vector<winrt::Windows::UI::Color> out;
    if (count == 0) return out;

    auto value = LookupRaw(L"YipMeterGradientBrush");
    if (!value) return out;
    auto gradient = value.try_as<muxm::LinearGradientBrush>();
    if (!gradient) return out;

    struct Stop {
        double offset;
        winrt::Windows::UI::Color color;
    };
    std::vector<Stop> stops;
    for (auto const& s : gradient.GradientStops()) {
        stops.push_back({s.Offset(), s.Color()});
    }
    if (stops.empty()) return out;
    std::sort(stops.begin(), stops.end(), [](Stop const& a, Stop const& b) { return a.offset < b.offset; });

    out.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        const double t = (count == 1) ? 0.0 : static_cast<double>(i) / (count - 1);
        if (t <= stops.front().offset) {
            out.push_back(stops.front().color);
            continue;
        }
        if (t >= stops.back().offset) {
            out.push_back(stops.back().color);
            continue;
        }
        for (size_t s = 1; s < stops.size(); ++s) {
            if (t <= stops[s].offset) {
                const double span = stops[s].offset - stops[s - 1].offset;
                const double f = (span <= 0.0) ? 0.0 : (t - stops[s - 1].offset) / span;
                out.push_back(Lerp(stops[s - 1].color, stops[s].color, f));
                break;
            }
        }
    }
    return out;
}

} // namespace yip::theme
