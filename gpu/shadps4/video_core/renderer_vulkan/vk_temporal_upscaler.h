// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: temporal upscaling of Bloodborne's HDR scene color (docs/upscaler.md). BB_UPSCALER=fsr3
// runs FSR 3.1 (FireBurn/FSR-Vulkan, native Vulkan) on the scene color right before the
// post-processing combine pass, with the scene depth and camera motion vectors, and writes the
// result back so the game's own post, tonemap and UI continue unchanged (Native AA preset).
//
// Guest targets stay at 1920x1080. Host scene targets use output/preset dimensions,
// resized at frame boundaries. At 1080p HDR reconstruction precedes post; other outputs
// reconstruct tonemapped scene proxies before output-size UI/display composition.
// BB_RENDER_RES retains the older startup-patched guest-resolution path.

#pragma once

#include <array>
#include <chrono>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

#include "common/types.h"
#include "video_core/renderer_vulkan/vk_common.h"
#include "video_core/renderer_vulkan/vk_dlss.h"
#include "video_core/renderer_vulkan/vk_xess.h"
#include "video_core/renderer_vulkan/vk_fsr4.h"
#include "video_core/texture_cache/image.h"

struct FfxVkPortableUpscaleContext;

namespace VideoCore {
class TextureCache;
}

namespace Vulkan {

class Instance;
class Scheduler;
class Runtime;
class CameraMotion;
class SceneTargets;
class FrameGenerator;

class TemporalUpscaler {
public:
    TemporalUpscaler(const Instance& instance, Scheduler& scheduler,
                     VideoCore::TextureCache& texture_cache, Runtime& runtime,
                     CameraMotion& camera_motion, SceneTargets& scene_targets);
    ~TemporalUpscaler();

    [[nodiscard]] bool Enabled() const noexcept {
        return enabled;
    }

    /// A draw into a full-size RGBA16F target with the scene depth: the scene color.
    void OnSceneColor(VideoCore::ImageId color);

    /// A blended (transparent) draw into the scene color that is not a full-screen pass: the
    /// first one of a frame snapshots the opaque scene for the reactive mask.
    void OnBlendedSceneDraw();

    /// A full-screen pass into the scene color: after a snapshot, the reactive mask is taken
    /// before it (the fog composite rewrites every pixel).
    void OnSceneComposite();

    /// Before a compute dispatch: the post-processing combine shader triggers the upscale.
    void OnDispatch(u64 cs_hash);

    /// bbport: frame generation takes its depth and motion vectors at the same point.
    void SetFrameGenerator(FrameGenerator* generator) noexcept {
        frame_generator = generator;
    }
    /// bbport: BB_HUDLESS_PS=<pixel shader hash>: the draw that writes the game's finished
    /// picture into the display buffer before the UI (Uncharted). After it, frame generation
    /// copies that target: the frame without UI.
    void OnDrawRecorded(u64 ps_hash, VideoCore::ImageId color);

    /// Start of a frame in the command stream (display pass).
    bool OnFrameStart();
    bool RasterScaling() const;
    /// bbport: LOD bias for scene materials, log2(render / output) while a reduced scene is
    /// upscaled (0 at native size, for TAA and without an upscaler).
    [[nodiscard]] float SceneMipBias() const;
    /// bbport: the state RedirectColor/RedirectDepth/RasterScaling depend on, for memoizing a
    /// draw's render state (Rasterizer::BeginRendering).
    [[nodiscard]] u64 RedirectState() const noexcept {
        return u64(ui_phase) | u64(display_redirect) << 1 | u64(done_this_frame) << 2 |
               u64(ui_color.index) << 8 | u64(ui_depth.index) << 36;
    }


    /// This frame's sub-pixel jitter in pixels (screen x right, y down); zero when off and after
    /// the upscale (post and UI are not jittered).
    [[nodiscard]] std::array<float, 2> Jitter() const noexcept {
        // Projection jitter (prototype): the game's projection carries it; no viewport / vertex
        // shader jitter and none for the per-pass fixes.
        return done_this_frame || ProjectionJitterOn() ? std::array<float, 2>{} : jitter;
    }

    // Scaled presets.

    /// After identifying this draw's targets, before drawing any UI pixels. Menus use
    /// spatial background copy and native UI, without needing scene depth or FSR history.
    void OnDraw(u64 vs_hash, VideoCore::ImageId color, VideoCore::ImageId depth,
                bool native_viewport);

