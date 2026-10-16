// SPDX-License-Identifier: GPL-2.0-or-later
#include <cassert>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include "gpu/shadps4/video_core/renderer_vulkan/hires_output.h"

int main() {
    using namespace Vulkan::HiResOutput;

    // BB_HIRES_OUTPUT: the two presets only, free sizes need BB_HIRES_ANY.
    Size size{};
    assert((Parse("2560x1440", size, false) && size == Size{2560, 1440}));
    assert((Parse("3840x2160", size, false) && size == Size{3840, 2160}));
    size = {};
    assert(!Parse("1920x1080", size, false) && !Parse("1920x1080", size, true));
    assert(!Parse("abc", size, false) && !Parse("", size, true) && !Parse(nullptr, size, true));
    assert(!Parse("4000x3000", size, true) && !Parse("3840x2161", size, true));
    assert(!Parse("2560x1440x", size, false) && !Parse("2560x1440 ", size, false));
    assert(!Parse("2560X1440", size, false) && !Parse("-2560x1440", size, true));
    assert(!Parse("2560x", size, true) && !Parse("x1440", size, true));
    assert(!Parse("2000x1200", size, false));
    assert(size == Size{});
    assert((Parse("2000x1200", size, true) && size == Size{2000, 1200}));
    assert(!Parse("2001x1200", size, true) && !Parse("2000x1201", size, true));
    assert(!Parse("1920x1200", size, true) && !Parse("2000x1080", size, true));
    assert((Parse("3840x2160", size, true) && size == Size{3840, 2160}));

    // FSR 4 / DLSS quality for the scale of a 1080p scene.
    static_assert(PresetForScale(2560 / 1920.0f) == 1);
    static_assert(PresetForScale(3840 / 1920.0f) == 3);
    static_assert(PresetForScale(1.0f) == 0);
    static_assert(PresetForScale(3.0f) == 4 && PresetForScale(1.7f) == 2);

    // Decide: the first failing condition, in the order of the enum.
    Inputs in{};
    in.fsr4_assets_ok = true;
    assert(Decide(in) == Reason::NotRequested);
    in.requested = true;
    assert(Decide(in) == Reason::NotVelocityMode);
    in.velocity_mode = true;
    in.fixed_render = true;
    assert(Decide(in) == Reason::FixedRender);
    in.fixed_render = false;
    assert(Decide(in) == Reason::Provider);
    in.provider_supported = true;
    assert(Decide(in) == Reason::Inactive);
    in.active = true;
    in.memory_ok = false;
    in.toggle_off = true;
    assert(Decide(in) == Reason::Toggle);
    in.toggle_off = false;
    assert(Decide(in) == Reason::Memory);
    in.memory_ok = true;
    in.fsr4_assets_ok = false;
    in.provider_failed = true;
    assert(Decide(in) == Reason::Fsr4Assets);
    in.fsr4_assets_ok = true;
    assert(Decide(in) == Reason::ProviderFailed);
    in.provider_failed = false;
    in.frame_gen_wanted = true;
    in.chain_failed = true;
    assert(Decide(in) == Reason::FrameGen);
    in.frame_gen_wanted = false;
    assert(Decide(in) == Reason::Chain);
    in.chain_failed = false;
    assert(Decide(in) == Reason::None);
    for (Reason reason : {Reason::None, Reason::NotRequested, Reason::NotVelocityMode,
                          Reason::FixedRender, Reason::Provider, Reason::Inactive, Reason::Toggle,
                          Reason::Memory, Reason::Fsr4Assets, Reason::ProviderFailed,
                          Reason::FrameGen, Reason::Chain}) {
        assert(std::strlen(Describe(reason)) > 0 && std::strcmp(Describe(reason), "unknown") != 0);
    }

    // The display pass needs enlarged inputs and no native 1080p color input.
    static_assert(DisplayInputsOk(3, 0, true));
    static_assert(!DisplayInputsOk(0, 0, true));
    static_assert(!DisplayInputsOk(3, 1, true));
    static_assert(!DisplayInputsOk(0, 2, true));
    static_assert(DisplayInputsOk(0, 2, false) && DisplayInputsOk(3, 1, false));

    // Chain monitor: 120 consecutive failed scene frames trip it, one success resets.
    ChainMonitor chain;
    for (unsigned i = 0; i < ChainMonitor::Limit - 1; ++i) {
        chain.Frame(false);
        assert(!chain.Failed());
    }
    chain.Frame(true);
    for (unsigned i = 0; i < ChainMonitor::Limit - 1; ++i) {
        chain.Frame(false);
    }
    assert(!chain.Failed());
    chain.Frame(false);
    assert(chain.Failed());
    chain.Frame(false);
    assert(chain.Failed());
    chain.Frame(true);
    assert(!chain.Failed());
    chain.Frame(false);
    chain.Reset();
    for (unsigned i = 0; i < ChainMonitor::Limit; ++i) {
        chain.Frame(false);
    }
    assert(chain.Failed());
    chain.Reset();
    assert(!chain.Failed());

    // Memory estimate grows with the output size.
    static_assert(EstimateBytes({2560, 1440}) < EstimateBytes({3840, 2160}));
    static_assert(EstimateBytes({2560, 1440}) < EstimateBytes({2562, 1440}));
    static_assert(EstimateBytes({2560, 1440}) > 256ull << 20);
    static_assert(EstimateBytes({3840, 2160}) > 400ull << 20);

    std::puts("Hi-res output: PASS (parse, decide, display inputs, chain monitor, memory estimate)");
}
