//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~//
// Copyright (c) Lawrence Livermore National Security, LLC and other Ascent
// Project developers. See top-level LICENSE AND COPYRIGHT files for dates and
// other details. No copyright assignment is required to contribute to Ascent.
//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~//

#include "ascent_runtime_anari_common.hpp"
#include "ascent_runtime_anari_upscale.hpp"

#include <chrono>
#include <cstring>
#include <iostream>

#include <viskores/Matrix.h>

#include <ascent_logging.hpp>
#include <ascent_string_utils.hpp>

#include <ascent_runtime_conduit_to_viskores_parsing.hpp>

#include <runtimes/ascent_data_object.hpp>

// flow_filter.hpp is transitively included via ascent_runtime_anari_common.hpp
#include <flow_graph.hpp>
#include <flow_workspace.hpp>

#include <viskores/cont/EnvironmentTracker.h>
#include <viskores/interop/anari/ANARIMapperTriangles.h>
#include <viskores/interop/anari/ANARIMapperGlyphs.h>
#include <viskores/interop/anari/ANARIMapperVolume.h>

#include <png_utils/ascent_png_encoder.hpp>

#ifdef ASCENT_MPI_ENABLED
#include <mpi.h>
#endif

#include <cmath>
#include <cstdio>
#include <cstdlib>

// Note: `namespace anari_cpp = anari` is already declared inside viskores'
// ViskoresANARITypes.h (transitively pulled via ANARIScene.h in our header),
// so we intentionally do NOT redeclare the alias here \u2014 doing so triggers
// a "namespace alias conflicts with previous declaration" error.

using viskores::interop::anari::ANARIMapper;
using viskores::interop::anari::ANARIMapperGlyphs;
using viskores::interop::anari::ANARIMapperTriangles;
using viskores::interop::anari::ANARIMapperVolume;
using viskores::interop::anari::ANARIScene;