    /// A pass's first color target: the last render-size RGBA8 target before the UI is the
    /// game's finished frame.
    void OnColorTarget(VideoCore::ImageId color);

    /// bbport: the first draw into a display buffer, before CameraMotion::OnDisplayPass clears
    /// the frame's depth and camera. Velocity mode (Uncharted) at a patched render size
    /// (BB_RENDER_RES, BB_OUTPUT_RES): records this frame's motion vectors and keeps its depth
    /// and jitter for the upscale at the first UI draw (RunScaledVelocity).
    void BeforeDisplayPass(VAddr display);

    struct Target {
        vk::ImageView view;
        vk::ImageLayout layout;
        u32 width, height;
        bool native_ui = false;
    };
    /// Render target redirection (UI passes, display pass): false when not redirected.
    /// `view` is the game's view of the image: redirected views mirror its format (sRGB) and,
    /// for sampling, its channel swizzle.
    bool RedirectColor(VideoCore::ImageId color, const VideoCore::ImageViewInfo& view,
                       Target& target);
    /// `color_to_ui`: the pass's color target goes to the UI image (RedirectColor).
    bool RedirectDepth(VideoCore::ImageId depth, Target& target, bool color_to_ui);
    /// A sampled image replaced by the upscaled frame (the display pass).
    bool RedirectSampled(VideoCore::ImageId image, const VideoCore::ImageViewInfo& info,
                         vk::ImageView& view, vk::ImageLayout& layout);
    /// Whether RedirectSampled would replace `image` (it may record a barrier then).
    [[nodiscard]] bool RedirectsSampled(VideoCore::ImageId image) const {
        return display_redirect && image == ui_color;
    }

    /// Presenter: the output-size display buffer standing in for the guest one at `address`.
    struct Display {
        vk::Image image;
        vk::Format format;
        u32 width, height;
        /// bbport (scene HDR): the HDR scene before the UI (RGBA16F, ShaderReadOnlyOptimal, the
        /// display's size) when `image` is the HDR composite; null otherwise.
        vk::ImageView hdr_hudless{};
    };
    bool DisplayOverride(VAddr address, Display& display);

    /// bbport: Uncharted 2's scene HDR (BbHdr::Scene, gpu/hdr/README.md). The tone pass is drawn
    /// again with its HDR replacement into the Tone shadow; the pass after it (BB_HUDLESS_PS) again
    /// with that shadow as its input into the Scene shadow (linear HDR, the game's distortion and
    /// overlays included), which the upscale reads instead of the game's 8-bit frame
    /// (RunScaledVelocity); the presenter shows the HDR composite of it and the frame's HUD.
    /// Blur1/Blur2: the game's motion blur after the tone pass (compute 006d71bb: the half-size
    /// colour and weight from the tone pass output, then 19367542: the blur), run again on the
    /// Tone shadow; the pass after the tone pass adds Blur2 (its binding 3) where things move.
    enum class HdrShadow { Tone, Scene, Blur1, Blur2 };
    struct ShadowTarget {
        vk::Image image;
        vk::ImageView view;
    };
    [[nodiscard]] bool HdrScene() const noexcept {
        return hdr_scene;
    }
    /// The shadow's RGBA16F image at w x h (made or resized here), for this frame's draw.
    ShadowTarget HdrShadowTarget(HdrShadow which, u32 w, u32 h);
    void HdrShadowDrawn(HdrShadow which) {
        Shadow(which).drawn = true;
    }
    [[nodiscard]] bool HdrShadowWasDrawn(HdrShadow which) const noexcept {
        return Shadow(which).drawn;
    }
    [[nodiscard]] vk::ImageView HdrShadowView(HdrShadow which) const noexcept {
        return *Shadow(which).view;
    }
    [[nodiscard]] std::pair<u32, u32> HdrShadowSize(HdrShadow which) const noexcept {
        return {Shadow(which).width, Shadow(which).height};
    }
    /// A motion blur dispatch to run again on the HDR values: its output shadow, and the shadow
    /// it reads in place of the game's image (Tone for the first, Blur1 for the second).
    [[nodiscard]] bool HdrBlurPass(u64 cs_hash, HdrShadow& out, HdrShadow& in) const noexcept {
        if (!hdr_scene || !tone_shadow.drawn) {
            return false;
        }
        if (u32(cs_hash) == 0x006d71bbu) {
            out = HdrShadow::Blur1;
            in = HdrShadow::Tone;
            return true;
        }
        if (u32(cs_hash) == 0x19367542u && blur1_shadow.drawn) {
            out = HdrShadow::Blur2;
            in = HdrShadow::Blur1;
            return true;
        }
        return false;
    }
    /// The pass after the tone pass, with a Tone shadow drawn this frame to read.
    [[nodiscard]] bool HdrPostPass(u64 ps_hash) const noexcept {
        return hdr_scene && hudless_hash && u32(ps_hash) == hudless_hash && tone_shadow.drawn;
    }
    [[nodiscard]] vk::ImageView ToneShadowView() const noexcept {
        return *tone_shadow.view;
    }

private:
    /// bbport: views of guest images the upscaler reads, kept across frames: FSR 4 registers
    /// images by view in a registry of eight (a new view per frame filled it at Native AA).
    vk::ImageView CachedView(const VideoCore::Image& image, vk::Format format,
                             vk::ImageAspectFlags aspect);
    /// The stencil aspect of a depth image (CachedView), or null for a format without one.
    vk::ImageView StencilView(const VideoCore::Image& depth);
    struct ViewEntry {
        vk::Image image;
        u64 uid = 0;
        vk::Format format{};
        vk::ImageAspectFlags aspect{};
        vk::ImageView view;
        u64 last_use = 0;
    };
    std::array<ViewEntry, 8> view_cache{};
    u64 view_uses = 0;

