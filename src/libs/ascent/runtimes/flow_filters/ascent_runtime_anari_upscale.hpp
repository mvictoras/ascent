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

}}} // namespace ascent::runtime::filters

#endif // ASCENT_RUNTIME_ANARI_UPSCALE_HPP
