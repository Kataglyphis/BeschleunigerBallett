// The index walk mirrors ObjLoader::loadVertices and must be kept in sync with it.

// Must precede fuzztest.h, which friend-declares them unincluded; a force-include would break module BMI synthesis.
#include "absl/random/internal/distribution_caller.h"// IWYU pragma: keep
#include "absl/random/internal/mock_helpers.h"// IWYU pragma: keep

#include <array>
#include <cstddef>
#include <string>
#include <vector>

#include "fuzztest/fuzztest.h"

// This target links no engine, so it carries its own copy of the header-only implementation.
#define TINYOBJLOADER_IMPLEMENTATION
#include "tiny_obj_loader.h"

namespace {

void ParsingArbitraryObjNeverCrashes(const std::string &obj_text, const std::string &mtl_text)
{
    tinyobj::ObjReader reader;
    tinyobj::ObjReaderConfig config;
    config.triangulate = true;

    if (!reader.ParseFromString(obj_text, mtl_text, config)) {
        return;// parse rejection is fine; crashing is not
    }

    const auto &attrib = reader.GetAttrib();

    // A face with any bad index is dropped whole, so `indices` stays a multiple of 3.
    std::vector<float> positions;// flattened x,y,z per emitted vertex
    std::vector<unsigned int> indices;

    float checksum = 0.0F;
    for (const auto &shape : reader.GetShapes()) {
        size_t index_offset = 0;
        for (size_t f = 0; f < shape.mesh.num_face_vertices.size(); f++) {
            const auto fv = static_cast<size_t>(shape.mesh.num_face_vertices[f]);

            bool face_valid = true;
            for (size_t v = 0; v < fv; v++) {
                if (index_offset + v >= shape.mesh.indices.size()) {
                    face_valid = false;
                    break;
                }
                tinyobj::index_t const idx = shape.mesh.indices[index_offset + v];
                const auto vertex_index = static_cast<size_t>(idx.vertex_index);
                if (idx.vertex_index < 0 || (3 * vertex_index) + 2 >= attrib.vertices.size()) {
                    face_valid = false;
                    break;
                }
            }

            if (face_valid) {
                for (size_t v = 0; v < fv; v++) {
                    tinyobj::index_t const idx = shape.mesh.indices[index_offset + v];
                    const auto vertex_index = static_cast<size_t>(idx.vertex_index);

                    checksum += attrib.vertices[(3 * vertex_index) + 0];
                    if (idx.normal_index >= 0
                        && (3 * static_cast<size_t>(idx.normal_index)) + 2 < attrib.normals.size()) {
                        checksum += attrib.normals[3 * static_cast<size_t>(idx.normal_index)];
                    }
                    if ((3 * vertex_index) + 2 < attrib.colors.size()) {
                        checksum += attrib.colors[3 * vertex_index];
                    }
                    if (idx.texcoord_index >= 0
                        && (2 * static_cast<size_t>(idx.texcoord_index)) + 1 < attrib.texcoords.size()) {
                        checksum += attrib.texcoords[2 * static_cast<size_t>(idx.texcoord_index)];
                    }

                    indices.push_back(static_cast<unsigned int>(positions.size() / 3));
                    positions.push_back(attrib.vertices[(3 * vertex_index) + 0]);
                    positions.push_back(attrib.vertices[(3 * vertex_index) + 1]);
                    positions.push_back(attrib.vertices[(3 * vertex_index) + 2]);
                }
            }

            index_offset += fv;
        }
    }

    // Zero-area-guarded like the engine, so a degenerate triangle cannot normalize to NaN.
    for (size_t i = 0; i + 2 < indices.size(); i += 3) {
        const auto vertexPosition = [&](size_t elem) -> std::array<float, 3> {
            const size_t base = static_cast<size_t>(indices[elem]) * 3;
            return { positions[base + 0], positions[base + 1], positions[base + 2] };
        };
        const auto p0 = vertexPosition(i + 0);
        const auto p1 = vertexPosition(i + 1);
        const auto p2 = vertexPosition(i + 2);

        const std::array<float, 3> e1{ p1[0] - p0[0], p1[1] - p0[1], p1[2] - p0[2] };
        const std::array<float, 3> e2{ p2[0] - p0[0], p2[1] - p0[1], p2[2] - p0[2] };
        const std::array<float, 3> faceNormal{ (e1[1] * e2[2]) - (e1[2] * e2[1]),
            (e1[2] * e2[0]) - (e1[0] * e2[2]), (e1[0] * e2[1]) - (e1[1] * e2[0]) };
        const float lenSq = (faceNormal[0] * faceNormal[0]) + (faceNormal[1] * faceNormal[1])
                             + (faceNormal[2] * faceNormal[2]);
        if (lenSq <= 0.0F) { continue; }// degenerate triangle: no normalize
        checksum += faceNormal[0];
    }

    (void)checksum;
}

FUZZ_TEST(ObjParsingFuzz, ParsingArbitraryObjNeverCrashes)
  .WithSeeds({ { "v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n", "" },
    { "v 0 0 0\nf -1 -2 -3\n", "" },
    { "v 1 2 3\nvn 0 1 0\nvt 0.5 0.5\nf 1/1/1 1/1/1 1/1/1\n", "newmtl m\nKd 1 0 0\n" },
    { "v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 999999\n", "" } });

}// namespace