//-----------------------------------------------------------------------------
namespace ascent   { namespace runtime { namespace filters { namespace anari_detail
{

namespace
{

/// ANARI status callback: routes device-side diagnostics to stderr when
/// $VTKM_ANARI_VERBOSE is set. Suppressed by default so silent runs stay silent.
void status_func(const void *user_data,
                 ANARIDevice /*device*/,
                 ANARIObject source,
                 ANARIDataType /*source_type*/,
                 ANARIStatusSeverity severity,
                 ANARIStatusCode /*code*/,
                 const char *message)
{
    const bool verbose = *static_cast<const bool*>(user_data);
    if (!verbose)
    {
        return;
    }

    const char *tag = nullptr;
    switch (severity)
    {
        case ANARI_SEVERITY_FATAL_ERROR:        tag = "FATAL"; break;
        case ANARI_SEVERITY_ERROR:              tag = "ERROR"; break;
        case ANARI_SEVERITY_WARNING:            tag = "WARN "; break;
        case ANARI_SEVERITY_PERFORMANCE_WARNING: tag = "PERF "; break;
        case ANARI_SEVERITY_INFO:               tag = "INFO "; break;
        case ANARI_SEVERITY_DEBUG:              tag = "DEBUG"; break;
        default:                                tag = "?????"; break;
    }
    std::fprintf(stderr, "[%s][%p] %s\n", tag, source, message);
}

const char* resolve_library_name()
{
    if (const char *v = std::getenv("ANARI_LIBRARY"))           return v;
    if (const char *v = std::getenv("VTKM_ANARI_LIBRARY"))      return v;
    if (const char *v = std::getenv("VTKM_TEST_ANARI_LIBRARY")) return v;
    return "helide";
}

bool check_image_names(const conduit::Node &params, conduit::Node &info)
{
    const bool has_prefix = params.has_path("image_prefix");
    const bool has_cinema = params.has_path("camera/db_name");
    if (!has_prefix && !has_cinema)
    {
        info.append() = "Anari filters require either 'image_prefix' "
                        "(single image) or 'camera/db_name' (cinema).";
        return false;
    }
    if (has_prefix && has_cinema)
    {
        info.append() = "Anari filters cannot use both 'image_prefix' "
                        "and 'camera/db_name'.";
        return false;
    }
    return true;
}

} // namespace (anonymous)

//-----------------------------------------------------------------------------
anari::Device
load_device()
{
    static bool verbose  = std::getenv("VTKM_ANARI_VERBOSE") != nullptr;
    static bool debug    = std::getenv("VTKM_ANARI_DEBUG_DEVICE") != nullptr;
    const char *trace_dir = std::getenv("VTKM_ANARI_DEBUG_TRACE_DIR");

    auto lib = anari_cpp::loadLibrary(resolve_library_name(), status_func, &verbose);
    auto dev = anari_cpp::newDevice(lib, "default");
    anari_cpp::unloadLibrary(lib);

    if (debug)
    {
        auto dbg_lib = anari_cpp::loadLibrary("debug", status_func, &verbose);
        anari::Device dbg = anariNewDevice(dbg_lib, "debug");
        anari::setParameter(dbg, dbg, "wrappedDevice", dev);
        if (trace_dir != nullptr)
        {
            anari::setParameter(dbg, dbg, "traceDir", trace_dir);
            anari::setParameter(dbg, dbg, "traceMode", "code");
        }
        anari::commitParameters(dbg, dbg);
        anari::release(dev, dev);
        dev = dbg;
        anari_cpp::unloadLibrary(dbg_lib);
    }

    return dev;
}

//-----------------------------------------------------------------------------
bool
verify_params(const conduit::Node &params, conduit::Node &info)
{
    info.reset();

    bool res = true;
    // Two mutually exclusive forms: a single 'field', or a 'plots' map whose
    // children each carry their own field (and optional color_table/range).
    const bool has_plots = params.has_path("plots") &&
                           params["plots"].number_of_children() > 0;
    if (has_plots)
    {
        const conduit::Node &plots = params["plots"];
        for (conduit::index_t i = 0; i < plots.number_of_children(); ++i)
        {
            if (!plots.child(i).has_path("field"))
            {
                info["errors"].append() =
                    "anari plot '" + plots.child_names()[i] + "' requires a 'field' param";
                res = false;
            }
        }
    }
    else if (!params.has_path("field"))
    {
        info["errors"].append() =
            "anari filters require either a 'field' param or a non-empty 'plots' map";
        res = false;
    }
    if (!check_image_names(params, info))
    {
        res = false;
    }
    return res;
}

//-----------------------------------------------------------------------------
vtkh::DataSet*
extract_topology(::flow::Filter *filter,
                 int expected_components,
                 const char *filter_kind,
                 std::string &out_field_name,
                 viskores::Bounds &out_bounds)
{
    if (!filter->input(0).check_type<DataObject>())
    {
        ASCENT_ERROR("Anari " << filter_kind << " input must be a DataObject");
    }

    DataObject *d_input = filter->input<DataObject>(0);
    if (!d_input->is_valid())
    {
        return nullptr;
    }

    VTKHCollection *collection = d_input->as_vtkh_collection().get();
    const std::vector<std::string> topos = collection->topology_names();
    if (topos.size() != 1)
    {
        ASCENT_ERROR("Anari " << filter_kind << " accepts only one topology, "
                     << "got " << topos.size());
    }

    vtkh::DataSet &topo = collection->dataset_by_topology(topos[0]);

    out_field_name = filter->params()["field"].as_string();
    if (topo.NumberOfComponents(out_field_name) != expected_components)
    {
        ASCENT_ERROR("Anari " << filter_kind << " requires a field with "
                     << expected_components << " component(s); '"
                     << out_field_name << "' has "
                     << topo.NumberOfComponents(out_field_name));
    }

    out_bounds = collection->global_bounds();
    return &topo;
}

}}}} // namespace ascent::runtime::filters::anari_detail

