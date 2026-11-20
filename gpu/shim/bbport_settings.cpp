// SPDX-License-Identifier: GPL-2.0-or-later
#include "bbport_settings.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
#ifdef _WIN32
#include <windows.h>
#include <dxgi1_6.h>
#endif

namespace BbSettings {

namespace {

const char* Path() {
    const char* env = std::getenv("BB_CONFIG");
    return env && env[0] ? env : "bbport.ini";
}

float Clamp(float v, float lo, float hi) {
    return std::clamp(v, lo, hi);
}

// hdr_peak_nits=auto: the peak brightness Windows reports for the brightest display in HDR mode
// (DXGI output MaxLuminance, from the display's EDID or its Windows HDR calibration); 0 when there
// is none.
float DisplayPeakNits() {
#ifdef _WIN32
    using Create = HRESULT(WINAPI*)(REFIID, void**);
    HMODULE dxgi = LoadLibraryA("dxgi.dll");
    auto create = dxgi ? reinterpret_cast<Create>(GetProcAddress(dxgi, "CreateDXGIFactory1")) : nullptr;
    IDXGIFactory1* factory = nullptr;
    if (!create || FAILED(create(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory)))) {
        return 0.0f;
    }
    float best = 0.0f;
    IDXGIAdapter1* adapter = nullptr;
    for (UINT a = 0; factory->EnumAdapters1(a, &adapter) != DXGI_ERROR_NOT_FOUND; ++a) {
        IDXGIOutput* output = nullptr;
        for (UINT o = 0; adapter->EnumOutputs(o, &output) != DXGI_ERROR_NOT_FOUND; ++o) {
            IDXGIOutput6* output6 = nullptr;
            if (SUCCEEDED(output->QueryInterface(__uuidof(IDXGIOutput6),
                                                 reinterpret_cast<void**>(&output6)))) {
                DXGI_OUTPUT_DESC1 desc{};
                if (SUCCEEDED(output6->GetDesc1(&desc)) &&
                    desc.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020) {
                    best = std::max(best, desc.MaxLuminance);
                }
                output6->Release();
            }
            output->Release();
        }
        adapter->Release();
    }
    factory->Release();
    return best;
#else
    return 0.0f;
#endif
}

void Set(Values& v, const std::string& key, const std::string& value) {
    const float f = float(std::atof(value.c_str()));
    const int i = std::atoi(value.c_str());
    if (key == "upscaler") {
        for (int u = 0; u < UpscalerCount; ++u) {
            if (value == UpscalerName(u)) {
                v.upscaler = u;
            }
        }
    } else if (key == "frame_gen") {
        for (int g = 0; g < FrameGenCount; ++g) {
            if (value == FrameGenName(g)) {
                v.frame_gen = g;
            }
        }
    } else if (key == "frame_gen_multiplier") {
        v.frame_gen_multiplier = std::clamp(i, 2, 4);
    } else if (key == "preset") {
        v.preset = std::clamp(i, 0, PresetCount - 1);
    } else if (key == "sharpen") {
        v.sharpen = i != 0;
    } else if (key == "sharpness") {
        v.sharpness = Clamp(f, 0.0f, 2.0f);
    } else if (key == "jitter") {
        v.jitter = i != 0;
    } else if (key == "reactive") {
        v.reactive = i != 0;
    } else if (key == "object_motion") {
        v.object_motion = i != 0;
    } else if (key == "reactive_scale") {
        v.reactive_scale = Clamp(f, 0.0f, 16.0f);
    } else if (key == "reactive_threshold") {
        v.reactive_threshold = Clamp(f, 0.0f, 1.0f);
    } else if (key == "reactive_max") {
        v.reactive_max = Clamp(f, 0.0f, 1.0f);
    } else if (key == "debug_view") {
        v.debug_view = std::clamp(i, 0, DebugViewCount - 1);
    } else if (key == "show_fps") {
        v.show_fps = i != 0;
    } else if (key == "show_fps_graph") {
        v.show_fps_graph = i != 0;
    } else if (key == "fsr4_auto_exposure") {
        v.fsr4_auto_exposure = i != 0;
    } else if (key == "fsr4_invert_jitter") {
        v.fsr4_invert_jitter = i != 0;
    } else if (key == "model_lod") {
        v.model_lod = std::clamp(i, -2, 2);
    } else if (key == "live_resolution") {
        v.live_resolution = value == "auto" ? -1 : std::clamp(i, 0, 1);
    } else if (key == "hdr_paper_white") {
        v.hdr_paper_white = Clamp(f, 80.0f, 500.0f);
    } else if (key == "hdr_peak_nits") {
        if (value == "auto") {
            const float nits = DisplayPeakNits();
            std::printf("HDR: peak brightness auto: the display reports %.0f nits%s\n", nits,
                        nits > 0.0f ? "" : " (none in HDR mode: 1000)");
            v.hdr_peak_nits = nits > 0.0f ? Clamp(nits, 400.0f, 10000.0f) : 1000.0f;
        } else {
            v.hdr_peak_nits = Clamp(f, 400.0f, 10000.0f);
        }
    } else if (key == "hdr_exposure") {
        v.hdr_exposure = Clamp(f, 0.25f, 4.0f);
    } else if (key == "hdr_saturation") {
        v.hdr_saturation = Clamp(f, 0.0f, 2.0f);
    } else if (key == "hdr_highlight_saturation") {
        v.hdr_highlight_saturation = Clamp(f, 0.0f, 2.0f);
    } else if (key == "hdr_contrast") {
        v.hdr_contrast = Clamp(f, 0.5f, 2.0f);
    } else if (key == "hdr_chrominance") {
        v.hdr_chrominance = Clamp(f, 0.0f, 1.0f);
    } else if (key == "hdr_blowout") {
        v.hdr_blowout = Clamp(f, 0.0f, 1.0f);
    } else if (key == "hdr_hue_correction") {
        v.hdr_hue_correction = Clamp(f, 0.0f, 1.0f);
    } else if (key == "hdr_per_channel") {
        v.hdr_per_channel = i != 0;
    } else if (key == "hdr_highlight_start") {
        // The menu's range is 0.10-0.95; 0 (anything below 0.10) asks for the earlier ramp.
        v.hdr_highlight_start = f < 0.1f ? 0.0f : Clamp(f, 0.1f, 0.95f);
    } else if (key == "hdr_scene_mode") {
        v.hdr_scene_mode = i < 0 ? 0 : i > 2 ? 2 : i;
    } else if (key == "hdr_knee") {
        v.hdr_knee = Clamp(f, 0.2f, 0.9f);
    } else if (key == "hdr_strength") {
        v.hdr_strength = Clamp(f, 0.0f, 1.0f);
    } else if (key == "hdr_hud_vivid") {
        v.hdr_hud_vivid = i != 0;
    } else if (key == "hdr_split") {
        v.hdr_split = Clamp(f, 0.0f, 1.0f);
    } else if (key == "hdr_sdr_gamma") {
        v.hdr_sdr_gamma = Clamp(f, 1.8f, 2.8f);
    } else if (key == "hdr_sdr_saturation") {
        v.hdr_sdr_saturation = Clamp(f, 0.0f, 2.0f);
    } else if (key == "hdr_gamut_expansion") {
        v.hdr_gamut_expansion = Clamp(f, 0.0f, 2.0f);
    } else if (key == "output_res") {
        for (int r = 0; r < OutputCount; ++r) {
            if (value == std::to_string(OutputWidths[r]) + "x" + std::to_string(OutputHeights[r])) {
                v.output_res = r;
            }
        }
    } else {
        for (int e = 0; e < EffectCount; ++e) {
            if (key == Effects[e].key) {
                v.effects[e] = i != 0;
            }
        }
    }
}

} // namespace

