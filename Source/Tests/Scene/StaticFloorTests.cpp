#include "Runtime/Scene/StaticFloor.h"

#include "Runtime/Asset/Model.h"
#include "Runtime/Core/TestFramework.h"

#include <cstddef>

MMDLAB_TEST(Scene.StaticFloor, BuildsReflectiveFloor)
{
    const MmdLab::Model floor = MmdLab::BuildReflectiveFloorModel();

    MMDLAB_CHECK_EQUAL(floor.name, "ReflectiveFloor");
    MMDLAB_CHECK(floor.meshType == MmdLab::MeshType::Static);
    MMDLAB_CHECK_EQUAL(floor.mesh.vertices.size(), std::size_t{4});
    MMDLAB_CHECK_EQUAL(floor.mesh.indices.size(), std::size_t{6});
    MMDLAB_CHECK_EQUAL(floor.mesh.materials.size(), std::size_t{1});
    MMDLAB_CHECK_EQUAL(floor.mesh.drawPackets.size(), std::size_t{1});
    MMDLAB_CHECK_EQUAL(floor.mesh.drawPackets[0].refBoneOffset, std::uint32_t{0});
    MMDLAB_CHECK_EQUAL(floor.mesh.drawPackets[0].refBoneCount, std::uint32_t{0});

    // A Static floor carries no skeleton, skinning, or bind pose.
    MMDLAB_CHECK(floor.mesh.refBones.empty());
    MMDLAB_CHECK(floor.skeleton.bones.empty());
    MMDLAB_CHECK(floor.skinning.empty());
    MMDLAB_CHECK(floor.bindPose.inverseBind.empty());
}
