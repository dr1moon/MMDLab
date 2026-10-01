#include "Runtime/Scene/StaticFloor.h"

#include "Runtime/Asset/Model.h"

namespace MmdLab
{
Model BuildReflectiveFloorModel()
{
    constexpr float kHalfExtent = 100.0f; // Large enough to underpin any model.

    Model model;
    model.name = "ReflectiveFloor";
    model.meshType = MeshType::Static;

    // A unit-up quad in the XZ plane, wound so its +Y normal faces the camera from above. The
    // vertex shader recomputes the w component, so it is left at 1.0 for clarity.
    model.mesh.vertices = {
        { { -kHalfExtent, 0.0f, -kHalfExtent, 1.0f }, { 0.0f, 1.0f, 0.0f, 0.0f }, { 0.0f, 0.0f }, { 0.0f, 0.0f } },
        { {  kHalfExtent, 0.0f, -kHalfExtent, 1.0f }, { 0.0f, 1.0f, 0.0f, 0.0f }, { 1.0f, 0.0f }, { 0.0f, 0.0f } },
        { {  kHalfExtent, 0.0f,  kHalfExtent, 1.0f }, { 0.0f, 1.0f, 0.0f, 0.0f }, { 1.0f, 1.0f }, { 0.0f, 0.0f } },
        { { -kHalfExtent, 0.0f,  kHalfExtent, 1.0f }, { 0.0f, 1.0f, 0.0f, 0.0f }, { 0.0f, 1.0f }, { 0.0f, 0.0f } },
    };
    model.mesh.indices = { 0, 1, 2, 0, 2, 3 };

    MMDToonMaterial material;
    for (int c = 0; c < 3; ++c)
    {
        material.baseColor[c] = 0.85f;
    }
    material.baseColor[3] = 1.0f;
    material.flags = 0x01u; // Double-sided so the floor never back-face culls.
    model.mesh.materials = { material };

    model.mesh.drawPackets = { { 0, 6, 0, 0, 0 } }; // Static: no skin-reference bones.

    model.mesh.boundsMin[0] = -kHalfExtent;
    model.mesh.boundsMin[1] = 0.0f;
    model.mesh.boundsMin[2] = -kHalfExtent;
    model.mesh.boundsMax[0] = kHalfExtent;
    model.mesh.boundsMax[1] = 0.0f;
    model.mesh.boundsMax[2] = kHalfExtent;

    // A Static floor: no skeleton, skinning, or bind pose. The static vertex shader transforms the
    // quad directly without reading Bones/RefBones.
    return model;
}
} // namespace MmdLab
