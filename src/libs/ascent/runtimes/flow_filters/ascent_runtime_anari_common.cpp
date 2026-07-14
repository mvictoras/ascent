//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~//
// Copyright (c) Lawrence Livermore National Security, LLC and other Ascent
// Project developers. See top-level LICENSE AND COPYRIGHT files for dates and
// other details. No copyright assignment is required to contribute to Ascent.
//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~//

#include "ascent_runtime_anari_common.hpp"

#include <ascent_logging.hpp>
#include <ascent_metadata.hpp>
#include <ascent_runtime_param_check.hpp>
#include <ascent_runtime_utils.hpp>
#include <ascent_string_utils.hpp>

#include <ascent_runtime_conduit_to_viskores_parsing.hpp>
#include <ascent_runtime_vtkh_utils.hpp>

#include <runtimes/ascent_data_object.hpp>

#include <flow_filter.hpp>
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

#include <cstdio>
#include <cstdlib>

namespace anari_cpp = anari;

using conduit::Node;
using flow::Filter;
using viskores::interop::anari::ANARIMapper;
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
    std::vector<std::string> valid_paths;
    std::vector<std::string> ignore_paths;

    res &= check_string ("field",       params, info, true);
    res &= check_image_names(params, info);
    res &= check_numeric("min_value",   params, info, false);
    res &= check_numeric("max_value",   params, info, false);
    res &= check_numeric("image_width", params, info, false);
    res &= check_numeric("image_height",params, info, false);

    valid_paths = {
        "field", "image_prefix",
        "min_value", "max_value",
        "image_width", "image_height",
        "camera/look_at",   "camera/position",  "camera/up",
        "camera/fov",       "camera/xpan",      "camera/ypan",
        "camera/zoom",      "camera/near_plane","camera/far_plane",
        "camera/azimuth",   "camera/elevation",
    };
    ignore_paths.push_back("color_table");

    std::string surprises = surprise_check(valid_paths, ignore_paths, params);

    if (params.has_path("color_table"))
    {
        surprises += filters::detail::check_color_table_surprises(params["color_table"]);
    }

    if (!surprises.empty())
    {
        info["errors"].append() = surprises;
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

//-----------------------------------------------------------------------------
void
AnariImpl::set_lights()
{
    for (auto &light : lights)
    {
        anari_cpp::release(device, light);
    }
    lights.clear();

    anari_cpp::Light sun = anari_cpp::newObject<anari_cpp::Light>(device, "directional");
    anari_cpp::setParameter(device, sun, "direction",       viskores::Vec3f_32(0.0f, -1.0f, 0.0f));
    anari_cpp::setParameter(device, sun, "irradiance",      2.0f);
    anari_cpp::setParameter(device, sun, "angularDiameter", 0.00925f);
    anari_cpp::setParameter(device, sun, "radiance",        1.0f);
    anari_cpp::commitParameters(device, sun);
    lights.push_back(sun);
}

//-----------------------------------------------------------------------------
namespace
{

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
AnariImpl::render(ANARIScene &scene)
{
    anari_cpp::setParameter(device, renderer, "background",      background);
    anari_cpp::setParameter(device, renderer, "pixelSamples",    pixel_samples);
    anari_cpp::setParameter(device, renderer, "ambientRadiance", 0.8f);
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
    anari_cpp::commitParameters(device, camera);

    auto world = scene.GetANARIWorld();
    anari_cpp::setAndReleaseParameter(device, world, "light",
        anari_cpp::newArray1D(device, lights.data(), lights.size()));
    anari_cpp::commitParameters(device, world);

    anari_cpp::setParameter(device, frame, "size",          img_size);
    anari_cpp::setParameter(device, frame, "channel.color", ANARI_UFIXED8_VEC4);
    anari_cpp::setParameter(device, frame, "world",         world);
    anari_cpp::setParameter(device, frame, "camera",        camera);
    anari_cpp::setParameter(device, frame, "renderer",      renderer);
    anari_cpp::commitParameters(device, frame);

    anari_cpp::render(device, frame);
    anari_cpp::wait(device, frame);

    int rank = 0;
#ifdef ASCENT_MPI_ENABLED
    rank = viskores::cont::EnvironmentTracker::GetCommunicator().rank();
#endif
    if (rank == 0)
    {
        const auto fb = anari_cpp::map<uint32_t>(device, frame, "channel.color");
        ascent::PNGEncoder encoder;
        encoder.Encode(reinterpret_cast<const unsigned char*>(fb.data), fb.width, fb.height);
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
    Node meta = Metadata::n_metadata;
    int cycle = meta.has_path("cycle") ? meta["cycle"].to_int32() : 0;

    self.field_name = params["field"].as_string();

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

    std::string image_name = params["image_prefix"].as_string();
    image_name = expand_family_name(image_name, cycle);
    image_name = output_dir(image_name);
    self.img_name = image_name;

    int image_width  = 0;
    int image_height = 0;
    parse_image_dims(params, image_width, image_height);
    self.img_size = viskores::Vec2ui_32(image_width, image_height);
}

}}} // namespace ascent::runtime::filters
