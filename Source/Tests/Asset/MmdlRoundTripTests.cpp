#include "Runtime/Asset/MmdlFile.h"
#include "Runtime/Asset/MmdlFormat.h"
#include "Runtime/Asset/MmdlWriter.h"
#include "Runtime/Core/TestFramework.h"

#include <filesystem>

namespace
{
MmdLab::MmdlMeshData MakeTestMesh()
{
    MmdLab::MmdlMeshData mesh;

    // A unit quad: 4 vertices, 2 triangles, split across 2 materials.
    mesh.vertices = {
        { { 0.0f, 0.0f, 0.0f }, { 0.0f, 0.0f, 1.0f }, { 0.0f, 0.0f } },
        { { 1.0f, 0.0f, 0.0f }, { 0.0f, 0.0f, 1.0f }, { 1.0f, 0.0f } },
        { { 1.0f, 1.0f, 0.0f }, { 0.0f, 0.0f, 1.0f }, { 1.0f, 1.0f } },
        { { 0.0f, 1.0f, 0.0f }, { 0.0f, 0.0f, 1.0f }, { 0.0f, 1.0f } },
    };
    mesh.indices = { 0, 1, 2, 0, 2, 3 };

    MmdLab::MMDToonMaterial red;
    red.baseColor[0] = 1.0f;
    red.baseColor[1] = 0.0f;
    red.baseColor[2] = 0.0f;
    red.baseColor[3] = 1.0f;
    red.specularStrength = 0.5f;
    red.baseColorTexture = 0;
    red.sphereTexture = 1;
    red.sphereMode = 2u; // sphere add.
    red.flags = 0x01u; // double-sided.

    MmdLab::MMDToonMaterial green;
    green.baseColor[0] = 0.0f;
    green.baseColor[1] = 1.0f;
    green.baseColor[2] = 0.0f;
    green.baseColor[3] = 1.0f;
    green.toonTexture = 0;
    green.sphereMode = 1u; // sphere multiply.
    green.flags = 0x00u; // single-sided.

    mesh.materials = { red, green };
    mesh.drawPackets = {
        { 0, 3, 0, 0, 1 }, // first triangle -> red; skin-reference bones [0, 1).
        { 3, 3, 1, 1, 1 }, // second triangle -> green; skin-reference bones [1, 2).
    };
    mesh.refBones = { 0, 1 };
    mesh.strings = { "red.png", "green.png" };
    mesh.boundsMin[0] = 0.0f; mesh.boundsMin[1] = 0.0f; mesh.boundsMin[2] = 0.0f;
    mesh.boundsMax[0] = 1.0f; mesh.boundsMax[1] = 1.0f; mesh.boundsMax[2] = 0.0f;

    // Two bones (root + child); names are stored inline.
    MmdLab::MmdlBone root{};
    root.name = "root";
    root.position[0] = 0.0f; root.position[1] = 0.0f; root.position[2] = 0.0f;
    root.tail[0] = 0.0f; root.tail[1] = 1.0f; root.tail[2] = 0.0f;
    root.parentIndex = MmdLab::kInvalidBoneIndex;

    MmdLab::MmdlBone child{};
    child.name = "child";
    child.position[0] = 0.0f; child.position[1] = 1.0f; child.position[2] = 0.0f;
    child.tail[0] = 0.0f; child.tail[1] = 2.0f; child.tail[2] = 0.0f;
    child.parentIndex = 0;
    child.hasLocalAxes = 1u;
    child.localX[0] = 1.0f;
    child.localZ[2] = 1.0f;

    child.hasInheritRotation = 1u;
    child.hasInheritTranslation = 1u;
    child.inheritParentIndex = 0;
    child.inheritInfluence = 0.5f;
    child.hasFixedAxis = 1u;
    child.fixedAxis[0] = 1.0f;

    root.ikTargetIndex = 1; // root drives an IK chain targeting the child.
    root.ikLoopCount = 40;
    root.ikLimitAngle = 2.0f;
    MmdLab::MmdlIkLink ikLink;
    ikLink.boneIndex = 1;
    ikLink.hasLimit = 1u;
    ikLink.limitMin[0] = -1.0f;
    ikLink.limitMax[0] = 0.5f;
    root.ikLinks = { ikLink };

    mesh.bones = { root, child };

    for (int i = 0; i < 4; ++i)
    {
        MmdLab::MmdlSkinningVertex skinning{};
        skinning.boneIndices[0] = (i < 2) ? 0 : 1;
        skinning.boneWeights[0] = 1.0f;
        mesh.skinning.push_back(skinning);
    }
    return mesh;
}
} // namespace

