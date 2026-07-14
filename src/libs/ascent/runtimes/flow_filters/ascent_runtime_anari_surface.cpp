//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~//
// Copyright (c) Lawrence Livermore National Security, LLC and other Ascent
// Project developers. See top-level LICENSE AND COPYRIGHT files for dates and
// other details. No copyright assignment is required to contribute to Ascent.
//~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~//

#include "ascent_runtime_anari_filters.hpp"
#include "ascent_runtime_anari_common.hpp"

#include <ascent_logging.hpp>
#include <ascent_vtkh_collection.hpp>

#include <runtimes/ascent_data_object.hpp>

namespace ascent { namespace runtime { namespace filters {

AnariSurface::AnariSurface()
    : Filter()
    , pimpl(std::make_shared<AnariImpl>())
{}

AnariSurface::~AnariSurface() = default;

void
AnariSurface::declare_interface(conduit::Node &i)
{
    i["type_name"]           = "anari";
    i["port_names"].append() = "in";
    i["output_port"]         = "false";
}

bool
AnariSurface::verify_params(const conduit::Node &params, conduit::Node &info)
{
    return anari_detail::verify_params(params, info);
}

void
AnariSurface::execute()
{
    if (!input(0).check_type<DataObject>())
    {
        ASCENT_ERROR("Anari surface input must be a DataObject");
    }
    DataObject *d_input = input<DataObject>(0);
    if (!d_input->is_valid())
    {
        return;
    }

    VTKHCollection *collection = d_input->as_vtkh_collection().get();
    const std::vector<std::string> topos = collection->topology_names();
    if (topos.size() != 1)
    {
        ASCENT_ERROR("Anari surface accepts only one topology, got "
                     << topos.size());
    }
    vtkh::DataSet &topo = collection->dataset_by_topology(topos[0]);

    const std::string field_name = params()["field"].as_string();
    const int components = topo.NumberOfComponents(field_name);

    configure_from_params(*pimpl, params(), collection->global_bounds());

    switch (components)
    {
        case 1:
            pimpl->render_triangles(topo);
            break;
        case 3:
            pimpl->render_glyphs(topo);
            break;
        default:
            ASCENT_ERROR("Anari surface field '" << field_name
                         << "' has " << components
                         << " components; expected 1 (scalar -> triangles) "
                         << "or 3 (vector -> glyphs)");
    }
}

}}} // namespace ascent::runtime::filters