Values& Get() {
    static Values values;
    return values;
}

void Load() {
    auto& v = Get();
    for (int e = 0; e < EffectCount; ++e) {
        v.effects[e] = Effects[e].default_on;
    }
    if (FILE* file = std::fopen(Path(), "r")) {
        char line[256];
        while (std::fgets(line, sizeof(line), file)) {
            std::string text{line};
            text.erase(text.find_last_not_of(" \t\r\n") + 1);
            const auto eq = text.find('=');
            if (text.empty() || text[0] == '#' || eq == std::string::npos) {
                continue;
            }
            Set(v, text.substr(0, eq), text.substr(eq + 1));
        }
        std::fclose(file);
        std::printf("Settings: %s\n", Path());
    }
    // Environment overrides (scripts, A/B tests).
    if (const char* env = std::getenv("BB_UPSCALER")) {
        v.upscaler = UpscalerOff;
        for (int u = 0; u < UpscalerCount; ++u) {
            if (std::strcmp(env, UpscalerName(u)) == 0) v.upscaler = u;
        }
    }
    const std::pair<const char*, const char*> env_keys[] = {
        {"BB_FSR_SHARPNESS", "sharpness"},        {"BB_JITTER", "jitter"},
        {"BB_REACTIVE", "reactive"},              {"BB_REACTIVE_SCALE", "reactive_scale"},
        {"BB_REACTIVE_THRESHOLD", "reactive_threshold"}, {"BB_REACTIVE_MAX", "reactive_max"},
        {"BB_UPSCALE_PRESET", "preset"},            {"BB_OBJECT_MOTION", "object_motion"},
        {"BB_FRAME_GEN", "frame_gen"},              {"BB_FRAME_GEN_MULTIPLIER", "frame_gen_multiplier"},
        {"BB_HDR_PAPER_WHITE", "hdr_paper_white"},  {"BB_HDR_PEAK_NITS", "hdr_peak_nits"},
        {"BB_HDR_EXPOSURE", "hdr_exposure"},        {"BB_HDR_SATURATION", "hdr_saturation"},
        {"BB_HDR_HIGHLIGHT_SATURATION", "hdr_highlight_saturation"},
        {"BB_HDR_CONTRAST", "hdr_contrast"},        {"BB_HDR_CHROMINANCE", "hdr_chrominance"},
        {"BB_HDR_BLOWOUT", "hdr_blowout"},          {"BB_HDR_HUE_CORRECTION", "hdr_hue_correction"},
        {"BB_HDR_PER_CHANNEL", "hdr_per_channel"},  {"BB_HDR_SDR_GAMMA", "hdr_sdr_gamma"},
        {"BB_HDR_SDR_SATURATION", "hdr_sdr_saturation"},
        {"BB_HDR_HIGHLIGHT_START", "hdr_highlight_start"},
        {"BB_HDR_GAMUT_EXPANSION", "hdr_gamut_expansion"},
        {"BB_HDR_SCENE_MODE", "hdr_scene_mode"},  {"BB_HDR_KNEE", "hdr_knee"},
        {"BB_HDR_SPLIT", "hdr_split"},
        {"BB_HDR_STRENGTH", "hdr_strength"},      {"BB_HDR_HUD_VIVID", "hdr_hud_vivid"},
    };
    for (const auto& [env, key] : env_keys) {
        if (const char* value = std::getenv(env)) {
            Set(v, key, value);
        }
    }
    v.startup_preset = v.preset;
    v.startup_upscaler = v.upscaler;
    v.startup_object_motion = v.object_motion;
    for (int e = 0; e < EffectCount; ++e) {
        v.startup_effects[e] = v.effects[e];
    }
    v.startup_model_lod = v.model_lod;
    v.startup_output_res = v.output_res;
    v.startup_live_resolution = v.live_resolution;
    v.startup_hdr_paper_white = v.hdr_paper_white;
    v.startup_hdr_peak_nits = v.hdr_peak_nits;
}

