//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~//
// Copyright (c) Lawrence Livermore National Security, LLC and other Ascent
// Project developers. See top-level LICENSE AND COPYRIGHT files for dates and
// other details. No copyright assignment is required to contribute to Ascent.
//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~//

//-----------------------------------------------------------------------------
///
/// file: ascent_runtime_anari_upscale.hpp
///
/// Super-resolution upscaler abstraction for the ANARI render path.
///
/// Milestone 1 ships a CPU bilinear implementation that consumes the RGBA8
/// framebuffer returned by anari::map(channel.color). A future milestone will
/// add an airender-backed GPU implementation (FSR1/DLSS via EGL+OpenGL/Vulkan)
/// behind this same interface; depth/motion inputs are accepted now but ignored
/// by the CPU bilinear path so the interface does not change when GPU backends
/// land.
///
//-----------------------------------------------------------------------------

#ifndef ASCENT_RUNTIME_ANARI_UPSCALE_HPP
#define ASCENT_RUNTIME_ANARI_UPSCALE_HPP

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

//-----------------------------------------------------------------------------
namespace ascent { namespace runtime { namespace filters {

enum class UpscaleAlgorithm
{
    None,      ///< no upscaling; emit the native-resolution frame
    Bilinear,  ///< CPU bilinear (milestone 1)
    FSR1,      ///< airender AMD FSR1 (milestone 2, not yet implemented)
    DLSS,      ///< airender NVIDIA DLSS (milestone 2, not yet implemented)
};

/// Parse a YAML algorithm string ("bilinear" | "fsr1" | "dlss" | "none").
/// Unknown / empty strings return UpscaleAlgorithm::None.
UpscaleAlgorithm parse_upscale_algorithm(const std::string &name);

/// Config for one upscale request. factor is the linear scale (2.0 => 2x per
/// axis => 4x pixels). algorithm selects the backend.
struct UpscaleConfig
{
    UpscaleAlgorithm algorithm{UpscaleAlgorithm::None};
    double           factor{1.0};

    bool enabled() const
    {
        return algorithm != UpscaleAlgorithm::None && factor > 1.0;
    }
};

/// Per-frame inputs to an upscaler. color is required (RGBA8, row-major, 4
/// bytes/pixel). depth (float32, 1/pixel) and motion (float32 vec2, 2/pixel)
/// are only consumed by the DLSS backend; leave them null otherwise. jitter_x/y
/// are the DLSS sub-pixel camera-jitter offsets in [-0.5,0.5] pixel units
/// (Halton); ignored by bilinear/FSR1.
struct UpscaleInputs
{
    const std::uint8_t *color{nullptr};
    const float        *depth{nullptr};
    const float        *motion{nullptr};
    int                 src_w{0};
    int                 src_h{0};
    float               jitter_x{0.0f};
    float               jitter_y{0.0f};
};

/// Backend interface: take UpscaleInputs and produce an enlarged RGBA8 image.
class Upscaler
{
public:
    virtual ~Upscaler() = default;

    /// Upscale `in` into dst (dst_w x dst_h, RGBA8). dst is resized by the callee.
    virtual void upscale(const UpscaleInputs &in,
                         std::vector<std::uint8_t> &dst, int dst_w, int dst_h) = 0;
};

/// Construct the upscaler for the requested algorithm. Bilinear uses a pure-CPU
/// resampler with no GPU dependencies. FSR1 and DLSS use the airender GPU
/// backend (EGL/OpenGL plus Vulkan) in builds where ASCENT_AIRENDER_ENABLED is
/// defined, and otherwise degrade to CPU bilinear with a single warning.
std::unique_ptr<Upscaler> make_upscaler(const UpscaleConfig &cfg);

/// Pure-CPU bilinear upscaler, used directly for Bilinear and as the fallback
/// when a GPU backend is unavailable. Defined in ascent_runtime_anari_upscale.cpp.
std::unique_ptr<Upscaler> make_cpu_bilinear_upscaler();

/// Process-lifetime upscaler shared across filter recreation, keyed by
/// (algorithm, src, dst). Ascent rebuilds the flow graph (and the anari_*
/// filters) on every trigger, so a filter-owned upscaler would be destroyed and
/// rebuilt each frame; for DLSS that is a second in-process CreateFeature while
/// the first is still live and fails 0xbad00002. frame_seq is the shared
/// monotonic counter so DLSS Halton jitter advances across triggers.
std::shared_ptr<Upscaler> shared_upscaler(const UpscaleConfig &cfg,
                                          int src_w, int src_h,
                                          int dst_w, int dst_h,
                                          unsigned &frame_seq);

/// Process-lifetime accumulators for the ANARI render and super-resolution
/// stages. These must outlive the filters, which Ascent destroys and rebuilds
/// on every trigger, so they cannot be filter members.
///
/// render is measured on every rank. upscale_* are measured on rank 0 only,
/// because the map/upscale/encode block in the ANARI filter is rank-0 guarded;
/// a max-across-ranks reduction of those would be meaningless.
/// upscale_infer covers just the backend call (the FSR1/DLSS inference);
/// upscale_total additionally covers the colour upload and result readback.
struct AnariStageTimings
{
    double render{0.0};
    double upscale_infer{0.0};
    double upscale_total{0.0};
    long long render_calls{0};
    long long upscale_calls{0};

    // Time in pipeline transforms (slice, contour, clip, composite_vector,
    // ...) as opposed to the anari extract. Accumulated by the flow workspace,
    // which already measures every filter, so this needs no second timer.
    double viz{0.0};
    long long viz_calls{0};
};

AnariStageTimings &anari_stage_timings();

/// Registers the flow filter-timing callback that fills AnariStageTimings::viz.
/// Call once at runtime setup.
void install_anari_filter_timing_sink();

/// Previous frame's view-projection, needed by Barney's motion.viewProjection /
/// motion.previousViewProjection pair to write channel.motion. Barney only
/// emits motion vectors when BOTH are supplied (anari/Camera.cpp), so the
/// previous matrix has to survive between triggers; like the timings above it
/// cannot live in the filter, which Ascent rebuilds every frame.
struct AnariCameraMotion
{
    float prev_view_proj[16]{};
    bool  has_prev{false};

    // Jitter must be applied to the camera on every rank before rendering,
    // while shared_upscaler's counter only advances on rank 0 after it. This
    // is the render-side counter; keeping it here keeps it per-extract.
    unsigned jitter_seq{0};
    float    jitter_x{0.0f};
    float    jitter_y{0.0f};
};

/// Keyed per extract: a single actions file can declare several anari extracts
/// (pb146 has three), each with its own camera. One shared store would let each
/// extract overwrite the others' previous matrix within the same trigger,
/// yielding prev == curr and therefore zero motion.
AnariCameraMotion &anari_camera_motion(const std::string &key);

}}} // namespace ascent::runtime::filters

#endif // ASCENT_RUNTIME_ANARI_UPSCALE_HPP
