/****************************************************************************/
//    Copyright (C) 2026 Chris Hendrickson (cmh175)                         //
//                                                                          //
//    This file is part of FFNx                                             //
//                                                                          //
//    FFNx is free software: you can redistribute it and/or modify          //
//    it under the terms of the GNU General Public License as published by  //
//    the Free Software Foundation, either version 3 of the License         //
//                                                                          //
//    FFNx is distributed in the hope that it will be useful,               //
//    but WITHOUT ANY WARRANTY; without even the implied warranty of        //
//    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         //
//    GNU General Public License for more details.                          //
/****************************************************************************/

#include "../../common.h"
#include "../../globals.h"
#include "../../cfg.h"
#include "../../gl.h"
#include "../../log.h"
#include "../../matrix.h"
#include "../../movies.h"
#include "../../renderer.h"
#include "../../external_mesh.h"

#include "environment.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <filesystem>
#include <map>
#include <set>
#include <toml++/toml.h>

void ff7_get_field_view_matrix(struct matrix *outViewMatrix);

namespace ff7::field
{
    Environment environment;

    // glTF is Y up and the field Z up: the field's (x, y, z) is the glTF's (x, -z, y)
    static void environment_world_matrix(struct matrix* matrix)
    {
        identity_matrix(matrix);
        matrix->_22 = 0.0f;
        matrix->_23 = 1.0f;
        matrix->_32 = -1.0f;
        matrix->_33 = 0.0f;
    }