//-----------------------------------------------------------------------------
namespace ascent   { namespace runtime { namespace filters
{

using anari_detail::load_device;

//-----------------------------------------------------------------------------
AnariImpl::AnariImpl()
    : device(load_device())
    , renderer(anari_cpp::newObject<anari_cpp::Renderer>(device, "default"))
    , frame   (anari_cpp::newObject<anari_cpp::Frame>(device))
{
    set_lights();
}

AnariImpl::~AnariImpl()
{
    for (auto &light : lights)
    {
        anari_cpp::release(device, light);
    }
    anari_cpp::release(device, frame);
    anari_cpp::release(device, renderer);
    anari_cpp::release(device, device);
}

//-----------------------------------------------------------------------------
void
AnariImpl::set_tfn(ANARIMapper &mapper)
{
    constexpr int resolution                            = 256;
    constexpr viskores::Float32 conversion_to_float01   = 1.0f / 255.0f;

    viskores::cont::ArrayHandle<viskores::Vec4ui_8> sampled;
    {
        viskores::cont::ScopedRuntimeDeviceTracker tracker(
            viskores::cont::DeviceAdapterTagSerial{});
        tfn.Sample(resolution, sampled);
    }
    auto color_portal = sampled.ReadPortal();

    auto color_array   = anari_cpp::newArray1D(device, ANARI_FLOAT32_VEC3, resolution);
    auto opacity_array = anari_cpp::newArray1D(device, ANARI_FLOAT32,      resolution);
    auto *colors    = anari_cpp::map<viskores::Vec3f_32>(device, color_array);
    auto *opacities = anari_cpp::map<viskores::Float32> (device, opacity_array);
    for (viskores::Id i = 0; i < resolution; ++i)
    {
        auto rgba    = color_portal.Get(i);
        colors[i]    = viskores::Vec3f_32(rgba[0], rgba[1], rgba[2]) * conversion_to_float01;
        opacities[i] = rgba[3] * conversion_to_float01;
    }
    anari_cpp::unmap(device, color_array);
    anari_cpp::unmap(device, opacity_array);

    mapper.SetANARIColorMap(color_array, opacity_array, /*shared=*/true);

    const viskores::Range range = scalar_range.IsNonEmpty()
        ? scalar_range
        : tfn.GetRange();
    mapper.SetANARIColorMapValueRange(viskores::Vec2f_32(range.Min, range.Max));
    mapper.SetANARIColorMapOpacityScale(1.0f);
}

namespace
{
// A YAML sequence reaches conduit either as child nodes or as a contiguous
// numeric array. child(i) throws on the array form, which silently aborts the
// whole extract, so every vector param must go through this.
viskores::Vec3f_32 node_to_vec3(const conduit::Node &n,
                                const viskores::Vec3f_32 &fallback)
{
    float v[3] = {fallback[0], fallback[1], fallback[2]};
    if (n.number_of_children() > 0)
    {
        for (conduit::index_t i = 0; i < n.number_of_children() && i < 3; ++i)
            v[i] = n.child(i).to_float32();
    }
    else if (n.dtype().number_of_elements() > 0)
    {
        conduit::Node tmp;
        n.to_float32_array(tmp);
        const conduit::float32 *a = tmp.value();
        for (conduit::index_t i = 0; i < tmp.dtype().number_of_elements() && i < 3; ++i)
            v[i] = a[i];
    }
    return viskores::Vec3f_32(v[0], v[1], v[2]);
}
} // namespace

//-----------------------------------------------------------------------------
void
AnariImpl::set_lights()
{
    for (auto &light : lights)
    {
        anari_cpp::release(device, light);
    }
    lights.clear();

    // No `lights` in the YAML: keep the historical single directional light so
    // existing actions files render identically.
    if (light_spec.number_of_children() == 0)
    {
        anari_cpp::Light sun = anari_cpp::newObject<anari_cpp::Light>(device, "directional");
        anari_cpp::setParameter(device, sun, "direction",  viskores::Vec3f_32(0.0f, -1.0f, 0.0f));
        anari_cpp::setParameter(device, sun, "irradiance", 2.0f);
        anari_cpp::setParameter(device, sun, "radiance",   1.0f);
        anari_cpp::commitParameters(device, sun);
        lights.push_back(sun);
        return;
    }

    for (conduit::index_t i = 0; i < light_spec.number_of_children(); ++i)
    {
        const conduit::Node &l = light_spec.child(i);
        const std::string name = light_spec.child_names()[i];
        const std::string type = l.has_path("type") ? l["type"].as_string()
                                                    : std::string("directional");

        // Barney implements exactly these three; anything else silently
        // becomes an inert UnknownObject, so reject it loudly instead.
        if (type != "directional" && type != "point" && type != "hdri")
        {
            ASCENT_ERROR("anari light '" << name << "': unsupported type '"
                         << type << "'. Barney supports directional, point, hdri.");
        }

        anari_cpp::Light lt = anari_cpp::newObject<anari_cpp::Light>(device, type.c_str());

        if (l.has_path("color"))
        {
            anari_cpp::setParameter(device, lt, "color",
                node_to_vec3(l["color"], viskores::Vec3f_32(1.f, 1.f, 1.f)));
        }

        auto set_vec3 = [&](const char *key)
        {
            if (!l.has_path(key)) return;
            anari_cpp::setParameter(device, lt, key,
                node_to_vec3(l[key], viskores::Vec3f_32(0.f, 0.f, 0.f)));
        };
        auto set_f32 = [&](const char *key)
        {
            if (l.has_path(key))
            {
                anari_cpp::setParameter(device, lt, key, l[key].to_float32());
            }
        };

        if (type == "directional")
        {
            set_vec3("direction");
            set_f32("irradiance");
            set_f32("radiance");
        }
        else if (type == "point")
        {
            set_vec3("position");
            set_f32("intensity");
            set_f32("power");
        }
        else
        {
            set_vec3("direction");
            set_vec3("up");
            set_f32("scale");
        }

        anari_cpp::commitParameters(device, lt);
        lights.push_back(lt);
    }
}

//-----------------------------------------------------------------------------
namespace
{

/// DLSS sub-pixel camera jitter: the radical-inverse Halton sequence (bases 2
/// and 3), remapped from [0,1) to [-0.5,0.5) pixel offsets. index is the frame
/// counter. DLSS accumulates detail across frames only if each frame is jittered
/// by this low-discrepancy sequence.
void halton_jitter(unsigned index, float &jx, float &jy)
{
    auto radical_inverse = [](unsigned i, unsigned base) {
        float f = 1.0f, r = 0.0f;
        while (i > 0)
        {
            f /= base;
            r += f * (i % base);
            i /= base;
        }
        return r;
    };
    // NVIDIA specifies base_phases * scale^2 phases; 8 * 2^2 = 32 for a 2x
    // upscale. Without wrapping, the sequence never repeats and DLSS keeps
    // being handed new sample positions instead of a fixed cycling set.
    constexpr unsigned phase_count = 32;
    const unsigned phase = index % phase_count;
    // Halton is 1-based; phase+1 avoids the degenerate (0,0) sample.
    jx = radical_inverse(phase + 1, 2) - 0.5f;
    jy = radical_inverse(phase + 1, 3) - 0.5f;
}

/// Compute the scalar range for `field_name` on `dset` when the user did
/// not supply min_value/max_value. `expected_components` is 1 for scalar
/// fields (triangles/volume) or 3 for vector fields (glyphs).
void ensure_scalar_range(viskores::Range &range,
                         vtkh::DataSet &dset,
                         const std::string &field_name,
                         int expected_components,
                         const char *filter_kind)
{
    if (range.IsNonEmpty())
    {
        return;
    }
    auto ranges = dset.GetGlobalRange(field_name);
    const auto count = ranges.GetNumberOfValues();
    if (count != expected_components)
    {
        ASCENT_ERROR("Anari " << filter_kind
                     << " expected a field with " << expected_components
                     << " component(s); got " << count);
    }
    auto portal = ranges.ReadPortal();
    for (viskores::Id cc = 0; cc < count; ++cc)
    {
        range.Include(portal.Get(cc));
    }
}

} // namespace (anonymous)

//-----------------------------------------------------------------------------
void
AnariImpl::render_triangles(vtkh::DataSet &dset)
{
    ensure_scalar_range(scalar_range, dset, field_name, /*expected=*/1, "Pseudocolor");

    ANARIScene scene(device);
    for (int i = 0; i < dset.GetNumberOfDomains(); ++i)
    {
        auto &m = scene.AddMapper(viskores::interop::anari::ANARIMapperTriangles(device));
        m.SetName(("triangles_" + std::to_string(i)).c_str());
        m.SetActor({
            dset.GetDomain(i).GetCellSet(),
            dset.GetDomain(i).GetCoordinateSystem(),
            dset.GetDomain(i).GetField(field_name),
        });
        m.SetCalculateNormals(true);
        set_tfn(m);
    }
    render(scene);
}

//-----------------------------------------------------------------------------
void
AnariImpl::render_glyphs(vtkh::DataSet &dset)
{
    ensure_scalar_range(scalar_range, dset, field_name, /*expected=*/3, "Glyphs");

    ANARIScene scene(device);
    for (int i = 0; i < dset.GetNumberOfDomains(); ++i)
    {
        auto &m = scene.AddMapper(viskores::interop::anari::ANARIMapperGlyphs(device));
        m.SetName(("glyphs_" + std::to_string(i)).c_str());
        m.SetActor({
            dset.GetDomain(i).GetCellSet(),
            dset.GetDomain(i).GetCoordinateSystem(),
            dset.GetDomain(i).GetField(field_name),
        });
        set_tfn(m);
    }
    render(scene);
}

//-----------------------------------------------------------------------------
void
AnariImpl::render_volume(vtkh::DataSet &dset)
{
    ensure_scalar_range(scalar_range, dset, field_name, /*expected=*/1, "Volume");

    ANARIScene scene(device);
    for (int i = 0; i < dset.GetNumberOfDomains(); ++i)
    {
        auto &m = scene.AddMapper(viskores::interop::anari::ANARIMapperVolume(device));
        m.SetName(("volume_" + std::to_string(i)).c_str());
        m.SetActor({
            dset.GetDomain(i).GetCellSet(),
            dset.GetDomain(i).GetCoordinateSystem(),
            dset.GetDomain(i).GetField(field_name),
        });
        set_tfn(m);
    }
    render(scene);
}

//-----------------------------------------------------------------------------
void
AnariImpl::ensure_scene()
{
    if (!scene)
    {
        scene.reset(new ANARIScene(device));
    }
}

//-----------------------------------------------------------------------------
void
AnariImpl::add_plot_triangles(vtkh::DataSet &dset, const std::string &tag)
{
    ensure_scalar_range(scalar_range, dset, field_name, /*expected=*/1, "Pseudocolor");
    ensure_scene();

    for (int i = 0; i < dset.GetNumberOfDomains(); ++i)
    {
        auto &m = scene->AddMapper(ANARIMapperTriangles(device));
        m.SetName((tag + "_triangles_" + std::to_string(i)).c_str());
        m.SetActor({
            dset.GetDomain(i).GetCellSet(),
            dset.GetDomain(i).GetCoordinateSystem(),
            dset.GetDomain(i).GetField(field_name),
        });
        m.SetCalculateNormals(true);
        set_tfn(m);
    }
}

//-----------------------------------------------------------------------------
void
AnariImpl::add_plot_glyphs(vtkh::DataSet &dset, const std::string &tag)
{
    ensure_scalar_range(scalar_range, dset, field_name, /*expected=*/3, "Glyphs");
    ensure_scene();

    for (int i = 0; i < dset.GetNumberOfDomains(); ++i)
    {
        auto &m = scene->AddMapper(ANARIMapperGlyphs(device));
        m.SetName((tag + "_glyphs_" + std::to_string(i)).c_str());
        m.SetActor({
            dset.GetDomain(i).GetCellSet(),
            dset.GetDomain(i).GetCoordinateSystem(),
            dset.GetDomain(i).GetField(field_name),
        });
        set_tfn(m);
    }
}

//-----------------------------------------------------------------------------
void
AnariImpl::add_plot_volume(vtkh::DataSet &dset, const std::string &tag)
{
    ensure_scalar_range(scalar_range, dset, field_name, /*expected=*/1, "Volume");
    ensure_scene();

    for (int i = 0; i < dset.GetNumberOfDomains(); ++i)
    {
        auto &m = scene->AddMapper(ANARIMapperVolume(device));
        m.SetName((tag + "_volume_" + std::to_string(i)).c_str());
        m.SetActor({
            dset.GetDomain(i).GetCellSet(),
            dset.GetDomain(i).GetCoordinateSystem(),
            dset.GetDomain(i).GetField(field_name),
        });
        set_tfn(m);
    }
}

//-----------------------------------------------------------------------------
void
AnariImpl::render_scene()
{
    ensure_scene();
    render(*scene);

    // Mappers hold references to this cycle's datasets, which are freed once
    // the pipelines re-execute. Drop the scene so the next frame rebuilds it.
    scene.reset();
}

//-----------------------------------------------------------------------------
void
AnariImpl::render(ANARIScene &scene)
{
    anari_cpp::setParameter(device, renderer, "background",      background);
    anari_cpp::setParameter(device, renderer, "pixelSamples",    pixel_samples);
    anari_cpp::setParameter(device, renderer, "ambientRadiance", ambient_radiance);
    // Barney's ANARI renderer defaults denoise to true and runs its OptiX 8
    // denoiser (barney/src/anari/Renderer.cpp:21, FrameBuffer.cu:275). Setting
    // it explicitly makes the YAML authoritative either way.
    anari_cpp::setParameter(device, renderer, "denoise", denoise);
    anari_cpp::commitParameters(device, renderer);

    // Camera. TODO: xpan/ypan via imageRegion; correct zoom semantics.
    const auto  cam_zoom  = cam.GetZoom();
    const char *cam_type  = cam.GetMode() == viskores::rendering::Camera::Mode::ThreeD
                                ? "perspective" : "orthographic";
    const auto  cam_dir   = cam.GetLookAt() - cam.GetPosition();
    const auto  cam_pos   = cam_zoom > 0
                                ? cam.GetLookAt() - cam_dir / cam_zoom
                                : cam.GetPosition();
    const auto  cam_up    = cam.GetViewUp();
    const auto  cam_range = cam.GetClippingRange();

    // Advance the jitter here, before both the motion matrices and the camera
    // direction, so the two are guaranteed to describe the same sample offset.
    const bool jitter_active =
        upscale.enabled() && upscale.algorithm == UpscaleAlgorithm::DLSS;
    float jitter_px_x = 0.0f, jitter_px_y = 0.0f;
    if (jitter_active)
    {
        AnariCameraMotion &m = anari_camera_motion(img_prefix);
        halton_jitter(m.jitter_seq++, m.jitter_x, m.jitter_y);
        if (const char *fixed = getenv("ASCENT_ANARI_FIXED_JITTER"))
        {
            float fx = 0.0f, fy = 0.0f;
            if (std::sscanf(fixed, "%f,%f", &fx, &fy) == 2)
            {
                m.jitter_x = fx;
                m.jitter_y = fy;
            }
        }
        jitter_px_x = m.jitter_x;
        jitter_px_y = m.jitter_y;
    }

    anari_cpp::Camera camera = anari_cpp::newObject<anari_cpp::Camera>(device, cam_type);
    anari_cpp::setParameter(device, camera, "aspect",    img_size[0] / float(img_size[1]));
    anari_cpp::setParameter(device, camera, "position",  cam_pos);
    anari_cpp::setParameter(device, camera, "direction", cam_dir);
    anari_cpp::setParameter(device, camera, "up",        cam_up);
    anari_cpp::setParameter(device, camera, "near",      cam_range.Min);
    anari_cpp::setParameter(device, camera, "far",       cam_range.Max);
    if (std::string(cam_type) == "perspective")
    {
        anari_cpp::setParameter(device, camera, "fov",
                                cam.GetFieldOfView() / 180.0 * viskores::Pi());
    }
    else
    {
        anari_cpp::setParameter(device, camera, "height",
                                cam.GetXScale() / img_size[0] * img_size[1]);
    }

    // Feed Barney the current and previous view-projection so it can write
    // channel.motion. Viskores matrices are row-major and ANARI mat4 is
    // column-major, hence the transpose. On the first frame there is no
    // previous matrix, so motion is left disabled and the upscaler falls back
    // to spatial-only for that frame.
    {
        const auto view = cam.CreateViewMatrix();
        const auto proj = cam.CreateProjectionMatrix(img_size[0], img_size[1]);
        const auto vp   = viskores::MatrixMultiply(proj, view);

        float curr[16];
        for (int c = 0; c < 4; ++c)
            for (int r = 0; r < 4; ++r)
                curr[c * 4 + r] = static_cast<float>(vp[r][c]);

        // Key on the unexpanded prefix: img_name has the frame number already
        // substituted, so using it would allocate a fresh entry every frame and
        // has_prev would never become true.
        AnariCameraMotion &motion = anari_camera_motion(img_prefix);

        if (motion.has_prev)
        {
            anari_cpp::setParameter(device, camera,
                                    "motion.viewProjection",
                                    ANARI_FLOAT32_MAT4, curr);
            anari_cpp::setParameter(device, camera,
                                    "motion.previousViewProjection",
                                    ANARI_FLOAT32_MAT4, motion.prev_view_proj);
        }
        std::memcpy(motion.prev_view_proj, curr, sizeof(curr));
        motion.has_prev = true;
    }

    // Sub-pixel jitter. Barney's ANARI camera forwards imageRegion's offset
    // to its imageJitter param, which generateRays applies per pixel. The
    // value is in PIXELS ([-0.5,0.5] per NVIDIA DLSS guide 3.7.3); Barney
    // divides by the resolution itself, so do NOT pre-normalise here.
    if (jitter_active)
    {
        // DLSS screen space is +Y down; Barney's dir_dv is +Y up.
        float ysign = 1.0f;
        if (const char *ys = getenv("ASCENT_ANARI_JITTER_YSIGN"))
            ysign = (ys[0] == '-') ? -1.0f : 1.0f;
        const float jx = jitter_px_x;
        const float jy = ysign * jitter_px_y;
        const float region[4] = { jx, jy, 1.0f + jx, 1.0f + jy };
        anari_cpp::setParameter(device, camera, "imageRegion",
                                ANARI_FLOAT32_BOX2, region);
    }

    anari_cpp::commitParameters(device, camera);

    auto world = scene.GetANARIWorld();
    anari_cpp::setAndReleaseParameter(device, world, "light",
        anari_cpp::newArray1D(device, lights.data(), lights.size()));
    anari_cpp::commitParameters(device, world);

    anari_cpp::setParameter(device, frame, "size",          img_size);
    anari_cpp::setParameter(device, frame, "channel.color", ANARI_UFIXED8_VEC4);
    // DLSS needs per-pixel depth + screen-space motion vectors as additional
    // temporal-upsampling inputs. Request them from Barney only for that path
    // (bilinear/FSR1 are single-frame and use color alone).
    const bool want_dlss = upscale.enabled() && upscale.algorithm == UpscaleAlgorithm::DLSS;
    if (want_dlss)
    {
        anari_cpp::setParameter(device, frame, "channel.depth",  ANARI_FLOAT32);
        anari_cpp::setParameter(device, frame, "channel.motion", ANARI_FLOAT32_VEC2);
    }
    anari_cpp::setParameter(device, frame, "world",         world);
    anari_cpp::setParameter(device, frame, "camera",        camera);
    anari_cpp::setParameter(device, frame, "renderer",      renderer);
    anari_cpp::commitParameters(device, frame);

    // anari_cpp::wait blocks until the frame is complete, so this brackets the
    // real distributed render cost rather than just the async dispatch.
    const auto t_render_start = std::chrono::steady_clock::now();
    anari_cpp::render(device, frame);
    anari_cpp::wait(device, frame);
    anari_stage_timings().render +=
        std::chrono::duration<double>(std::chrono::steady_clock::now() - t_render_start).count();
    anari_stage_timings().render_calls++;

    int rank = 0;
#ifdef ASCENT_MPI_ENABLED
    rank = viskores::cont::EnvironmentTracker::GetCommunicator().rank();
#endif
    if (rank == 0)
    {
        const auto fb = anari_cpp::map<uint32_t>(device, frame, "channel.color");
        const auto *rgba = reinterpret_cast<const unsigned char*>(fb.data);

        ascent::PNGEncoder encoder;
        if (upscale.enabled())
        {
            const int up_w = int(std::lround(fb.width  * upscale.factor));
            const int up_h = int(std::lround(fb.height * upscale.factor));

            UpscaleInputs in;
            in.color = rgba;
            in.src_w = int(fb.width);
            in.src_h = int(fb.height);

            // DIAGNOSTIC: dump the exact color buffer handed to DLSS (this is
            // Barney's channel.color AFTER its internal denoise, i.e. the DLSS
            // INPUT). Compared against the DLSS output PNG, this shows what the
            // upscaler received vs produced.
            if (getenv("ASCENT_ANARI_DUMP_DLSS_INPUT"))
            {
                ascent::PNGEncoder pre;
                pre.Encode(const_cast<unsigned char*>(rgba), fb.width, fb.height);
                pre.Save(img_name + "_dlssINPUT.png");
            }

            unsigned shared_seq = 0;
            upscaler = shared_upscaler(upscale, in.src_w, in.src_h,
                                       up_w, up_h, shared_seq);

            anari_cpp::MappedFrameData<float> depth_map{}, motion_map{};
            if (want_dlss)
            {
                depth_map  = anari_cpp::map<float>(device, frame, "channel.depth");
                motion_map = anari_cpp::map<float>(device, frame, "channel.motion");

                // Barney writes world-space ray hit distance, and misses come
                // back as ~1e30. DLSS requires depth normalised to [0,1] with
                // near=0 and far=1 (Programming Guide 3.8). Feeding it raw
                // distances leaves every pixel far outside that range, so DLSS
                // cannot align history to the jittered sample grid - which is
                // why a varying jitter destabilised the image while a constant
                // one did not.
                depth_norm_.resize(std::size_t(fb.width) * fb.height);
                {
                    const float dnear = float(cam.GetClippingRange().Min);
                    const float dfar  = float(cam.GetClippingRange().Max);
                    const float inv   = (dfar > dnear) ? 1.0f / (dfar - dnear) : 0.0f;
                    for (std::size_t i = 0; i < depth_norm_.size(); ++i)
                    {
                        const float d = depth_map.data[i];
                        float n = (std::isfinite(d) ? (d - dnear) * inv : 1.0f);
                        depth_norm_[i] = std::min(1.0f, std::max(0.0f, n));
                    }
                }
                in.depth   = depth_norm_.data();
                in.motion  = motion_map.data;

                if (getenv("ASCENT_ANARI_DIAG_DEPTH") && in.depth)
                {
                    // NVIDIA requires depth in [0,1] (guide 3.8). Barney writes
                    // world-space hit_t, so confirm the actual range.
                    float dmin = 1e30f, dmax = -1e30f;
                    std::size_t n = std::size_t(fb.width) * fb.height, finite = 0;
                    for (std::size_t i = 0; i < n; ++i)
                    {
                        const float d = in.depth[i];
                        if (!std::isfinite(d)) continue;
                        ++finite;
                        dmin = std::min(dmin, d); dmax = std::max(dmax, d);
                    }
                    std::cerr << "[DEPTH_DIAG] finite=" << finite << "/" << n
                              << " min=" << dmin << " max=" << dmax << std::endl;
                }
                if (getenv("ASCENT_ANARI_DIAG_MOTION"))
                {
                    double sum = 0.0, peak = 0.0;
                    std::size_t nz = 0, n = std::size_t(fb.width) * fb.height;
                    for (std::size_t i = 0; in.motion && i < n; ++i)
                    {
                        const double mx = std::fabs(in.motion[2 * i]);
                        const double my = std::fabs(in.motion[2 * i + 1]);
                        if (mx > 0.0 || my > 0.0) ++nz;
                        sum += mx + my;
                        peak = std::max(peak, std::max(mx, my));
                    }
                    std::cerr << "[MV_DIAG] " << img_name
                              << " motion_ptr=" << (in.motion ? "yes" : "NULL")
                              << " nonzero=" << nz << "/" << n
                              << " mean=" << (n ? sum / (2 * n) : 0.0)
                              << " peak=" << peak << std::endl;
                }
                // Reuse the offset the camera was actually rendered with,
                // rather than recomputing from a different counter.
                const AnariCameraMotion &motion = anari_camera_motion(img_prefix);
                in.jitter_x = motion.jitter_x;
                in.jitter_y = motion.jitter_y;
            }

            std::vector<std::uint8_t> up_pixels;
            const auto t_upscale_start = std::chrono::steady_clock::now();
            upscaler->upscale(in, up_pixels, up_w, up_h);
            anari_stage_timings().upscale_total +=
                std::chrono::duration<double>(std::chrono::steady_clock::now() - t_upscale_start).count();
            anari_stage_timings().upscale_calls++;
            encoder.Encode(up_pixels.data(), up_w, up_h);

            if (want_dlss)
            {
                anari_cpp::unmap(device, frame, "channel.depth");
                anari_cpp::unmap(device, frame, "channel.motion");
            }
        }
        else
        {
            encoder.Encode(rgba, fb.width, fb.height);
        }
        encoder.Save(img_name + ".png");
        anari_cpp::unmap(device, frame, "channel.color");
    }

    anari_cpp::release(device, camera);
}

//-----------------------------------------------------------------------------
void
configure_from_params(AnariImpl &self,
                      const conduit::Node &params,
                      const viskores::Bounds &bounds)
{
    // Absent in the multi-plot form, where each plot supplies its own field.
    if (params.has_path("field"))
    {
        self.field_name = params["field"].as_string();
    }

    viskores::rendering::Camera camera;
    camera.ResetToBounds(bounds);
    if (params.has_path("camera"))
    {
        parse_camera(params["camera"], camera);
    }
    self.cam = camera;

    if (params.has_path("color_table"))
    {
        self.tfn = parse_color_table(params["color_table"]);
    }

    self.scalar_range = viskores::Range();
    if (params.has_path("min_value")) self.scalar_range.Min = params["min_value"].to_float64();
    if (params.has_path("max_value")) self.scalar_range.Max = params["max_value"].to_float64();

    int mpi_comm_id = -1;
#ifdef ASCENT_MPI_ENABLED
    mpi_comm_id = flow::Workspace::default_mpi_comm();
#endif
    self.img_prefix = params["image_prefix"].as_string();
    self.img_name = expand_path_special_variables(
        self.img_prefix, ".png", mpi_comm_id);

    int image_width  = 0;
    int image_height = 0;
    parse_image_dims(params, image_width, image_height);
    self.img_size = viskores::Vec2ui_32(image_width, image_height);

    if (params.has_path("samples"))
    {
        self.pixel_samples = params["samples"].to_int32();
    }

    if (params.has_path("denoise"))
    {
        // Conduit parses unquoted YAML true/false as STRINGS, and to_int32()
        // on "true"/"false" is 0 for both, so a numeric test silently never
        // fires. Accept the string form first, then genuine numbers.
        const conduit::Node &n = params["denoise"];
        if (n.dtype().is_string())
        {
            const std::string v = n.as_string();
            self.denoise = (v == "true" || v == "True" || v == "1");
        }
        else
        {
            self.denoise = n.to_int32() != 0;
        }
    }

    if (params.has_path("background"))
    {
        // Conduit may hold a YAML sequence either as child nodes or as a
        // contiguous numeric array, depending on whether the entries parsed
        // to a uniform type. Handle both; indexing children of an array node
        // throws and would abort the whole extract.
        const conduit::Node &b = params["background"];
        float rgba[4] = {0.0f, 0.0f, 0.0f, 1.0f};
        if (b.number_of_children() > 0)
        {
            for (conduit::index_t i = 0; i < b.number_of_children() && i < 4; ++i)
            {
                rgba[i] = b.child(i).to_float32();
            }
        }
        else if (b.dtype().number_of_elements() > 0)
        {
            conduit::Node tmp;
            b.to_float32_array(tmp);
            const conduit::float32 *v = tmp.value();
            for (conduit::index_t i = 0; i < tmp.dtype().number_of_elements() && i < 4; ++i)
            {
                rgba[i] = v[i];
            }
        }
        self.background = viskores::Vec4f_32(rgba[0], rgba[1], rgba[2], rgba[3]);
    }

    if (params.has_path("ambient_radiance"))
    {
        self.ambient_radiance = params["ambient_radiance"].to_float32();
    }

    self.light_spec.reset();
    if (params.has_path("lights"))
    {
        self.light_spec.set(params["lights"]);
    }

    self.upscale = UpscaleConfig{};
    if (params.has_path("upscale"))
    {
        const conduit::Node &up = params["upscale"];
        if (up.has_path("algorithm"))
        {
            self.upscale.algorithm = parse_upscale_algorithm(up["algorithm"].as_string());
        }
        if (up.has_path("factor"))
        {
            self.upscale.factor = up["factor"].to_float64();
        }
    }
}

}}} // namespace ascent::runtime::filters
