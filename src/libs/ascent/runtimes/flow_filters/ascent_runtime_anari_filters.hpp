//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~//
// Copyright (c) Lawrence Livermore National Security, LLC and other Ascent
// Project developers. See top-level LICENSE AND COPYRIGHT files for dates and
// other details. No copyright assignment is required to contribute to Ascent.
//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~//


//-----------------------------------------------------------------------------
///
/// file: ascent_runtime_anari_filters.hpp
///
//-----------------------------------------------------------------------------

#ifndef ASCENT_RUNTIME_ANARI_FILTERS
#define ASCENT_RUNTIME_ANARI_FILTERS

#include <ascent.hpp>

#include <flow_filter.hpp>

#include <memory>

//-----------------------------------------------------------------------------
// -- begin ascent:: --
//-----------------------------------------------------------------------------
namespace ascent
{

//-----------------------------------------------------------------------------
// -- begin ascent::runtime --
//-----------------------------------------------------------------------------
namespace runtime
{

//-----------------------------------------------------------------------------
// -- begin ascent::runtime::filters --
//-----------------------------------------------------------------------------
namespace filters
{

//-----------------------------------------------------------------------------
///
/// Anari Filters
///
//-----------------------------------------------------------------------------

struct AnariImpl;

//-----------------------------------------------------------------------------
/// YAML type: 'anari'. Renders surface geometry through ANARI.
/// Auto-detects mapper from the field's component count:
///   scalar field (1 component) -> triangles mapper (colored surface)
///   vector field (3 components) -> glyphs mapper (per-cell arrows/spheres)
///
/// The volumetric renderer is a separate filter (AnariVolume, type 'anari_volume')
/// because it takes a semantically different input (volumetric field over a 3D
/// domain) and has different sensible defaults (transfer function required,
/// higher default sample count, etc.).
class ASCENT_API AnariSurface : public ::flow::Filter
{
public:
    AnariSurface();
    virtual ~AnariSurface();

    virtual void   declare_interface(conduit::Node &i);
    virtual bool   verify_params(const conduit::Node &params,
                                 conduit::Node &info);
    virtual void   execute();

private:
    std::shared_ptr<AnariImpl> pimpl;
};

//-----------------------------------------------------------------------------
/// YAML type: 'anari_volume'. Renders volumetric fields through ANARI.
/// Requires a scalar (1-component) field defined over a 3D domain.
class ASCENT_API AnariVolume : public ::flow::Filter
{
public:
    AnariVolume();
    virtual ~AnariVolume();

    virtual void   declare_interface(conduit::Node &i);
    virtual bool   verify_params(const conduit::Node &params,
                                 conduit::Node &info);
    virtual void   execute();

private:
    std::shared_ptr<AnariImpl> pimpl;
};

};
//-----------------------------------------------------------------------------
// -- end ascent::runtime::filters --
//-----------------------------------------------------------------------------


//-----------------------------------------------------------------------------
};
//-----------------------------------------------------------------------------
// -- end ascent::runtime --
//-----------------------------------------------------------------------------


//-----------------------------------------------------------------------------
};
//-----------------------------------------------------------------------------
// -- end ascent:: --
//-----------------------------------------------------------------------------




#endif
//-----------------------------------------------------------------------------
// -- end header ifdef guard
//-----------------------------------------------------------------------------