    void Run();
    /// The scene depth and the game's motion vectors for frame generation (velocity mode).
    void CaptureFrameGen();
    void RunScaled();
    /// Velocity mode at a patched render size: the finished frame in the display buffer
    /// (`display`), upscaled to the output size before the game's UI, which then draws into the
    /// output-size image the presenter shows.
    /// `source`: the image the final composite drew the frame into; `target`: the one the UI
    /// draws into (the texture cache may keep several images of the display buffer).
    void RunScaledVelocity(VideoCore::ImageId source, VideoCore::ImageId target);
    /// The upscale of RunScaled/RunScaledVelocity after the motion vectors: into ui_image.
    bool DispatchToUi(vk::CommandBuffer cmdbuf, const VideoCore::Image& color,
                      const VideoCore::Image& depth, vk::Image color_image,
                      vk::ImageView color_view, vk::Format color_format, vk::Image depth_image,
                      vk::ImageView depth_view, u32 source_width, u32 source_height, u32 w, u32 h,
                      u32 ow, u32 oh, bool hdr_input = false);
    void RunUiOnly(VideoCore::ImageId color, VideoCore::ImageId depth);
    /// bbport (scene HDR): after the upscale of the HDR scene into output_image (General): the
    /// 8-bit frame the HUD draws into (ui_image) and the scene in the presenter's domain, in place.
    void RecordHdrSplit(vk::CommandBuffer cmdbuf, vk::Image scene, vk::ImageView scene_view,
                        vk::Image sdr, vk::ImageView sdr_view, u32 w, u32 h);
    /// The UI image through a UNORM view (its own format may be sRGB), for compute passes.
    vk::ImageView UiStorageView();
    /// bbport (scene HDR with the upscaler before the post-processing, early_native): the pass
    /// after the tone pass has just drawn the display frame `color` and its HDR copy (the Scene
    /// shadow, at the display's size: no upscale follows): sharpens the HDR scene if that was
    /// left for the finished frame and splits it (the display buffer gets the 8-bit frame made
    /// from it, for the HUD and the composite at the flip). False: this frame stays SDR.
    bool HdrEarlyFrame(VideoCore::ImageId color);
    /// At the flip: the HDR composite of the scene and the frame's HUD (hdr_composite_image).
    void RecordHdrComposite(vk::CommandBuffer cmdbuf);
    void CreateHdrPipelines();
    void EnsureUiResources(u32 width, u32 height, vk::Format color, vk::Format depth);
    void PrepareUiDepth(VideoCore::ImageId depth);
    /// Render size below the scaled-preset output size (the resolution patch is on).
    [[nodiscard]] bool Scaled() const;
    /// A target of the patched render size: the game allocates it with aligned dimensions
    /// (a 1916x1078 scene in 1916x1080 targets).
    [[nodiscard]] bool RenderTarget(u32 w, u32 h) const {
        if (!scaled_session) return w == 1920 && h == 1080;
        return w >= render_width && h >= render_height && w < render_width + 8 &&
               h < render_height + 8;
    }
    /// The scene's size inside a `w` x `h` render target: the game's viewport.
    [[nodiscard]] std::array<u32, 2> SceneSize(u32 w, u32 h) const;
    // The scene color is drawn into a reduced SceneTargets proxy (live presets).
    [[nodiscard]] bool ReducedScene(const VideoCore::Image& color) const;
    /// Available and switched on (menu setting, toggle 1 << 24).
    [[nodiscard]] bool Active() const;
    [[nodiscard]] bool ReactiveOn() const;
    bool EnsureResources(u32 width, u32 height, u32 out_width, u32 out_height, bool hdr);
    /// bbport: creates the upscaler's pipelines and (make_context) the FSR 3 context for the expected
    /// scene size on a background thread, so the first 3D scene does not wait for them
    /// (EnsureResources adopts the context when the size matches).
    void StartContextPrecreate(u32 width, u32 height, u32 out_width, u32 out_height, bool hdr,
                               bool make_context);
    void CreatePipelines();
    /// Records the reactive mask pass; false when there is no snapshot this frame.
    bool RecordReactive(vk::ImageView color_view);
    /// FSR 4 is selected, possible in this session (not BB_RENDER_RES) and has not failed.
    [[nodiscard]] bool UseFsr4() const;
    /// bbport: DLSS selected and the bridge is ready (NVIDIA RTX, bbport_dlss.dll).
    [[nodiscard]] bool UseDlss() const;
    /// Records DLSS into `cmdbuf` (output in General). `hdr`: linear scene color input.
    /// bbport: true while DLSS's history should be reset for a fade in (see the .cpp).
    bool FadeReset(vk::CommandBuffer cmdbuf, const Dlss::Resource& color, bool reset);
    /// bbport: the save picture from the scene before the UI (bbport_thumbnail.h): ui_image scaled down into a small
    /// image and copied to host memory about once a second; read back once the GPU has run it.
    void CaptureCleanPicture(vk::CommandBuffer cmdbuf, vk::Image source, u32 width, u32 height);
    void ReadCleanPicture();
    struct {
        vk::Image image{};
        VmaAllocation image_allocation{};
        vk::Buffer buffer{};
        VmaAllocation buffer_allocation{};
        void* mapped = nullptr;
        u64 tick = 0;
        bool pending = false, failed = false;
    } clean_picture;
    /// bbport (diagnosis, BB_MOTION_TRIGGER=<file>): once the file appears, the scene before the UI of every frame,
    /// a fifth of its size in one channel, with the frame's time, for BB_MOTION_FRAMES frames (720); written next to
    /// the trigger file once the GPU has run them (motion_<n>.raw, motion_<n>.txt). Small enough not to change the
    /// frame timing it is there to show (judder: does an object move on every frame or in steps).
    void CaptureMotion(vk::CommandBuffer cmdbuf, vk::Image source, u32 width, u32 height);
    void WriteMotion();
    struct {
        vk::Image image{};
        VmaAllocation image_allocation{};
        vk::Buffer buffer{};
        VmaAllocation buffer_allocation{};
        void* mapped = nullptr;
        u32 width = 0, height = 0;
        int frames = 0, taken = 0, written = 0;
        bool armed = false, pending_write = false, failed = false;
        u64 write_tick = 0;
        std::vector<double> times; // ms since the capture started, per frame
        std::chrono::steady_clock::time_point start{};
    } motion;
    bool RecordDlss(vk::CommandBuffer cmdbuf, const Dlss::Resource& color,
                    const Dlss::Resource& depth, u32 w, u32 h, u32 ow, u32 oh, float frame_ms,
                    bool hdr);
    /// bbport: Intel XeSS selected and libxess.dll is ready.
    [[nodiscard]] bool UseXess() const;
    /// Records XeSS into output_image (General). Moves color, depth and the motion vectors to
    /// ShaderReadOnlyOptimal for it and back to General; on a failure FSR 3 takes over.
    bool RecordXess(vk::CommandBuffer cmdbuf, const Xess::Resource& color,
                    const Xess::Resource& depth, u32 w, u32 h, u32 ow, u32 oh, bool hdr);
    /// Records FSR 4 into output_image; on a permanent failure FSR 3 takes over.
    bool RecordFsr4(vk::CommandBuffer cmdbuf, Fsr4Upscaler::Image color, Fsr4Upscaler::Image depth,
                    u32 w, u32 h, u32 ow, u32 oh, float frame_ms);
    void RecordTaa(vk::CommandBuffer cmdbuf, vk::ImageView color, vk::ImageView depth);
    /// Sharpness above 1 for FSR 3/4 (their RCAS stops at 1): one more RCAS pass over the target
    /// (output_image, or the 8-bit UI image with ldr) in General layout after the upscaler.
    /// With `deferred` the pass is recorded in order by the recording thread (Scheduler::Record) and
    /// `cmdbuf` is not used: no wait for that thread (SharpenDisplay, every frame).
    void ExtraSharpen(vk::CommandBuffer cmdbuf, vk::Image target, bool ldr, u32 w, u32 h,
                      vk::ImageView view = {}, bool srgb_source = false, bool deferred = false);
    /// bbport (DLSS rework 3c): Native before post: the sharpening pass on the finished display
    /// frame (after the game's post-processing, before its UI), not on the HDR scene.
    void SharpenDisplay(VideoCore::ImageId color);
    bool sharpen_pending = false;      ///< this frame's upscale left its sharpening for the display frame
    bool sharpen_display_failed = false; ///< the display buffer cannot take it: sharpen before post

