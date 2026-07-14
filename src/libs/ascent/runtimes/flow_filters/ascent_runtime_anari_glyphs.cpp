//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~//
// Copyright (c) Lawrence Livermore National Security, LLC and other Ascent
// Project developers. See top-level LICENSE AND COPYRIGHT files for dates and
// other details. No copyright assignment is required to contribute to Ascent.
//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~//

#include "ascent_runtime_anari_filters.hpp"
#include "ascent_runtime_anari_common.hpp"

#include <ascent_logging.hpp>

namespace ascent { namespace runtime { namespace filters {

AnariGlyphs::AnariGlyphs()
    : Filter()
    , pimpl(std::make_shared<AnariImpl>())
{}

AnariGlyphs::~AnariGlyphs() = default;

void
AnariGlyphs::declare_interface(conduit::Node &i)
{
    i["type_name"]           = "anari_glyphs";
    i["port_names"].append() = "in";
    i["output_port"]         = "false";
}

bool
AnariGlyphs::verify_params(const conduit::Node &params, conduit::Node &info)
{
    return anari_detail::verify_params(params, info);
}

void
AnariGlyphs::execute()
{
    std::string      field_name;
    viskores::Bounds bounds;
    vtkh::DataSet   *topo = anari_detail::extract_topology(
        this, /*expected_components=*/3, /*filter_kind=*/"Glyphs",
        field_name, bounds);
    if (topo == nullptr)
    {
        return;
    }

    configure_from_params(*pimpl, params(), bounds);
    pimpl->render_glyphs(*topo);
}

}}} // namespace ascent::runtime::filters