void ConfigureUpscalerSupport(bool fsr4, bool fsr411) {
    auto& v = Get();
    v.fsr4_supported = fsr4;
    v.fsr411_supported = fsr4 && fsr411;
    const int requested = v.upscaler;
    if ((requested == UpscalerFsr4 && !v.fsr4_supported) ||
        (requested == UpscalerFsr411 && !v.fsr411_supported)) {
        v.fsr4_problem = "GPU does not support the selected FSR 4 shaders; using FSR 3.1";
        std::printf("Upscaler: %s unsupported on this GPU; falling back to FSR 3.1 before the first frame\n",
                    UpscalerName(requested));
        v.upscaler = UpscalerFsr3;
    }
}

void ConfigureDlssSupport(bool available, const char* problem) {
    auto& v = Get();
    v.dlss_supported = available;
    static std::string kept;
    kept = problem ? problem : "";
    v.dlss_problem = available || kept.empty() ? nullptr : kept.c_str();
    if (v.upscaler == UpscalerDlss && !available) {
        std::printf("Upscaler: DLSS unavailable (%s); falling back to FSR 3.1\n",
                    kept.empty() ? "bbport_dlss.dll or nvngx_dlss.dll missing" : kept.c_str());
        v.upscaler = UpscalerFsr3;
    }
}

