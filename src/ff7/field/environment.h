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

#pragma once

#include <memory>
#include <string>
#include <stdint.h>

class ExternalMesh;

namespace ff7::field
{
    // A 3D scene drawn in place of a field's painted background: mesh/field_env/<field>.gltf. It is modelled in the
    // field's own coordinates and units with Z up (exported from Blender with the default glTF settings, Blender's
    // coordinates are the field's) and drawn from the game's field camera. The game itself (walkmesh, scripts,
    // camera) doesn't change.
    class Environment
    {
    public:
        // Loads the current field's scene when the field changes (and drops the previous field's)
        void update();
        // True while the painted background is replaced by the scene: not while a movie plays in the field, and not
        // when the scene's config keeps the background ([environment] keep_background = true, to line the scene up
        // with the painting)
        bool hidesBackground();
        // Draws the scene with the projection of the frame's 3D draws (already set by the caller)
        void draw();
        // Draws the scene before the frame's first 3D draw, when draws aren't deferred (advanced lighting off)
        void drawBeforeFirst3dDraw();

    private:
        bool isShown();

        std::unique_ptr<ExternalMesh> mesh;
        std::string fieldName; // the field the scene was looked up for
        bool keepBackground = false;
        float lightmapIntensity = 1.0f; // [environment] lightmap_intensity
        uint32_t lastDrawnFrame = UINT32_MAX;
    };

    extern Environment environment;
}
