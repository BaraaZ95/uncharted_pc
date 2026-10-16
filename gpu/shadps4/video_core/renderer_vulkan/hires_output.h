// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: pure decisions of the hi-res output mode (Uncharted, BB_HIRES_OUTPUT, docs/upscaler.md):
// the scene stays 1920x1080, DLSS / FSR 3.1 reconstruct it at the output size and the game's
// post-processing and HUD are drawn there. No Vulkan here, so tests/test_hires_output.cpp runs
// without a device.
#pragma once

#include <cstdint>

namespace Vulkan::HiResOutput {

struct Size {
    uint32_t width = 0, height = 0;
    bool operator==(const Size&) const = default;
};

/// "WxH" (BB_HIRES_OUTPUT): 2560x1440 or 3840x2160; with `any` (BB_HIRES_ANY=1) any even size
/// from 1921x1081 to 3840x2160. Everything else, trailing text included, is rejected.
inline bool Parse(const char* text, Size& out, bool any) {
    if (!text) {
        return false;
    }
    const auto number = [&text](uint32_t& value) {
        uint32_t digits = 0;
        value = 0;
        while (*text >= '0' && *text <= '9' && digits < 6) {
            value = value * 10 + uint32_t(*text - '0');
            ++text;
            ++digits;
        }
        return digits > 0 && digits < 6;
    };
    uint32_t width = 0, height = 0;
    if (!number(width) || *text++ != 'x' || !number(height) || *text != '\0') {
        return false;
    }
    const bool preset = (width == 2560 && height == 1440) || (width == 3840 && height == 2160);
    const bool free_size = any && width > 1920 && width <= 3840 && height > 1080 &&
                           height <= 2160 && width % 2 == 0 && height % 2 == 0;
    if (!preset && !free_size) {
        return false;
    }
    out = {width, height};
    return true;
}

/// FSR 4 preset (0 Native AA .. 4 Ultra Performance) for an output / render size ratio: the
/// thresholds of Dlss::QualityForScale (vk_dlss.cpp).
constexpr int PresetForScale(float scale) {
    return scale >= 2.9f ? 4 : scale >= 1.95f ? 3 : scale >= 1.65f ? 2 : scale > 1.01f ? 1 : 0;
}

/// Why the mode is (not) running; the order is the order of the checks in Decide.
enum class Reason {
    None,
    NotRequested,
    NotVelocityMode,
    FixedRender,
    Provider,
    Inactive,
    Toggle,
    Memory,
    Fsr4Assets,
    ProviderFailed,
    FrameGen,
    Chain,
};

inline const char* Describe(Reason reason) {
    switch (reason) {
    case Reason::None:
        return "running";
    case Reason::NotRequested:
        return "not requested";
    case Reason::NotVelocityMode:
        return "the game has no velocity pass (Uncharted only)";
    case Reason::FixedRender:
        return "BB_RENDER_RES fixes the render size";
    case Reason::Provider:
        return "anti-aliasing is not DLSS or FSR 3.1";
    case Reason::Inactive:
        return "the upscaler is off";
    case Reason::Toggle:
        return "switched off by the toggle file";
    case Reason::Memory:
        return "not enough free video memory";
    case Reason::Fsr4Assets:
        return "FSR 4 assets for this output size are missing";
    case Reason::ProviderFailed:
        return "the upscaler failed at the output size";
    case Reason::FrameGen:
        return "frame generation is on (not supported with hi-res output yet)";
    case Reason::Chain:
        return "the game's post-processing is not drawn at the output size";
    }
    return "unknown";
}

/// What Decide looks at; every field is true when that condition holds.
struct Inputs {
    bool requested = false;        ///< BB_HIRES_OUTPUT is set and valid
    bool velocity_mode = false;    ///< the game has a velocity pass (Uncharted)
    bool fixed_render = false;     ///< BB_RENDER_RES startup patch
    bool provider_supported = false; ///< DLSS, or FSR 3.1 (FSR 4 later)
    bool active = false;           ///< the upscaler is on and has not failed
    bool toggle_off = false;       ///< BbToggle::HiResOutput set
    bool memory_ok = false;        ///< enough free video memory
    bool fsr4_assets_ok = true;    ///< FSR 4 model for the output size exists (FSR 4 only)
    bool provider_failed = false;  ///< the provider failed at the output size
    bool frame_gen_wanted = false; ///< frame generation is on
    bool chain_failed = false;     ///< the post chain never reached the display at output size
};

constexpr Reason Decide(const Inputs& in) {
    if (!in.requested) return Reason::NotRequested;
    if (!in.velocity_mode) return Reason::NotVelocityMode;
    if (in.fixed_render) return Reason::FixedRender;
    if (!in.provider_supported) return Reason::Provider;
    if (!in.active) return Reason::Inactive;
    if (in.toggle_off) return Reason::Toggle;
    if (!in.memory_ok) return Reason::Memory;
    if (!in.fsr4_assets_ok) return Reason::Fsr4Assets;
    if (in.provider_failed) return Reason::ProviderFailed;
    if (in.frame_gen_wanted) return Reason::FrameGen;
    if (in.chain_failed) return Reason::Chain;
    return Reason::None;
}

/// The display pass may be redirected to the output-size image when the HUD-less pass read
/// enlarged images and no native 1080p color image (else the picture would be 1080p content
/// stretched, and the frame is shown as today). `check` false: always.
constexpr bool DisplayInputsOk(uint32_t proxies, uint32_t native_fullres, bool check) {
    return !check || (proxies > 0 && native_fullres == 0);
}

/// Counts consecutive scene frames whose display pass could not be redirected; one success
/// resets. A long run means the game's chain never works at the output size: the session falls
/// back to 1080p instead of stretching every frame.
class ChainMonitor {
public:
    static constexpr uint32_t Limit = 120;
    void Frame(bool ok) {
        if (ok) {
            failures = 0;
        } else if (failures < Limit) {
            ++failures;
        }
    }
    [[nodiscard]] bool Failed() const {
        return failures >= Limit;
    }
    void Reset() {
        failures = 0;
    }

private:
    uint32_t failures = 0;
};

/// Video memory the mode adds at `out`: output image, enlarged post proxies (about eight RGBA16F
/// images), the display image and the frame generation inputs, plus the provider's own.
constexpr uint64_t EstimateBytes(Size out) {
    const uint64_t pixels = uint64_t(out.width) * out.height;
    const uint64_t provider = pixels > 2560ull * 1440 ? 400ull << 20 : 256ull << 20;
    return pixels * (8 + 8 * 8 + 4 + 8) + provider;
}

} // namespace Vulkan::HiResOutput
