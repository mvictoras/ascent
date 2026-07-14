//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~//
// Copyright (c) Lawrence Livermore National Security, LLC and other Ascent
// Project developers. See top-level LICENSE AND COPYRIGHT files for dates and
// other details. No copyright assignment is required to contribute to Ascent.
//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~//

//-----------------------------------------------------------------------------
///
/// file: ascent_runtime_anari_common.hpp
///
/// Shared machinery for the ANARI-based flow filters
/// (ascent_runtime_anari_triangles/glyphs/volume.cpp).
///
/// AnariImpl owns per-filter ANARI device/renderer/frame state and provides
/// three render_* entry points corresponding to the three filter kinds.
///
//-----------------------------------------------------------------------------

#ifndef ASCENT_RUNTIME_ANARI_COMMON_HPP
#define ASCENT_RUNTIME_ANARI_COMMON_HPP

#include <conduit.hpp>

#include <flow_filter.hpp>

#include <ascent_vtkh_collection.hpp>
#include <vtkh/DataSet.hpp>
#include <viskores/Bounds.h>
#include <viskores/Range.h>
#include <viskores/cont/ColorTable.h>
#include <viskores/rendering/Camera.h>

#include <anari/anari_cpp.hpp>
#include <viskores/interop/anari/ANARIScene.h>

#include <memory>
#include <string>
#include <vector>

//-----------------------------------------------------------------------------
namespace ascent   { namespace runtime { namespace filters { namespace anari_detail
{

/// Load the ANARI device once per process (respects $ANARI_LIBRARY env var,
/// falls back to "helide" if unset). Optionally wraps with the debug device
/// when $VTKM_ANARI_DEBUG_DEVICE is set.
anari::Device load_device();

/// Common conduit-param validation shared by every anari_* filter.
/// Checks field, image_prefix|camera/db_name, min/max/width/height/camera/*,
/// color_table. Returns true if params are valid; populates info on failure.
bool verify_params(const conduit::Node &params, conduit::Node &info);

/// Common flow::Filter::execute() prelude: unwrap the input DataObject,
/// extract the single expected topology, validate the field's component count.
/// Returns nullptr if the input is invalid (caller should early-return).
///
/// expected_components: required field arity (1 for scalar, 3 for vector).
/// filter_kind: short label used in ASCENT_ERROR messages ("Volume", etc).
vtkh::DataSet* extract_topology(::flow::Filter *filter,
                                int expected_components,
                                const char *filter_kind,
                                std::string &out_field_name,
                                viskores::Bounds &out_bounds);

}}}} // namespace ascent::runtime::filters::anari_detail

//-----------------------------------------------------------------------------
namespace ascent   { namespace runtime { namespace filters
{

/// Per-filter ANARI state. Owns its device/renderer/frame; releases them
/// in the destructor. Not thread-safe. One AnariImpl per Filter instance.
struct AnariImpl
{
    AnariImpl();
    ~AnariImpl();

    AnariImpl(const AnariImpl&)            = delete;
    AnariImpl& operator=(const AnariImpl&) = delete;

    void set_tfn(viskores::interop::anari::ANARIMapper &mapper);
    void set_lights();

    void render_triangles(vtkh::DataSet &dset);
    void render_glyphs(vtkh::DataSet &dset);
    void render_volume(vtkh::DataSet &dset);
    void render(viskores::interop::anari::ANARIScene &scene);

    anari::Device                  device{};
    anari::Renderer                renderer{};
    anari::Frame                   frame{};
    std::vector<anari::Light>      lights;

    std::string                    field_name;
    viskores::Range                scalar_range;
    viskores::cont::ColorTable     tfn{viskores::cont::ColorTable("Cool to Warm")};

    viskores::rendering::Camera    cam;

    std::string                    img_name{"anari"};
    viskores::Vec2ui_32            img_size{1024, 768};

    viskores::Vec4f_32             background{0.0f, 0.0f, 0.0f, 0.0f};
    int                            pixel_samples{128};
};

/// Extract YAML render parameters onto an AnariImpl. Handles field, camera
/// (default fits `bounds`), color_table, min/max_value, image_prefix/width/height.
void configure_from_params(AnariImpl &self,
                           const conduit::Node &params,
                           const viskores::Bounds &bounds);

}}} // namespace ascent::runtime::filters

#endif // ASCENT_RUNTIME_ANARI_COMMON_HPP
