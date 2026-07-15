//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~//
// Copyright (c) Lawrence Livermore National Security, LLC and other Ascent
// Project developers. See top-level LICENSE AND COPYRIGHT files for dates and
// other details. No copyright assignment is required to contribute to Ascent.
//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~//

//-----------------------------------------------------------------------------
///
/// file: ascent_runtime_anari_upscale_gpu.cpp
///
/// airender-backed GPU upscaler (FSR1 / DLSS / GPU bilinear) behind the
/// Upscaler interface. Owns a headless EGL/OpenGL context so airender's GL
/// path works inside Ascent's process. Falls back to CPU bilinear when
/// ASCENT_AIRENDER_ENABLED is not defined or EGL/GL initialization fails.
///
//-----------------------------------------------------------------------------

#include "ascent_runtime_anari_upscale.hpp"

#include <ascent_config.h> // ASCENT_AIRENDER_ENABLED
#include <ascent_logging.hpp>

#include <memory>
#include <string>

//-----------------------------------------------------------------------------
#if defined(ASCENT_AIRENDER_ENABLED)

#include <glad/egl.h>
#include <glad/gl.h>

#include <airender.h>

#include <cstdio>
#include <cstring>
#include <mutex>

namespace ascent { namespace runtime { namespace filters {

namespace
{

air::Upscaler to_air_upscaler(UpscaleAlgorithm a)
{
    switch (a)
    {
        case UpscaleAlgorithm::FSR1: return air::Upscaler::AMD_FSR1;
        case UpscaleAlgorithm::DLSS: return air::Upscaler::NVIDIA_DLSS;
        default:                     return air::Upscaler::BILINEAR;
    }
}

/// Headless EGL 1.5 OpenGL context (pbuffer surface) on the first CUDA-capable
/// EGL device. Mirrors the reference anari-ippl app's initializeEGL(). Created
/// lazily and made current per use, because a GL context is thread-affine and
/// Ascent may invoke the filter from a worker thread.
class EglContext
{
public:
    bool ensure()
    {
        if (initialized_)
        {
            return valid_;
        }
        initialized_ = true;
        valid_       = init();
        return valid_;
    }

    bool make_current()
    {
        if (!valid_)
        {
            return false;
        }
        return eglMakeCurrent(display_, surface_, surface_, context_) == EGL_TRUE;
    }

