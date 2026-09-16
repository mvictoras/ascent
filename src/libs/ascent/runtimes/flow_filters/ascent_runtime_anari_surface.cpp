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

namespace
{

// One plot of a multi-plot anari extract: which pipeline feeds it, and the
// per-plot render params (field, color_table, min/max_value) that override
// the extract-level ones.
void render_plot(AnariImpl &impl,
                 VTKHCollection *collection,
                 const conduit::Node &plot_params,
                 const conduit::Node &extract_params,
                 const viskores::Bounds &bounds,
                 const std::string &tag)
{
    // Start from the extract-level params so camera/image/samples are shared,
    // then let the plot's own entries win.
    conduit::Node merged = extract_params;
    merged.remove("plots");
    merged.update(plot_params);

    // min/max_value are per-plot; an extract-level leftover would silently
    // clamp a plot that meant to auto-range.
    if (!plot_params.has_path("min_value") && merged.has_path("min_value"))
    {
        merged.remove("min_value");
    }
    if (!plot_params.has_path("max_value") && merged.has_path("max_value"))
    {
        merged.remove("max_value");
    }

    configure_from_params(impl, merged, bounds);

    const std::string field_name = merged["field"].as_string();

    // Prefer the topology anari_merge keyed to this plot's pipeline. Fall back
    // to field lookup for the single-pipeline case, where no merge happened.
    std::string topo_name;
    if (plot_params.has_path("pipeline"))
    {
        const std::string prefix = plot_params["pipeline"].as_string() + "::";
        for (const std::string &t : collection->topology_names())
        {
            if (t.compare(0, prefix.size(), prefix) == 0)
            {
                topo_name = t;
                break;
            }
        }
    }
    if (topo_name.empty())
    {
        topo_name = collection->field_topology(field_name);
    }
    if (topo_name.empty())
    {
        ASCENT_ERROR("Anari plot '" << tag << "' field '" << field_name
                     << "' not found in the input");
    }
    vtkh::DataSet &topo = collection->dataset_by_topology(topo_name);

    const int components = topo.NumberOfComponents(field_name);
    switch (components)
    {
        case 1:
            impl.add_plot_triangles(topo, tag);
            break;
        case 3:
            impl.add_plot_glyphs(topo, tag);
            break;
        default:
            ASCENT_ERROR("Anari plot '" << tag << "' field '" << field_name
                         << "' has " << components
                         << " components; expected 1 (scalar -> triangles) "
                         << "or 3 (vector -> glyphs)");
    }
}

} // namespace

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

    if (params().has_path("plots"))
    {
        const conduit::Node &plots = params()["plots"];
        const viskores::Bounds bounds = collection->global_bounds();
        for (conduit::index_t p = 0; p < plots.number_of_children(); ++p)
        {
            render_plot(*pimpl,
                        collection,
                        plots.child(p),
                        params(),
                        bounds,
                        plots.child_names()[p]);
        }
        pimpl->render_scene();
        return;
    }

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

//-----------------------------------------------------------------------------
AnariMerge::AnariMerge()
    : Filter()
{}

AnariMerge::~AnariMerge() = default;

void
AnariMerge::declare_interface(conduit::Node &i)
{
    i["type_name"]           = "anari_merge";
    i["port_names"].append() = "a";
    i["port_names"].append() = "b";
    i["output_port"]         = "true";
}

bool
AnariMerge::verify_params(const conduit::Node &, conduit::Node &info)
{
    info.reset();
    return true;
}

void
AnariMerge::execute()
{
    if (!input(0).check_type<DataObject>() || !input(1).check_type<DataObject>())
    {
        ASCENT_ERROR("anari_merge inputs must be DataObjects");
    }

    DataObject *a = input<DataObject>(0);
    DataObject *b = input<DataObject>(1);

    // A pipeline that produced no geometry on this rank yields an invalid
    // DataObject; pass the other side through rather than failing the frame.
    if (!a->is_valid())
    {
        set_output<DataObject>(b);
        return;
    }
    if (!b->is_valid())
    {
        set_output<DataObject>(a);
        return;
    }

    std::shared_ptr<VTKHCollection> ca = a->as_vtkh_collection();
    std::shared_ptr<VTKHCollection> cb = b->as_vtkh_collection();

    VTKHCollection *merged = new VTKHCollection();

    // Sibling pipelines share topology names AND field names, so re-key by
    // source pipeline. An already-merged input (left side of the chain) keeps
    // the keys it arrived with.
    const std::string keys[2] = {
        params().has_path("a_key") ? params()["a_key"].as_string() : std::string(),
        params().has_path("b_key") ? params()["b_key"].as_string() : std::string(),
    };
    VTKHCollection *cols[2] = {ca.get(), cb.get()};

    for (int side = 0; side < 2; ++side)
    {
        for (const std::string &topo : cols[side]->topology_names())
        {
            const bool already_keyed = topo.find("::") != std::string::npos;
            const std::string key = (already_keyed || keys[side].empty())
                                        ? topo
                                        : keys[side] + "::" + topo;
            merged->add(cols[side]->dataset_by_topology(topo), key);
        }
    }

    DataObject *out = new DataObject(merged);
    set_output<DataObject>(out);
}

}}} // namespace ascent::runtime::filters