    const Instance& instance;
    Scheduler& scheduler;
    VideoCore::TextureCache& texture_cache;
    Runtime& runtime;
    CameraMotion& camera_motion;
    SceneTargets& scene_targets;
    FrameGenerator* frame_generator = nullptr;
    bool frame_gen_captured = false; ///< this frame's inputs are taken
    u64 hudless_hash = 0;
    /// Frame generation's picture without UI (not upscaled at a patched size): taken once the
    /// game's full-screen passes into the display buffer are done, i.e. at the frame's first UI
    /// draw there or, without one, when the frame is shown. The game's FXAA (6bd1e015 luma,
    /// 749bbec9 into the display buffer) runs after BB_HUDLESS_PS: a copy taken at that pass
    /// differed from the shown frame at every contrasted edge, which DLSS-G took for UI.
    void CaptureHudlessNow(VideoCore::ImageId color, const char* where);
    bool hudless_pending = false;
    VideoCore::ImageId hudless_color{};
    VAddr hudless_address = 0;
    int applied_preset = -1;
    int applied_upscaler = -1;
    bool dispatched_last_frame = false;
    bool last_active = false, last_jitter = false;


    bool enabled = false;
    bool failed = false;
    u64 trigger_hash = 0x9a9cf8a9;
    VideoCore::ImageId scene_color{};
    bool done_this_frame = false;
    u32 preset_file_frames = 0; ///< BB_PRESET_FILE polling
    int applied_output = -1;
    bool snapshot_taken = false;
    bool opaque_valid = false;
    bool mask_ready = false;
    std::array<float, 2> jitter{};
    u32 jitter_index = 0;
    u64 jitter_flip = ~0ull; ///< the flip the jitter last advanced at (once per shown frame)
    u64 start_flip = ~0ull;  ///< the flip of the last OnFrameStart's frame bookkeeping
    bool reset = true;
    u64 frame_id = 0;
    std::chrono::steady_clock::time_point last_frame{};
    float last_frame_cpu_ms = 0.0f; ///< the GPU thread's time between the last two evaluations
    /// The frame time for the upscalers (the frame's world step; see the definition).
    float FrameMs();