    ~EglContext()
    {
        // Intentionally leak the EGL display/context: it is only ever destroyed
        // at process shutdown, so the OS reclaims it anyway, and we avoid poking
        // the EGL teardown path from our side. NOTE: this does NOT by itself
        // prevent the teardown abort seen with the NVIDIA driver -- that abort
        // is a double-free inside the driver's own atexit thread-release path
        // (eglReleaseThread -> libGLX_nvidia; confirmed via gdb: the backtrace
        // has zero airender/Ascent frames) and fires independently of us, after
        // all rendering is complete and outputs are flushed. It is a driver bug,
        // not ours, and does not affect results. See run wrappers for how the
        // benign post-completion abort is treated as success.
    }

private:
    bool init()
    {
        if (!gladLoaderLoadEGL(EGL_NO_DISPLAY))
        {
            ASCENT_LOG_INFO("anari upscale(gpu): gladLoaderLoadEGL(no display) failed");
            return false;
        }

        static const int MAX_DEVICES = 16;
        EGLDeviceEXT devices[MAX_DEVICES];
        EGLint num_devices = 0;
        if (!eglQueryDevicesEXT(MAX_DEVICES, devices, &num_devices) || num_devices == 0)
        {
            ASCENT_LOG_INFO("anari upscale(gpu): no EGL devices found");
            return false;
        }

        // Prefer a CUDA-capable EGL device so the GL context lands on the same
        // class of GPU Barney used; fall back to device 0.
        EGLDeviceEXT chosen = devices[0];
        for (EGLint i = 0; i < num_devices; ++i)
        {
            EGLAttrib cuda_idx = 0;
            if (eglQueryDeviceAttribEXT(devices[i], EGL_CUDA_DEVICE_NV, &cuda_idx))
            {
                chosen = devices[i];
                break;
            }
        }

        display_ = eglGetPlatformDisplayEXT(EGL_PLATFORM_DEVICE_EXT, chosen, nullptr);
        if (display_ == EGL_NO_DISPLAY)
        {
            ASCENT_LOG_INFO("anari upscale(gpu): eglGetPlatformDisplayEXT failed");
            return false;
        }

        EGLint major = 0, minor = 0;
        if (!eglInitialize(display_, &major, &minor))
        {
            ASCENT_LOG_INFO("anari upscale(gpu): eglInitialize failed");
            return false;
        }
        gladLoaderLoadEGL(display_);

        const EGLint cfg_attribs[] = {
            EGL_SURFACE_TYPE,    EGL_PBUFFER_BIT,
            EGL_RED_SIZE,        8,
            EGL_GREEN_SIZE,      8,
            EGL_BLUE_SIZE,       8,
            EGL_ALPHA_SIZE,      8,
            EGL_DEPTH_SIZE,      24,
            EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
            EGL_NONE};
        EGLConfig config;
        EGLint num_configs = 0;
        if (!eglChooseConfig(display_, cfg_attribs, &config, 1, &num_configs) || num_configs == 0)
        {
            ASCENT_LOG_INFO("anari upscale(gpu): eglChooseConfig failed");
            return false;
        }

        const EGLint pbuffer_attribs[] = {EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE};
        surface_ = eglCreatePbufferSurface(display_, config, pbuffer_attribs);

        eglBindAPI(EGL_OPENGL_API);
        const EGLint ctx_attribs[] = {
            EGL_CONTEXT_MAJOR_VERSION,             4,
            EGL_CONTEXT_MINOR_VERSION,             5,
            EGL_CONTEXT_OPENGL_PROFILE_MASK,       EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT,
            EGL_CONTEXT_OPENGL_FORWARD_COMPATIBLE, EGL_TRUE,
            EGL_NONE};
        context_ = eglCreateContext(display_, config, EGL_NO_CONTEXT, ctx_attribs);
        if (context_ == EGL_NO_CONTEXT)
        {
            ASCENT_LOG_INFO("anari upscale(gpu): eglCreateContext failed");
            return false;
        }
        if (!eglMakeCurrent(display_, surface_, surface_, context_))
        {
            ASCENT_LOG_INFO("anari upscale(gpu): eglMakeCurrent failed");
            return false;
        }
        if (!gladLoaderLoadGL())
        {
            ASCENT_LOG_INFO("anari upscale(gpu): gladLoaderLoadGL failed");
            return false;
        }
        return true;
    }

    bool          initialized_{false};
    bool          valid_{false};
    EGLDisplay    display_{EGL_NO_DISPLAY};
    EGLSurface    surface_{EGL_NO_SURFACE};
    EGLContext    context_{EGL_NO_CONTEXT};
};

/// Upscaler backed by air::AiRenderer over the EglContext. Recreates the
/// AiRenderer whenever the source or destination dimensions change (airender
/// binds fixed input/output sizes at construction).
class GpuUpscaler final : public Upscaler
{
public:
    explicit GpuUpscaler(UpscaleAlgorithm algorithm)
        : algorithm_(algorithm)
    {}

    ~GpuUpscaler() override
    {
        if (egl_.make_current())
        {
            delete renderer_;
            renderer_ = nullptr;
        }
    }

    void upscale(const UpscaleInputs &in,
                 std::vector<std::uint8_t> &dst, int dst_w, int dst_h) override
    {
        std::lock_guard<std::mutex> guard(mutex_);

        if (!egl_.ensure() || !egl_.make_current())
        {
            fallback(in, dst, dst_w, dst_h);
            return;
        }

        if (!ensure_renderer(in.src_w, in.src_h, dst_w, dst_h))
        {
            fallback(in, dst, dst_w, dst_h);
            return;
        }

        // Upload the ANARI color buffer into the air-managed SRC_COLOR texture.
        glBindTexture(GL_TEXTURE_2D, tex_color_);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, in.src_w, in.src_h,
                        GL_RGBA, GL_UNSIGNED_BYTE, in.color);