void ConfigureXessSupport(bool available, const char* problem) {
    auto& v = Get();
    v.xess_supported = available;
    static std::string kept;
    kept = problem ? problem : "";
    v.xess_problem = available || kept.empty() ? nullptr : kept.c_str();
    if (v.upscaler == UpscalerXess && !available) {
        std::printf("Upscaler: XeSS unavailable (%s); falling back to FSR 3.1\n",
                    kept.empty() ? "libxess.dll missing" : kept.c_str());
        v.upscaler = UpscalerFsr3;
    }
}

bool FixedRenderSession() {
    const char* size = std::getenv("BB_RENDER_RES");
    return size && size[0];
}

int RenderPreset() {
    const auto& v = Get();
    return FixedRenderSession() ? v.startup_preset :
        v.upscaler == UpscalerTaa ? NativeAA : v.preset.load();
}

bool ResolutionNeedsRestart() {
    const auto& v = Get();
    // TAA needs the live path (native guest targets): run.sh selects it on restart.
    return FixedRenderSession() &&
        (v.preset != v.startup_preset || v.output_res != v.startup_output_res ||
         (v.upscaler == UpscalerOff) != (v.startup_upscaler == UpscalerOff) ||
         (v.upscaler == UpscalerTaa) != (v.startup_upscaler == UpscalerTaa));
}

