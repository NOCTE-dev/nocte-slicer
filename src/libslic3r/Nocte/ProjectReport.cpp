// NØCTE Slicer — Copyright (c) 2026 NØCTE Engineering. AGPL-3.0-or-later.

#include "libslic3r/Nocte/ProjectReport.hpp"

#include <cstddef>
#include <memory>
#include <string>

#include "libslic3r/Model.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/Nocte/NocteVersion.hpp"

#include "nlohmann/json.hpp"

namespace Slic3r {
namespace Nocte {

namespace {

using json = nlohmann::json;

} // namespace

bool project_report_enabled() { return true; }

std::string project_report_json(const Model &model, int indent)
{
    json objects = json::array();
    for (const ModelObject *object : model.objects) {
        if (object == nullptr)
            continue;

        std::size_t facets = 0;
        for (const ModelVolume *volume : object->volumes) {
            if (volume == nullptr)
                continue;
            // mesh_ptr() rather than mesh(): the latter dereferences the shared pointer, and a
            // volume whose mesh was never set would take the whole save down with it.
            const std::shared_ptr<const TriangleMesh> mesh = volume->mesh_ptr();
            if (mesh)
                facets += mesh->its.indices.size();
        }

        objects.push_back(json{
            { "name",    object->name },
            { "volumes", object->volumes.size() },
            { "facets",  facets }
        });
    }

    const json root = json{
        { "schema",        NOCTE_PROJECT_REPORT_SCHEMA },
        { "nocte_version", NOCTE_VERSION },
        { "generator",     NOCTE_APP_NAME },
        { "objects",       objects },
        // Per-mesh repair reports (Nocte::NocteReport::to_json) are attached here once the repair
        // pipeline persists them; the key exists from schema 1 so readers can rely on it.
        { "reports",       json::array() }
    };

    // Object names come from arbitrary file names and may not be valid UTF-8; replace rather than
    // throw, as Config.cpp does when it serialises user-supplied strings.
    return root.dump(indent, ' ', false, json::error_handler_t::replace);
}

} // namespace Nocte
} // namespace Slic3r