        air::ImageType img_type = air::ImageType::COLOR;
        if (algorithm_ == UpscaleAlgorithm::DLSS && in.depth && in.motion)
        {
            upload_depth_motion(in);
            img_type = air::ImageType::COLOR_AND_DEPTH;
        }
        glBindTexture(GL_TEXTURE_2D, 0);
        glFinish();

        air::UpscaleOptions opts{};
        opts.camera_jitter_x = in.jitter_x;
        opts.camera_jitter_y = in.jitter_y;
        renderer_->upscaleImage(img_type, &fbo_, &opts);
        glFinish();

        readback(dst, dst_w, dst_h);
    }

private:
    bool ensure_renderer(int src_w, int src_h, int dst_w, int dst_h)
    {
        if (renderer_ && src_w == src_w_ && src_h == src_h_ && dst_w == dst_w_ && dst_h == dst_h_)
        {
            return true;
        }
        delete renderer_;
        renderer_ = nullptr;

        air::Init(reinterpret_cast<air::GetProcAddrFunc>(eglGetProcAddress));

        air::CreateInfo ci{};
        ci.renderer   = air::Renderer::OPENGL;
        ci.upscaler   = to_air_upscaler(algorithm_);
        ci.depth_fmt  = air::DepthFormat::DEPTH32F;
        ci.in_width   = static_cast<uint32_t>(src_w);
        ci.in_height  = static_cast<uint32_t>(src_h);
        ci.up_width   = static_cast<uint32_t>(dst_w);
        ci.up_height  = static_cast<uint32_t>(dst_h);
        ci.device_id  = nullptr; // let air_vk pick the first discrete GPU
        renderer_     = new air::AiRenderer(&ci);

        tex_color_ = renderer_->createGLTexture(air::TextureType::SRC_COLOR);
        set_tex_params(tex_color_, GL_LINEAR);

        if (algorithm_ == UpscaleAlgorithm::DLSS)
        {
            tex_depth_  = renderer_->createGLTexture(air::TextureType::SRC_DEPTH);
            tex_motion_ = renderer_->createGLTexture(air::TextureType::SRC_MOTION);
            set_tex_params(tex_depth_, GL_NEAREST);
            set_tex_params(tex_motion_, GL_NEAREST);
        }

        glGenFramebuffers(1, &fbo_);
        glBindFramebuffer(GL_FRAMEBUFFER, fbo_);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex_color_, 0);
        if (algorithm_ == UpscaleAlgorithm::DLSS)
        {
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT1, GL_TEXTURE_2D, tex_motion_, 0);
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,  GL_TEXTURE_2D, tex_depth_, 0);
        }
        glBindFramebuffer(GL_FRAMEBUFFER, 0);

        src_w_ = src_w; src_h_ = src_h; dst_w_ = dst_w; dst_h_ = dst_h;
        return renderer_ != nullptr;
    }