    u32 width = 0, height = 0;             ///< render size
    u32 out_width = 0, out_height = 0;     ///< output size of the context
    bool context_hdr = true;
    u32 target_width = 1920, target_height = 1080; ///< output size of scaled presets
    u64 ui_trigger_vs = 0x34e8a281;
    VideoCore::ImageId ldr_target{};
    VideoCore::ImageId ui_color{}, ui_depth{};
    bool ui_phase = false;         ///< from the upscale to the next display pass
    bool display_redirect = false; ///< the display pass of an upscaled frame
    bool ui_read_barrier = false;
    VideoCore::UniqueImage ui_image;
    VideoCore::UniqueImage ui_depth_image;
    vk::UniqueImageView ui_view;
    vk::UniqueImageView ui_depth_view;
    bool depth_blit = false;
    bool scaled_session = false;
    /// bbport (DLSS rework 3): a patched render size equal to the output (Native / DLAA): the
    /// upscaler runs on the HDR scene colour before the post-processing, as at the game's own
    /// 1920x1080 (Run at the trigger dispatch), not on the finished LDR frame. BB_NATIVE_BEFORE_POST=0: off.
    bool early_native = false;
    /// bbport (DLSS rework 12, BB_SCALED_BEFORE_POST=1): with early_native, the scene's passes are
    /// drawn at this smaller size (BB_SCENE_SIZE, set at process start from the wanted render
    /// size) into SceneTargets' stand-ins and upscaled before the post-processing; 0: Native.
    u32 early_scene_width = 0, early_scene_height = 0;
    [[nodiscard]] bool EarlyScaled() const noexcept {
        return early_native && early_scene_width != 0;
    }
    /// The size the scene is drawn at (jitter, mip bias): the stand-ins' with EarlyScaled.
    [[nodiscard]] u32 SceneWidth() const;
    [[nodiscard]] u32 SceneHeight() const;
    /// bbport PROTOTYPE (DLSS rework 7, BB_PROJECTION_JITTER=1, Uncharted 1-3, 1.02): the jitter
    /// is in the game's projection (runtime_ultrawide.c); OnFrameStart writes the next one to the
    /// game's slot, the frame's own is read back from the velocity pass (UseProjectionJitter).
    /// (The game code is patched after the GPU starts: looked up at each use.)
    [[nodiscard]] static bool ProjectionJitterOn() noexcept;
    void UseProjectionJitter(u32 w, u32 h);
    /// BeforeDisplayPass recorded this frame's motion vectors (velocity mode, patched size).
    bool velocity_pending = false;
    /// The UI phase belongs to RunScaledVelocity: the next display pass is not redirected.
    bool velocity_ui = false;
    u64 velocity_ui_flip = 0; ///< the flip count when RunScaledVelocity started the UI phase
    u32 draw_vs = 0;          ///< the vertex shader of the draw OnDraw last saw
    VideoCore::ImageId velocity_depth{};
    VAddr velocity_address = 0;           ///< the display buffer of the frame
    VideoCore::ImageId velocity_display{}; ///< the image the frame's first draw there used
    std::array<float, 2> velocity_jitter{};
    u32 velocity_width = 0, velocity_height = 0;
    u32 ui_width = 0, ui_height = 0;
    u32 render_width = 0, render_height = 0;
    vk::Format ui_format = vk::Format::eUndefined;
    vk::Format ui_depth_format = vk::Format::eUndefined;
    /// Views of the port's images in the formats/swizzles the game's views use.
    struct MirrorView {
        vk::Format format;
        vk::ComponentMapping mapping;
        vk::UniqueImageView view;
    };
    vk::ImageView Mirror(vk::Image image, std::vector<MirrorView>& views, vk::Format format,
                         vk::ComponentMapping mapping);
    std::vector<MirrorView> ui_views;
    struct DisplayImage {
        VideoCore::UniqueImage image;
        vk::Image alias{}; ///< RunScaledVelocity: ui_image stands for the display buffer
        std::vector<MirrorView> views;
        vk::Format format{};
        u32 width = 0, height = 0;
        bool valid = false;
    };
    std::mutex display_mutex;
    std::unordered_map<VAddr, DisplayImage> displays;
    FfxVkPortableUpscaleContext* context = nullptr;
    struct PrecreatedContext {
        FfxVkPortableUpscaleContext* context = nullptr;
        u32 width = 0, height = 0, out_width = 0, out_height = 0;
        bool hdr = false;
    };
    PrecreatedContext precreated{}; ///< written by precreate_thread, read after joining it
    std::jthread precreate_thread;
    bool precreate_started = false;
    bool resources_ready = false; ///< images below match width/height/out size
    bool resources_fsr4 = false;  ///< made for FSR 4 (no FSR 3 context)
    bool resources_taa = false;
    std::unique_ptr<Fsr4Upscaler> fsr4;
    bool fsr4_failed = false;
    bool dlss_failed = false;
    bool xess_failed = false;
    VideoCore::UniqueImage motion_image;
    VideoCore::UniqueImage output_image;
    vk::UniqueImageView motion_view;
    vk::UniqueImageView output_view;
    VideoCore::UniqueImage opaque_image;   ///< scene color before the blended draws
    VideoCore::UniqueImage reactive_image; ///< R8 reactive mask
    vk::UniqueImageView opaque_view;
    vk::UniqueImageView reactive_view;
    vk::UniqueDescriptorSetLayout reactive_desc_layout;
    vk::UniquePipelineLayout reactive_pipeline_layout;
    vk::UniquePipeline reactive_pipeline;
    vk::UniqueDescriptorSetLayout merge_desc_layout;
    vk::UniquePipelineLayout merge_pipeline_layout;
    vk::UniquePipeline merge_pipeline;
    std::array<VideoCore::UniqueImage, 2> taa_history;
    std::array<vk::UniqueImageView, 2> taa_history_views;
    u32 taa_next = 0;
    vk::UniqueSampler taa_sampler;
    vk::UniqueDescriptorSetLayout taa_desc_layout;
    vk::UniquePipelineLayout taa_pipeline_layout;
    vk::UniquePipeline taa_pipeline;
    vk::UniqueDescriptorSetLayout taa_sharpen_desc_layout;
    vk::UniquePipelineLayout taa_sharpen_pipeline_layout;
    vk::UniquePipeline taa_sharpen_pipeline;
    vk::UniquePipeline taa_sharpen_ldr_pipeline;
    // ExtraSharpen: a copy of the upscaled frame (RCAS reads neighbours) and the target views.
    VideoCore::UniqueImage extra_sharpen_image;
    vk::UniqueImageView extra_sharpen_view;
    u32 extra_sharpen_width = 0, extra_sharpen_height = 0;
    vk::UniqueImageView ui_storage_view;