MMDLAB_TEST(Asset.Mmdl, RoundTripPreservesMesh)
{
    const MmdLab::MmdlMeshData expected = MakeTestMesh();
    const std::filesystem::path path = std::filesystem::temp_directory_path() / "mmdl_roundtrip.mmdl";

    MmdLab::WriteMmdl(path, expected);
    const MmdLab::MmdlMeshData actual = MmdLab::ReadMmdl(path);
    std::filesystem::remove(path);

    MMDLAB_CHECK_EQUAL(expected.vertices.size(), actual.vertices.size());
    MMDLAB_CHECK_EQUAL(expected.indices.size(), actual.indices.size());
    MMDLAB_CHECK_EQUAL(expected.materials.size(), actual.materials.size());
    MMDLAB_CHECK_EQUAL(expected.drawPackets.size(), actual.drawPackets.size());
    MMDLAB_CHECK_EQUAL(expected.strings.size(), actual.strings.size());

    for (std::size_t i = 0; i < expected.vertices.size(); ++i)
    {
        MMDLAB_CHECK_EQUAL(expected.vertices[i].position[0], actual.vertices[i].position[0]);
        MMDLAB_CHECK_EQUAL(expected.vertices[i].position[1], actual.vertices[i].position[1]);
        MMDLAB_CHECK_EQUAL(expected.vertices[i].position[2], actual.vertices[i].position[2]);
        MMDLAB_CHECK_EQUAL(expected.vertices[i].uv[0], actual.vertices[i].uv[0]);
        MMDLAB_CHECK_EQUAL(expected.vertices[i].uv[1], actual.vertices[i].uv[1]);
    }

    for (std::size_t i = 0; i < expected.indices.size(); ++i)
    {
        MMDLAB_CHECK_EQUAL(expected.indices[i], actual.indices[i]);
    }

    for (std::size_t i = 0; i < expected.materials.size(); ++i)
    {
        for (int c = 0; c < 4; ++c)
        {
            MMDLAB_CHECK_EQUAL(expected.materials[i].baseColor[c], actual.materials[i].baseColor[c]);
        }
        MMDLAB_CHECK_EQUAL(expected.materials[i].specularStrength, actual.materials[i].specularStrength);
        MMDLAB_CHECK_EQUAL(expected.materials[i].baseColorTexture, actual.materials[i].baseColorTexture);
        MMDLAB_CHECK_EQUAL(expected.materials[i].toonTexture, actual.materials[i].toonTexture);
        MMDLAB_CHECK_EQUAL(expected.materials[i].sphereTexture, actual.materials[i].sphereTexture);
        MMDLAB_CHECK_EQUAL(expected.materials[i].sphereMode, actual.materials[i].sphereMode);
        MMDLAB_CHECK_EQUAL(expected.materials[i].flags, actual.materials[i].flags);
    }

    for (std::size_t i = 0; i < expected.drawPackets.size(); ++i)
    {
        MMDLAB_CHECK_EQUAL(expected.drawPackets[i].firstIndex, actual.drawPackets[i].firstIndex);
        MMDLAB_CHECK_EQUAL(expected.drawPackets[i].indexCount, actual.drawPackets[i].indexCount);
        MMDLAB_CHECK_EQUAL(expected.drawPackets[i].materialIndex, actual.drawPackets[i].materialIndex);
        MMDLAB_CHECK_EQUAL(expected.drawPackets[i].refBoneOffset, actual.drawPackets[i].refBoneOffset);
        MMDLAB_CHECK_EQUAL(expected.drawPackets[i].refBoneCount, actual.drawPackets[i].refBoneCount);
    }

    // Skin-reference-bone table round-trips.
    MMDLAB_CHECK_EQUAL(expected.refBones.size(), actual.refBones.size());
    for (std::size_t i = 0; i < expected.refBones.size(); ++i)
    {
        MMDLAB_CHECK_EQUAL(expected.refBones[i], actual.refBones[i]);
    }

    MMDLAB_CHECK_EQUAL(expected.strings[0], actual.strings[0]);
    MMDLAB_CHECK_EQUAL(expected.strings[1], actual.strings[1]);

    // Skeleton + skinning round-trip.
    MMDLAB_CHECK_EQUAL(expected.bones.size(), actual.bones.size());
    MMDLAB_CHECK_EQUAL(expected.skinning.size(), actual.skinning.size());

    for (std::size_t i = 0; i < expected.bones.size(); ++i)
    {
        MMDLAB_CHECK_EQUAL(expected.bones[i].name, actual.bones[i].name);
        MMDLAB_CHECK_EQUAL(expected.bones[i].parentIndex, actual.bones[i].parentIndex);
        MMDLAB_CHECK_EQUAL(expected.bones[i].hasLocalAxes, actual.bones[i].hasLocalAxes);
        for (int axis = 0; axis < 3; ++axis)
        {
            MMDLAB_CHECK_EQUAL(expected.bones[i].position[axis], actual.bones[i].position[axis]);
            MMDLAB_CHECK_EQUAL(expected.bones[i].tail[axis], actual.bones[i].tail[axis]);
            MMDLAB_CHECK_EQUAL(expected.bones[i].localX[axis], actual.bones[i].localX[axis]);
            MMDLAB_CHECK_EQUAL(expected.bones[i].localZ[axis], actual.bones[i].localZ[axis]);
            MMDLAB_CHECK_EQUAL(expected.bones[i].fixedAxis[axis], actual.bones[i].fixedAxis[axis]);
        }
        MMDLAB_CHECK_EQUAL(expected.bones[i].hasInheritRotation, actual.bones[i].hasInheritRotation);
        MMDLAB_CHECK_EQUAL(expected.bones[i].hasInheritTranslation, actual.bones[i].hasInheritTranslation);
        MMDLAB_CHECK_EQUAL(expected.bones[i].inheritParentIndex, actual.bones[i].inheritParentIndex);
        MMDLAB_CHECK_EQUAL(expected.bones[i].inheritInfluence, actual.bones[i].inheritInfluence);
        MMDLAB_CHECK_EQUAL(expected.bones[i].hasFixedAxis, actual.bones[i].hasFixedAxis);
        MMDLAB_CHECK_EQUAL(expected.bones[i].ikTargetIndex, actual.bones[i].ikTargetIndex);
        MMDLAB_CHECK_EQUAL(expected.bones[i].ikLoopCount, actual.bones[i].ikLoopCount);
        MMDLAB_CHECK_EQUAL(expected.bones[i].ikLimitAngle, actual.bones[i].ikLimitAngle);
        MMDLAB_CHECK_EQUAL(expected.bones[i].ikLinks.size(), actual.bones[i].ikLinks.size());
        for (std::size_t link = 0; link < expected.bones[i].ikLinks.size(); ++link)
        {
            MMDLAB_CHECK_EQUAL(expected.bones[i].ikLinks[link].boneIndex, actual.bones[i].ikLinks[link].boneIndex);
            MMDLAB_CHECK_EQUAL(expected.bones[i].ikLinks[link].hasLimit, actual.bones[i].ikLinks[link].hasLimit);
            for (int axis = 0; axis < 3; ++axis)
            {
                MMDLAB_CHECK_EQUAL(expected.bones[i].ikLinks[link].limitMin[axis], actual.bones[i].ikLinks[link].limitMin[axis]);
                MMDLAB_CHECK_EQUAL(expected.bones[i].ikLinks[link].limitMax[axis], actual.bones[i].ikLinks[link].limitMax[axis]);
            }
        }
    }

    for (std::size_t i = 0; i < expected.skinning.size(); ++i)
    {
        for (int slot = 0; slot < 4; ++slot)
        {
            MMDLAB_CHECK_EQUAL(expected.skinning[i].boneIndices[slot], actual.skinning[i].boneIndices[slot]);
            MMDLAB_CHECK_EQUAL(expected.skinning[i].boneWeights[slot], actual.skinning[i].boneWeights[slot]);
        }
    }

    for (int axis = 0; axis < 3; ++axis)
    {
        MMDLAB_CHECK_EQUAL(expected.boundsMin[axis], actual.boundsMin[axis]);
        MMDLAB_CHECK_EQUAL(expected.boundsMax[axis], actual.boundsMax[axis]);
    }
}

