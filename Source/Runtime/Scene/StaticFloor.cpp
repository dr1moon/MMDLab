#include "Runtime/Scene/StaticFloor.h"

#include "Runtime/Animation/SkeletonPose.h"
#include "Runtime/Asset/Model.h"

namespace MmdLab
{
Model BuildReflectiveFloorModel()
{
    constexpr float kHalfExtent = 100.0f; // Large enough to underpin any model.

    Model model;
    model.name = "ReflectiveFloor";

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

    model.mesh.drawPackets = { { 0, 6, 0, 0, 1 } };
    model.mesh.refBones = { 0 };

    model.mesh.boundsMin[0] = -kHalfExtent;
    model.mesh.boundsMin[1] = 0.0f;
    model.mesh.boundsMin[2] = -kHalfExtent;
    model.mesh.boundsMax[0] = kHalfExtent;
    model.mesh.boundsMax[1] = 0.0f;
    model.mesh.boundsMax[2] = kHalfExtent;

    // One root bone at the origin with a +Y tail: its bind rotation and bind pose collapse to
    // identity, so the skinning palette is identity and the quad passes through the skinned
    // vertex shader unchanged. The shader always reads Bones/RefBones, so a valid single-bone
    // table is required (an empty refBones table would leave the SRV unbound).
    Bone root;
    root.name = "floor_root";
    root.position[0] = 0.0f;
    root.position[1] = 0.0f;
    root.position[2] = 0.0f;
    root.tail[0] = 0.0f;
    root.tail[1] = 1.0f;
    root.tail[2] = 0.0f;
    root.parentIndex = kInvalidBoneIndex;
    model.skeleton.bones = { root };
    model.skeleton.children.resize(1);

    for (int i = 0; i < 4; ++i)
    {
        SkinningVertex vertex;
        vertex.boneIndices[0] = 0;
        vertex.boneWeights[0] = 1.0f;
        model.skinning.push_back(vertex);
    }

    model.bindPose = BuildBindPose(model.skeleton);
    return model;
}
} // namespace MmdLab