void Save() {
    const auto& v = Get();
    FILE* file = std::fopen(Path(), "w");
    if (!file) {
        std::printf("Settings: cannot write %s\n", Path());
        return;
    }
    std::fprintf(file,
                 "# bbport settings (in-game menu: Insert / L3+R3)\n"
                 "upscaler=%s\npreset=%d\nsharpen=%d\nsharpness=%.2f\njitter=%d\n"
                 "reactive=%d\nobject_motion=%d\nreactive_scale=%.2f\nreactive_threshold=%.2f\nreactive_max=%.2f\n"
                 "debug_view=%d\nshow_fps=%d\nshow_fps_graph=%d\nfsr4_auto_exposure=%d\nfsr4_invert_jitter=%d\n",
                 UpscalerName(v.upscaler), v.preset.load(), int(v.sharpen.load()),
                 v.sharpness.load(), int(v.jitter.load()), int(v.reactive.load()),
                 int(v.object_motion.load()),
                 v.reactive_scale.load(), v.reactive_threshold.load(), v.reactive_max.load(),
                 v.debug_view.load(), int(v.show_fps.load()), int(v.show_fps_graph.load()),
                 int(v.fsr4_auto_exposure.load()), int(v.fsr4_invert_jitter.load()));
    // Read by patches.py at start.
    for (int e = 0; e < EffectCount; ++e) {
        std::fprintf(file, "%s=%d\n", Effects[e].key, int(v.effects[e].load()));
    }
    std::fprintf(file, "model_lod=%d\noutput_res=%dx%d\n", v.model_lod.load(),
                 OutputWidths[v.output_res], OutputHeights[v.output_res]);
    std::fprintf(file, "frame_gen=%s\nframe_gen_multiplier=%d\n", FrameGenName(v.frame_gen),
                 v.frame_gen_multiplier.load());
    // Read by run.sh at start.
    std::fprintf(file, "live_resolution=%s\n", v.live_resolution < 0 ? "auto"
                                                  : v.live_resolution ? "1" : "0");
    std::fprintf(file,
                 "hdr_paper_white=%.0f\nhdr_peak_nits=%.0f\nhdr_exposure=%.2f\nhdr_saturation=%.2f\n"
                 "hdr_highlight_saturation=%.2f\nhdr_contrast=%.2f\nhdr_chrominance=%.2f\n"
                 "hdr_blowout=%.2f\nhdr_hue_correction=%.2f\nhdr_per_channel=%d\nhdr_sdr_gamma=%.2f\n"
                 "hdr_sdr_saturation=%.2f\nhdr_highlight_start=%.2f\nhdr_gamut_expansion=%.2f\n",
                 v.hdr_paper_white.load(), v.hdr_peak_nits.load(), v.hdr_exposure.load(),
                 v.hdr_saturation.load(), v.hdr_highlight_saturation.load(), v.hdr_contrast.load(),
                 v.hdr_chrominance.load(), v.hdr_blowout.load(), v.hdr_hue_correction.load(),
                 int(v.hdr_per_channel.load()), v.hdr_sdr_gamma.load(), v.hdr_sdr_saturation.load(),
                 v.hdr_highlight_start.load(), v.hdr_gamut_expansion.load());
    std::fprintf(file, "hdr_scene_mode=%d\nhdr_knee=%.2f\nhdr_strength=%.2f\nhdr_hud_vivid=%d\n",
                 v.hdr_scene_mode.load(), v.hdr_knee.load(), v.hdr_strength.load(),
                 int(v.hdr_hud_vivid.load()));
    std::fclose(file);
}

void ResetHdr() {
    auto& v = Get();
    const Values defaults;
    for (auto [field, value] : {std::pair{&v.hdr_paper_white, v.startup_hdr_paper_white},
                                {&v.hdr_peak_nits, v.startup_hdr_peak_nits},
                                {&v.hdr_exposure, defaults.hdr_exposure.load()},
                                {&v.hdr_saturation, defaults.hdr_saturation.load()},
                                {&v.hdr_highlight_saturation, defaults.hdr_highlight_saturation.load()},
                                {&v.hdr_contrast, defaults.hdr_contrast.load()},
                                {&v.hdr_chrominance, defaults.hdr_chrominance.load()},
                                {&v.hdr_blowout, defaults.hdr_blowout.load()},
                                {&v.hdr_hue_correction, defaults.hdr_hue_correction.load()},
                                {&v.hdr_highlight_start, defaults.hdr_highlight_start.load()},
                                {&v.hdr_sdr_gamma, defaults.hdr_sdr_gamma.load()},
                                {&v.hdr_sdr_saturation, defaults.hdr_sdr_saturation.load()},
                                {&v.hdr_gamut_expansion, defaults.hdr_gamut_expansion.load()},
                                {&v.hdr_knee, defaults.hdr_knee.load()},
                                {&v.hdr_strength, defaults.hdr_strength.load()},
                                {&v.hdr_split, defaults.hdr_split.load()}}) {
        field->store(value);
    }
    v.hdr_per_channel = defaults.hdr_per_channel.load();
    v.hdr_hud_vivid = defaults.hdr_hud_vivid.load();
}