    // bg<group>s<state> anywhere in a part's name (any case): group 1-63, state 0-7
    static bool parse_background_tag(const std::string& name, uint8_t& group, uint8_t& stateBits)
    {
        std::string lower = name;
        std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return std::tolower(c); });
        for (size_t at = lower.find("bg"); at != std::string::npos; at = lower.find("bg", at + 1))
        {
            size_t i = at + 2, digits = i;
            while (i < lower.size() && isdigit((unsigned char)lower[i])) i++;
            if (i == digits || i >= lower.size() || lower[i] != 's') continue;
            size_t stateAt = ++i;
            while (i < lower.size() && isdigit((unsigned char)lower[i])) i++;
            if (i == stateAt) continue;
            int g = atoi(lower.substr(digits, stateAt - 1 - digits).c_str()), st = atoi(lower.substr(stateAt, i - stateAt).c_str());
            if (g < 1 || g > 63 || st < 0 || st > 7) continue;
            group = static_cast<uint8_t>(g);
            stateBits = static_cast<uint8_t>(1 << st);
            return true;
        }
        return false;
    }

    // Which background groups and state bits the field's painted tiles use (layers 2-4), for scene makers
    void Environment::logBackgroundGroups()
    {
        if (!(trace_all || trace_loaders)) return;

        std::map<int, std::set<int>> groups;
        uint32_t* counts[] = { ff7_externals.field_layer2_tiles_num, ff7_externals.field_layer3_tiles_num, ff7_externals.field_layer4_tiles_num };
        field_tile** tiles[] = { ff7_externals.field_layer2_tiles, ff7_externals.field_layer3_tiles, ff7_externals.field_layer4_tiles };
        for (int layer = 0; layer < 3; ++layer)
        {
            if (counts[layer] == nullptr || tiles[layer] == nullptr || *tiles[layer] == nullptr) continue;
            for (uint32_t t = 0; t < *counts[layer]; ++t)
            {
                const field_tile& tile = (*tiles[layer])[t];
                if (tile.anim_group) groups[(uint8_t)tile.anim_group].insert((uint8_t)tile.anim_bitmask);
            }
        }

        std::string list;
        for (const auto& group : groups)
        {
            char entry[64];
            sprintf(entry, "%s%d:", list.empty() ? "" : ", ", group.first);
            list += entry;
            for (int bits : group.second)
            {
                sprintf(entry, " 0x%02X", bits);
                list += entry;
            }
        }
        ffnx_trace("Field environment: %s background groups (group: state bits of its tiles): %s\n", fieldName.c_str(), list.empty() ? "none" : list.c_str());
    }

    void Environment::update()
    {
        const char* name = get_current_field_name();
        std::string current = name ? name : "";
        if (current == fieldName) return;

        fieldName = current;
        if (!current.empty()) logBackgroundGroups();

        // The previous field's scene gives its textures and buffers back to the graphics card
        if (mesh) mesh->destroyFieldResources();
        mesh.reset();
        partGroups.clear();
        partStateBits.clear();
        keepBackground = false;
        lightmapIntensity = 1.0f;
        if (current.empty() || !enable_external_mesh) return;

        char gltfPath[MAX_PATH], texPath[MAX_PATH], configPath[MAX_PATH];
        sprintf(gltfPath, "%s/%s/field_env/%s.gltf", basedir, external_mesh_path.c_str(), current.c_str());
        if (!std::filesystem::exists(gltfPath)) return;
        sprintf(texPath, "%s/%s/field_env/textures/", basedir, external_mesh_path.c_str());
        sprintf(configPath, "%s/%s/field_env/%s_config.toml", basedir, external_mesh_path.c_str(), current.c_str());

        auto loadStart = std::chrono::steady_clock::now();
        auto loaded = std::make_unique<ExternalMesh>();
        if (!loaded->importExternalMeshGltfFile(gltfPath, texPath, true, true)) return;
        mesh = std::move(loaded);

        // Parts tagged for a background group (bg<group>s<state> in the node's name, or the mesh's)
        partGroups.assign(mesh->shapes.size(), 0);
        partStateBits.assign(mesh->shapes.size(), 0);
        for (size_t i = 0; i < mesh->shapes.size(); ++i)
        {
            const auto& shape = mesh->shapes[i];
            const std::string& name = shape.nodeName.empty() ? shape.name : shape.nodeName;
            if (parse_background_tag(name, partGroups[i], partStateBits[i]) && (trace_all || trace_loaders))
                ffnx_trace("Field environment: %s shows with background group %d, state bits 0x%02X\n", name.c_str(), partGroups[i], partStateBits[i]);
        }

        try
        {
            auto config = toml::parse_file(configPath);
            keepBackground = config["environment"]["keep_background"].value_or(false);
            lightmapIntensity = static_cast<float>(config["environment"]["lightmap_intensity"].value_or(1.0));
        }
        catch (const toml::parse_error &err)
        {
            if (std::filesystem::exists(configPath)) ffnx_error("Field environment: %s: %s (line %u)\n", configPath, std::string(err.description()).c_str(), (unsigned)err.source().begin.line);
        }

        double loadMilliseconds = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - loadStart).count();
        if (trace_all || trace_loaders) ffnx_trace("Field environment: loaded %s (%zu parts) in %.0f ms%s\n", gltfPath, mesh->shapes.size(), loadMilliseconds,
            keepBackground ? ", drawn over the painted background" : "");
    }

    bool Environment::isShown()
    {
        update();
        if (!mesh || getmode_cached()->driver_mode != MODE_FIELD) return false;

        // While a movie plays (as the field's background, or with the field's characters drawn over it, like the
        // opening), the movie is what shows behind the characters
        if (ff7_externals.movie_object->is_playing) return false;

        return true;
    }

    bool Environment::hidesBackground()
    {
        return isShown() && !keepBackground;
    }

    void Environment::draw()
    {
        if (!isShown()) return;
        lastDrawnFrame = frame_counter;

        struct driver_state savedState;
        gl_save_state(&savedState);

        struct matrix viewMatrix, worldMatrix, worldViewMatrix;
        ff7_get_field_view_matrix(&viewMatrix);
        environment_world_matrix(&worldMatrix);
        multiply_matrix(&worldMatrix, &viewMatrix, &worldViewMatrix);
        newRenderer.setWorldViewMatrix(&worldViewMatrix);

        newRenderer.setPrimitiveType();
        newRenderer.isTLVertex(false);
        newRenderer.isSmoothSkinning(false);
        newRenderer.isFBTexture(false);
        newRenderer.doTextureFiltering(true);
        newRenderer.isExternalTexture(true);
        newRenderer.doModulateAlpha(true);
        newRenderer.doDepthTest(true);
        newRenderer.setGameLightData(nullptr);

        auto shapeCount = mesh->shapes.size();
        std::vector<uint32_t> vertexOffsets(shapeCount), indexOffsets(shapeCount);
        uint32_t vertexOffset = 0, indexOffset = 0;
        for (size_t i = 0; i < shapeCount; ++i)
        {
            vertexOffsets[i] = vertexOffset;
            indexOffsets[i] = indexOffset;
            vertexOffset += mesh->shapes[i].vertices.size();
            indexOffset += mesh->shapes[i].indices.size();
        }

        // Opaque and cut-out parts first, then the blended ones over them
        for (int pass = 0; pass < 2; ++pass)
        {
            for (size_t i = 0; i < shapeCount; ++i)
            {
                auto& shape = mesh->shapes[i];
                bool blended = shape.alphaMode == ShapeAlphaMode::BLEND_MODE;
                if (blended != (pass == 1)) continue;

                // A part tagged for a background group shows while the script has that state on, like its painted tiles
                if (partGroups[i] != 0 && !(ff7_externals.modules_global_object->background_sprite_layer[partGroups[i]] & partStateBits[i])) continue;

                newRenderer.setInterpolationQualifier(SMOOTH);
                newRenderer.setCullMode(shape.isDoubleSided ? RendererCullMode::DISABLED : RendererCullMode::BACK);
                if (blended)
                {
                    newRenderer.setBlendMode(RendererBlendMode::BLEND_AVG);
                    newRenderer.doDepthWrite(false);
                    newRenderer.doAlphaTest(false);
                }
                else
                {
                    newRenderer.setBlendMode(RendererBlendMode::BLEND_DISABLED);
                    newRenderer.doDepthWrite(true);
                    newRenderer.doAlphaTest(shape.alphaMode == ShapeAlphaMode::MASK_MODE);
                    newRenderer.setAlphaRef(RendererAlphaFunc::GEQUAL, shape.alphaCutoff);
                }

                mesh->bindField3dVertexBuffer(vertexOffsets[i], shape.vertices.size());
                mesh->bindField3dIndexBuffer(indexOffsets[i], shape.indices.size());

                // Every texture slot is set for every part; a part without a base color texture shows its material color
                auto pMaterial = shape.pMaterial;
                bgfx::TextureHandle baseColorTexHandle = BGFX_INVALID_HANDLE;
                bgfx::TextureHandle normalTexHandle = BGFX_INVALID_HANDLE;
                bgfx::TextureHandle pbrTexHandle = BGFX_INVALID_HANDLE;
                if (pMaterial != nullptr)
                {
                    if (pMaterial->baseColorTexHandles.size() > 0) baseColorTexHandle = pMaterial->baseColorTexHandles[pMaterial->texIndex];
                    if (pMaterial->normalTexHandles.size() > 0) normalTexHandle = pMaterial->normalTexHandles[0];
                    if (pMaterial->pbrTexHandles.size() > 0) pbrTexHandle = pMaterial->pbrTexHandles[0];
                }
                newRenderer.useTexture(bgfx::isValid(baseColorTexHandle) ? baseColorTexHandle.idx : 0, RendererTextureSlot::TEX_Y);
                newRenderer.useTexture(bgfx::isValid(normalTexHandle) ? normalTexHandle.idx : 0, RendererTextureSlot::TEX_NML);
                newRenderer.useTexture(bgfx::isValid(pbrTexHandle) ? pbrTexHandle.idx : 0, RendererTextureSlot::TEX_PBR);
                // Baked lighting, on the part's second UV set
                newRenderer.useTexture(bgfx::isValid(shape.lightmapTexHandle) ? shape.lightmapTexHandle.idx : 0, RendererTextureSlot::TEX_LM);
                newRenderer.setLightmapIntensity(lightmapIntensity);
                newRenderer.bindTextures();

                newRenderer.setCommonUniforms();
                if (enable_lighting) newRenderer.setLightingUniforms();

                if (enable_lighting) newRenderer.drawWithLighting(true, true, true);
                else newRenderer.draw(true, true, true);
            }
        }

        newRenderer.discardAllBindings();
        newRenderer.useTexture(0, RendererTextureSlot::TEX_LM);
        newRenderer.isExternalTexture(false);
        gl_load_state(&savedState, true);
    }

    void Environment::drawBeforeFirst3dDraw()
    {
        if (enable_lighting || lastDrawnFrame == frame_counter || !isShown()) return;

        newRenderer.setD3DProjection(&current_state.d3dprojection_matrix);
        newRenderer.setD3DViweport(&d3dviewport_matrix);
        draw();
    }
}
