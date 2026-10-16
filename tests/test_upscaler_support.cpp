// SPDX-License-Identifier: GPL-2.0-or-later
// Saved FSR 4 choices must be safe before the first rendered frame on older GPUs.
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <unistd.h>
#include "bbport_settings.h"

int main() {
    using namespace BbSettings;
    char path[] = "/tmp/bbport-upscaler-test-XXXXXX";
    const int fd = mkstemp(path);
    assert(fd >= 0);
    close(fd);
    setenv("BB_CONFIG", path, 1);
    unsetenv("BB_UPSCALER");
    unsetenv("BB_UPSCALE_PRESET");
    unsetenv("BB_RENDER_RES");
    auto& s = Get();
    s.upscaler = UpscalerTaa;
    s.preset = Performance;
    assert(RenderPreset() == NativeAA && s.preset == Performance);
    Save();
    s.upscaler = UpscalerOff;
    Load();
    assert(s.upscaler == UpscalerTaa && s.preset == Performance);
    assert(RenderPreset() == NativeAA && !ResolutionNeedsRestart());
    s.upscaler = UpscalerFsr3;
    assert(RenderPreset() == Performance);
    for (bool fsr4 : {false, true}) {
        for (bool fsr411 : {false, true}) {
            for (int requested = 0; requested < UpscalerCount; ++requested) {
                FILE* config = std::fopen(path, "w");
                assert(config);
                std::fprintf(config, "upscaler=%s\npreset=3\noutput_res=2560x1440\n",
                             UpscalerName(requested));
                std::fclose(config);
                s.fsr4_problem = nullptr;
                Load();
                assert(s.upscaler == requested);
                ConfigureUpscalerSupport(fsr4, fsr411);
                const bool unsupported = (requested == UpscalerFsr4 && !fsr4) ||
                    (requested == UpscalerFsr411 && !(fsr4 && fsr411));
                assert(s.upscaler == (unsupported ? UpscalerFsr3 : requested));
                assert(s.fsr4_supported == fsr4);
                assert(s.fsr411_supported == (fsr4 && fsr411));
                assert(bool(s.fsr4_problem.load()) == unsupported);
                assert(s.preset == Performance && s.output_res == 2);
                // Startup patch settings still describe the already applied guest patches.
                assert(s.startup_upscaler == requested);
                ConfigureUpscalerSupport(fsr4, fsr411);
                assert(s.upscaler == (unsupported ? UpscalerFsr3 : requested));
            }
        }
    }
    s.startup_preset = Quality;
    s.startup_upscaler = UpscalerFsr3;
    s.upscaler = UpscalerFsr3;
    s.preset = Performance;
    assert(RenderPreset() == Performance && !ResolutionNeedsRestart());
    setenv("BB_RENDER_RES", "1706x960", 1);
    assert(RenderPreset() == Quality && ResolutionNeedsRestart());
    s.preset = Quality;
    assert(!ResolutionNeedsRestart());
    s.upscaler = UpscalerOff;
    assert(ResolutionNeedsRestart());
    s.upscaler = UpscalerFsr4;
    assert(!ResolutionNeedsRestart()); // provider change at the same patched size is live
    s.upscaler = UpscalerTaa;          // TAA needs native guest targets
    assert(ResolutionNeedsRestart());
    s.upscaler = UpscalerFsr4;
    const int output = s.startup_output_res;
    s.output_res = (output + 1) % OutputCount;
    assert(ResolutionNeedsRestart());
    s.output_res = output;
    setenv("BB_RENDER_RES", "", 1);
    assert(!FixedRenderSession() && !ResolutionNeedsRestart());
    unsetenv("BB_RENDER_RES");
    // Hi-res output (BB_HIRES_OUTPUT) comes from the environment only and is never saved.
    unsetenv("BB_HIRES_OUTPUT");
    unsetenv("BB_HIRES_ANY");
    {
        FILE* config = std::fopen(path, "w");
        assert(config);
        std::fputs("hires_width=2560\nhires_height=1440\nhires_output=2560x1440\n", config);
        std::fclose(config);
    }
    Load();
    assert(s.hires_width == 0 && s.hires_height == 0);
    setenv("BB_HIRES_OUTPUT", "3840x2160", 1);
    Load();
    assert(s.hires_width == 3840 && s.hires_height == 2160);
    setenv("BB_HIRES_OUTPUT", "1920x1080", 1);
    Load();
    assert(s.hires_width == 0 && s.hires_height == 0);
    setenv("BB_HIRES_OUTPUT", "2000x1200", 1);
    Load();
    assert(s.hires_width == 0);
    setenv("BB_HIRES_ANY", "1", 1);
    Load();
    assert(s.hires_width == 2000 && s.hires_height == 1200);
    unsetenv("BB_HIRES_ANY");
    setenv("BB_HIRES_OUTPUT", "2560x1440", 1);
    Load();
    assert(s.hires_width == 2560 && s.hires_height == 1440);
    Save();
    {
        FILE* config = std::fopen(path, "r");
        assert(config);
        char text[4096];
        const size_t length = std::fread(text, 1, sizeof(text) - 1, config);
        text[length] = '\0';
        std::fclose(config);
        assert(length > 0 && !std::strstr(text, "hires") && !std::strstr(text, "HIRES"));
    }
    unsetenv("BB_HIRES_OUTPUT");
    Load();
    assert(s.hires_width == 0 && s.hires_height == 0);
    std::remove(path);
    std::puts("Upscaler support: saved choices and FSR 3.1 fallback PASS");
}
