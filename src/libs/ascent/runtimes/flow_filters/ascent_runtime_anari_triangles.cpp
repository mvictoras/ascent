//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~//
// Copyright (c) Lawrence Livermore National Security, LLC and other Ascent
// Project developers. See top-level LICENSE AND COPYRIGHT files for dates and
// other details. No copyright assignment is required to contribute to Ascent.
//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~//

#include "ascent_runtime_anari_filters.hpp"
#include "ascent_runtime_anari_common.hpp"

#include <ascent_logging.hpp>

namespace ascent { namespace runtime { namespace filters {

AnariTriangles::AnariTriangles()
    : Filter()
    , pimpl(std::make_shared<AnariImpl>())
{}

AnariTriangles::~AnariTriangles() = default;

void
AnariTriangles::declare_interface(conduit::Node &i)
{
    i["type_name"]           = "anari_pseudocolor";
    i["port_names"].append() = "in";
    i["output_port"]         = "false";
}

bool
AnariTriangles::verify_params(const conduit::Node &params, conduit::Node &info)
{
    return anari_detail::verify_params(params, info);
}

void
AnariTriangles::execute()
{
    std::string      field_name;
    viskores::Bounds bounds;
    vtkh::DataSet   *topo = anari_detail::extract_topology(
        this, /*expected_components=*/1, /*filter_kind=*/"Pseudocolor",
        field_name, bounds);
    if (topo == nullptr)
    {
        return;
    }

    configure_from_params(*pimpl, params(), bounds);
    pimpl->render_triangles(*topo);
}

}}} // namespace ascent::runtime::filters