bool SetHdrKey(const std::string& key, const std::string& value, std::string& applied) {
    auto& v = Get();
    applied.clear();
    char* end = nullptr;
    std::strtof(value.c_str(), &end);
    if (end == value.c_str()) {
        return key.rfind("hdr_", 0) == 0; // not a number: left as it is
    }
    if (key == "hdr_hud_vivid") {
        const bool before = v.hdr_hud_vivid.load();
        Set(v, key, value);
        if (v.hdr_hud_vivid.load() != before) {
            applied = key + "=" + (v.hdr_hud_vivid.load() ? "1" : "0");
        }
        return true;
    }
    if (key == "hdr_scene_mode") {
        const int before = v.hdr_scene_mode.load();
        Set(v, key, value);
        if (v.hdr_scene_mode.load() != before) {
            applied = key + "=" + std::to_string(v.hdr_scene_mode.load());
        }
        return true;
    }
    if (key == "hdr_per_channel") {
        const bool before = v.hdr_per_channel.load();
        Set(v, key, value);
        if (v.hdr_per_channel.load() != before) {
            applied = key + "=" + (v.hdr_per_channel.load() ? "1" : "0");
        }
        return true;
    }
    const std::pair<const char*, std::atomic<float>*> keys[] = {
        {"hdr_paper_white", &v.hdr_paper_white},
        {"hdr_peak_nits", &v.hdr_peak_nits},
        {"hdr_exposure", &v.hdr_exposure},
        {"hdr_saturation", &v.hdr_saturation},
        {"hdr_highlight_saturation", &v.hdr_highlight_saturation},
        {"hdr_contrast", &v.hdr_contrast},
        {"hdr_chrominance", &v.hdr_chrominance},
        {"hdr_blowout", &v.hdr_blowout},
        {"hdr_hue_correction", &v.hdr_hue_correction},
        {"hdr_sdr_gamma", &v.hdr_sdr_gamma},
        {"hdr_sdr_saturation", &v.hdr_sdr_saturation},
        {"hdr_highlight_start", &v.hdr_highlight_start},
        {"hdr_gamut_expansion", &v.hdr_gamut_expansion},
        {"hdr_knee", &v.hdr_knee},
        {"hdr_strength", &v.hdr_strength},
        {"hdr_split", &v.hdr_split},
    };
    for (const auto& [name, field] : keys) {
        if (key != name) {
            continue;
        }
        const float before = field->load();
        Set(v, key, value);
        const float after = field->load();
        if (after != before) {
            char text[96];
            const bool nits = key == "hdr_paper_white" || key == "hdr_peak_nits";
            std::snprintf(text, sizeof(text), nits ? "%s=%.0f" : "%s=%.2f", name, after);
            applied = text;
        }
        return true;
    }
    return false;
}

float PresetScale(int preset) {
    static constexpr float scales[PresetCount] = {1.0f, 1.5f, 1.7f, 2.0f, 3.0f};
    return scales[std::clamp(preset, 0, PresetCount - 1)];
}

const char* PresetName(int preset) {
    static constexpr const char* names[PresetCount] = {"Native AA", "Quality", "Balanced",
                                                       "Performance", "Ultra Performance"};
    return names[std::clamp(preset, 0, PresetCount - 1)];
}

const char* UpscalerName(int upscaler) {
    static constexpr const char* names[UpscalerCount] = {"off", "fsr3", "fsr4", "fsr411", "taa", "dlss", "xess"};
    return names[std::clamp(upscaler, 0, UpscalerCount - 1)];
}

const char* FrameGenName(int frame_gen) {
    static constexpr const char* names[FrameGenCount] = {"off", "fsr3", "dlss"};
    return names[std::clamp(frame_gen, 0, FrameGenCount - 1)];
}

} // namespace BbSettings