    // bbport: scene HDR (HdrScene).
    bool hdr_scene = false; ///< BbHdr::Scene at start
    struct ShadowImage {
        VideoCore::UniqueImage image;
        vk::UniqueImageView view;
        u32 width = 0, height = 0;
        bool drawn = false; ///< this frame
    };
    ShadowImage tone_shadow, scene_shadow, blur1_shadow, blur2_shadow;
    ShadowImage& Shadow(HdrShadow which) noexcept {
        return which == HdrShadow::Tone    ? tone_shadow
               : which == HdrShadow::Scene ? scene_shadow
               : which == HdrShadow::Blur1 ? blur1_shadow
                                           : blur2_shadow;
    }
    const ShadowImage& Shadow(HdrShadow which) const noexcept {
        return const_cast<TemporalUpscaler*>(this)->Shadow(which);
    }
    bool last_hdr_input = false; ///< the upscaler's input domain last frame (its history)
    bool hdr_frame = false;      ///< output_image holds the shown frame's HDR scene (split done)
    VAddr hdr_address = 0;       ///< the display buffer of that frame
    /// The shown frame's HDR scene (output_image, or the Scene shadow with hdr_early) and the 8-bit
    /// frame the HUD is drawn over (ui_image, or the game's display buffer), as the split left them.
    struct HdrShown {
        vk::Image scene;
        vk::ImageView scene_view;
        vk::Image sdr;
        vk::ImageView sdr_view;
        u32 width = 0, height = 0;
    } hdr_shown;
    bool hdr_early = false;               ///< hdr_frame was made by HdrEarlyFrame
    VideoCore::ImageId hdr_early_color{}; ///< its display buffer
    u64 hdr_early_flip = 0;               ///< the flip count when it was made
    vk::ImageLayout hdr_output_layout = vk::ImageLayout::eGeneral; ///< hdr_shown.scene's, after the split
    VideoCore::UniqueImage hdr_composite_image;
    vk::UniqueImageView hdr_composite_view;
    u32 hdr_composite_width = 0, hdr_composite_height = 0;
    vk::UniqueDescriptorSetLayout hdr_desc_layout;
    vk::UniquePipelineLayout hdr_pipeline_layout;
    vk::UniquePipeline hdr_split_pipeline, hdr_composite_pipeline;
};

} // namespace Vulkan