    static void set_tex_params(GLuint tex, GLint filter)
    {
        glBindTexture(GL_TEXTURE_2D, tex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glBindTexture(GL_TEXTURE_2D, 0);
    }

    void upload_depth_motion(const UpscaleInputs &in)
    {
        glBindTexture(GL_TEXTURE_2D, tex_depth_);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, in.src_w, in.src_h,
                        GL_DEPTH_COMPONENT, GL_FLOAT, in.depth);

        // motion is float32 vec2; convert to half for the RG16F motion texture.
        const std::size_t n = std::size_t(in.src_w) * in.src_h;
        motion_half_.resize(n * 2);
        for (std::size_t i = 0; i < n * 2; ++i)
        {
            motion_half_[i] = f32_to_f16(in.motion[i]);
        }
        glBindTexture(GL_TEXTURE_2D, tex_motion_);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, in.src_w, in.src_h,
                        GL_RG, GL_HALF_FLOAT, motion_half_.data());
    }

    void readback(std::vector<std::uint8_t> &dst, int dst_w, int dst_h)
    {
        dst.assign(std::size_t(dst_w) * dst_h * 4, 0);
        const GLuint up_tex = renderer_->getUpscaledGLTexture(air::ImageType::COLOR);
        glBindTexture(GL_TEXTURE_2D, up_tex);
        glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, dst.data());
        glBindTexture(GL_TEXTURE_2D, 0);
    }

    void fallback(const UpscaleInputs &in,
                  std::vector<std::uint8_t> &dst, int dst_w, int dst_h)
    {
        if (!warned_fallback_)
        {
            ASCENT_LOG_INFO("anari upscale(gpu): GPU path unavailable; using CPU bilinear");
            warned_fallback_ = true;
        }
        if (!cpu_)
        {
            cpu_ = make_cpu_bilinear_upscaler();
        }
        cpu_->upscale(in, dst, dst_w, dst_h);
    }

    // IEEE-754 float32 -> float16 (round toward zero on mantissa). Matches the
    // reference anari-ippl motion-vector upload convention.
    static std::uint16_t f32_to_f16(float f)
    {
        std::uint32_t x;
        std::memcpy(&x, &f, sizeof(x));
        const std::uint32_t sign = (x >> 16) & 0x8000u;
        std::int32_t        exp  = std::int32_t((x >> 23) & 0xff) - 127 + 15;
        const std::uint32_t man  = x & 0x7fffffu;
        if (exp >= 31) return std::uint16_t(sign | 0x7c00u | (man ? 1u : 0u));
        if (exp <= 0)  return std::uint16_t(sign);
        return std::uint16_t(sign | (std::uint32_t(exp) << 10) | (man >> 13));
    }

    UpscaleAlgorithm            algorithm_;
    EglContext                  egl_;
    air::AiRenderer            *renderer_{nullptr};
    GLuint                      tex_color_{0};
    GLuint                      tex_depth_{0};
    GLuint                      tex_motion_{0};
    GLuint                      fbo_{0};
    int                         src_w_{0}, src_h_{0}, dst_w_{0}, dst_h_{0};
    std::vector<std::uint16_t>  motion_half_;
    std::unique_ptr<Upscaler>   cpu_;
    bool                        warned_fallback_{false};
    std::mutex                  mutex_;
};

} // namespace (anonymous)

//-----------------------------------------------------------------------------
std::unique_ptr<Upscaler>
make_upscaler(const UpscaleConfig &cfg)
{
    switch (cfg.algorithm)
    {
        case UpscaleAlgorithm::Bilinear:
            return make_cpu_bilinear_upscaler();
        case UpscaleAlgorithm::FSR1:
        case UpscaleAlgorithm::DLSS:
            return std::make_unique<GpuUpscaler>(cfg.algorithm);
        case UpscaleAlgorithm::None:
        default:
            return nullptr;
    }
}

}}} // namespace ascent::runtime::filters

//-----------------------------------------------------------------------------
#else // !ASCENT_AIRENDER_ENABLED

namespace ascent { namespace runtime { namespace filters {

std::unique_ptr<Upscaler>
make_upscaler(const UpscaleConfig &cfg)
{
    switch (cfg.algorithm)
    {
        case UpscaleAlgorithm::Bilinear:
            return make_cpu_bilinear_upscaler();
        case UpscaleAlgorithm::FSR1:
        case UpscaleAlgorithm::DLSS:
            ASCENT_LOG_INFO("anari upscale: built without airender; "
                            "FSR1/DLSS fall back to CPU bilinear");
            return make_cpu_bilinear_upscaler();
        case UpscaleAlgorithm::None:
        default:
            return nullptr;
    }
}

}}} // namespace ascent::runtime::filters

#endif // ASCENT_AIRENDER_ENABLED
