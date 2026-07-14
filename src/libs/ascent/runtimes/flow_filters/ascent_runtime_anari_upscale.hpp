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

/// Backend interface. Implementations take a source RGBA8 image and produce an
/// enlarged RGBA8 image. depth/motion are optional GPU-backend inputs (nullptr
/// for the CPU bilinear path).
class Upscaler
{
public:
    virtual ~Upscaler() = default;

    /// Upscale src (src_w x src_h, RGBA8, row-major, 4 bytes/pixel) into dst
    /// (dst_w x dst_h, RGBA8). dst is resized by the callee.
    virtual void upscale(const std::uint8_t *src, int src_w, int src_h,
                         std::vector<std::uint8_t> &dst, int dst_w, int dst_h) = 0;
};

/// Construct the upscaler for `cfg.algorithm`. Milestone 1 returns a CPU
/// bilinear upscaler for Bilinear; FSR1/DLSS fall back to bilinear with a
/// one-time warning until the airender backend lands.
std::unique_ptr<Upscaler> make_upscaler(const UpscaleConfig &cfg);

}}} // namespace ascent::runtime::filters

#endif // ASCENT_RUNTIME_ANARI_UPSCALE_HPP
