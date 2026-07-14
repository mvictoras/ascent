//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~//
// Copyright (c) Lawrence Livermore National Security, LLC and other Ascent
// Project developers. See top-level LICENSE AND COPYRIGHT files for dates and
// other details. No copyright assignment is required to contribute to Ascent.
//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~//

#include "ascent_runtime_anari_upscale.hpp"

#include <ascent_logging.hpp>

#include <algorithm>
#include <cmath>

namespace ascent { namespace runtime { namespace filters {

//-----------------------------------------------------------------------------
UpscaleAlgorithm
parse_upscale_algorithm(const std::string &name)
{
    std::string lowered;
    lowered.reserve(name.size());
    for (char c : name)
    {
        lowered.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }

    if (lowered == "bilinear") return UpscaleAlgorithm::Bilinear;
    if (lowered == "fsr1")     return UpscaleAlgorithm::FSR1;
    if (lowered == "dlss")     return UpscaleAlgorithm::DLSS;
    return UpscaleAlgorithm::None;
}

namespace
{

/// CPU bilinear resampler over RGBA8 buffers. Uses half-pixel-centered sampling
/// (the standard convention: sample at (x+0.5)*src/dst - 0.5) so the output is
/// not biased toward the top-left corner and matches GPU GL_LINEAR blits.
class BilinearUpscaler final : public Upscaler
{
public:
    void upscale(const std::uint8_t *src, int src_w, int src_h,
                 std::vector<std::uint8_t> &dst, int dst_w, int dst_h) override
    {
        dst.assign(std::size_t(dst_w) * dst_h * 4, 0);

        if (src_w <= 0 || src_h <= 0 || dst_w <= 0 || dst_h <= 0)
        {
            return;
        }

        const double scale_x = double(src_w) / dst_w;
        const double scale_y = double(src_h) / dst_h;

        for (int dy = 0; dy < dst_h; ++dy)
        {
            double sy = (dy + 0.5) * scale_y - 0.5;
            sy = std::clamp(sy, 0.0, double(src_h - 1));
            const int   y0 = int(std::floor(sy));
            const int   y1 = std::min(y0 + 1, src_h - 1);
            const double wy = sy - y0;

            for (int dx = 0; dx < dst_w; ++dx)
            {
                double sx = (dx + 0.5) * scale_x - 0.5;
                sx = std::clamp(sx, 0.0, double(src_w - 1));
                const int   x0 = int(std::floor(sx));
                const int   x1 = std::min(x0 + 1, src_w - 1);
                const double wx = sx - x0;

                std::uint8_t *out = &dst[(std::size_t(dy) * dst_w + dx) * 4];
                for (int c = 0; c < 4; ++c)
                {
                    const double p00 = src[(std::size_t(y0) * src_w + x0) * 4 + c];
                    const double p01 = src[(std::size_t(y0) * src_w + x1) * 4 + c];
                    const double p10 = src[(std::size_t(y1) * src_w + x0) * 4 + c];
                    const double p11 = src[(std::size_t(y1) * src_w + x1) * 4 + c];
                    const double top = p00 + wx * (p01 - p00);
                    const double bot = p10 + wx * (p11 - p10);
                    const double val = top + wy * (bot - top);
                    out[c] = static_cast<std::uint8_t>(std::lround(std::clamp(val, 0.0, 255.0)));
                }
            }
        }
    }
};

} // namespace (anonymous)

//-----------------------------------------------------------------------------
std::unique_ptr<Upscaler>
make_upscaler(const UpscaleConfig &cfg)
{
    switch (cfg.algorithm)
    {
        case UpscaleAlgorithm::Bilinear:
            return std::make_unique<BilinearUpscaler>();

        case UpscaleAlgorithm::FSR1:
        case UpscaleAlgorithm::DLSS:
            ASCENT_LOG_INFO("Anari upscale: FSR1/DLSS backend not yet available; "
                            "falling back to CPU bilinear");
            return std::make_unique<BilinearUpscaler>();

        case UpscaleAlgorithm::None:
        default:
            return nullptr;
    }
}

}}} // namespace ascent::runtime::filters