MMDLAB_TEST(Asset.Mmdl, RoundTripPreservesPhysics)
{
    MmdLab::MmdlMeshData expected = MakeTestMesh();
    MmdLab::BodySetup collider;
    collider.name = "thigh";
    collider.boneIndex = 0;
    collider.group = 1;
    collider.collisionMask = 0x82F0;
    collider.shape = MmdLab::BodyShape::Capsule;
    collider.mode = MmdLab::BodyMode::FollowBone;
    collider.size[0] = 0.8f; collider.size[1] = 2.5f;
    collider.position[1] = 0.5f;
    collider.rotation[2] = 0.25f;
    MmdLab::BodySetup cloth = collider;
    cloth.name = "skirt";
    cloth.boneIndex = 1;
    cloth.group = 7;
    cloth.collisionMask = 0xFF7F;
    cloth.shape = MmdLab::BodyShape::Box;
    cloth.mode = MmdLab::BodyMode::PhysicsWithBonePosition;
    cloth.mass = 0.5f;
    cloth.linearDamping = 0.9f;
    cloth.angularDamping = 0.99f;
    cloth.restitution = 0.1f;
    cloth.friction = 0.5f;
    expected.physics.bodies = { collider, cloth };

    MmdLab::ConstraintSetup joint;
    joint.name = "thigh-skirt";
    joint.bodyA = 0;
    joint.bodyB = 1;
    joint.position[1] = 1.0f;
    joint.rotation[0] = 0.1f;
    joint.angularLowerLimit[0] = -0.5f;
    joint.angularUpperLimit[0] = 0.5f;
    joint.linearStiffness[2] = 10.0f;
    joint.angularStiffness[1] = 20.0f;
    expected.physics.constraints = { joint };

    const std::filesystem::path path = std::filesystem::temp_directory_path() / "mmdl_physics_roundtrip.mmdl";
    MmdLab::WriteMmdl(path, expected);
    const MmdLab::MmdlMeshData actual = MmdLab::ReadMmdl(path);
    std::filesystem::remove(path);

    MMDLAB_CHECK_EQUAL(expected.physics.bodies.size(), actual.physics.bodies.size());
    MMDLAB_CHECK_EQUAL(expected.physics.constraints.size(), actual.physics.constraints.size());
    for (std::size_t i = 0; i < expected.physics.bodies.size() && i < actual.physics.bodies.size(); ++i)
    {
        const MmdLab::BodySetup& a = expected.physics.bodies[i];
        const MmdLab::BodySetup& b = actual.physics.bodies[i];
        MMDLAB_CHECK(a.name == b.name);
        MMDLAB_CHECK_EQUAL(a.boneIndex, b.boneIndex);
        MMDLAB_CHECK_EQUAL(a.group, b.group);
        MMDLAB_CHECK_EQUAL(a.collisionMask, b.collisionMask);
        MMDLAB_CHECK(a.shape == b.shape);
        MMDLAB_CHECK(a.mode == b.mode);
        for (int axis = 0; axis < 3; ++axis)
        {
            MMDLAB_CHECK_EQUAL(a.size[axis], b.size[axis]);
            MMDLAB_CHECK_EQUAL(a.position[axis], b.position[axis]);
            MMDLAB_CHECK_EQUAL(a.rotation[axis], b.rotation[axis]);
        }
        MMDLAB_CHECK_EQUAL(a.mass, b.mass);
        MMDLAB_CHECK_EQUAL(a.linearDamping, b.linearDamping);
        MMDLAB_CHECK_EQUAL(a.angularDamping, b.angularDamping);
        MMDLAB_CHECK_EQUAL(a.restitution, b.restitution);
        MMDLAB_CHECK_EQUAL(a.friction, b.friction);
    }
    if (!actual.physics.constraints.empty())
    {
        const MmdLab::ConstraintSetup& b = actual.physics.constraints[0];
        MMDLAB_CHECK(b.name == joint.name);
        MMDLAB_CHECK_EQUAL(joint.bodyA, b.bodyA);
        MMDLAB_CHECK_EQUAL(joint.bodyB, b.bodyB);
        for (int axis = 0; axis < 3; ++axis)
        {
            MMDLAB_CHECK_EQUAL(joint.position[axis], b.position[axis]);
            MMDLAB_CHECK_EQUAL(joint.rotation[axis], b.rotation[axis]);
            MMDLAB_CHECK_EQUAL(joint.linearLowerLimit[axis], b.linearLowerLimit[axis]);
            MMDLAB_CHECK_EQUAL(joint.linearUpperLimit[axis], b.linearUpperLimit[axis]);
            MMDLAB_CHECK_EQUAL(joint.angularLowerLimit[axis], b.angularLowerLimit[axis]);
            MMDLAB_CHECK_EQUAL(joint.angularUpperLimit[axis], b.angularUpperLimit[axis]);
            MMDLAB_CHECK_EQUAL(joint.linearStiffness[axis], b.linearStiffness[axis]);
            MMDLAB_CHECK_EQUAL(joint.angularStiffness[axis], b.angularStiffness[axis]);
        }
    }
}
