#include "Runtime/Scene/StaticFloor.h"

#include "Runtime/Asset/Model.h"
#include "Runtime/Core/TestFramework.h"

#include <cmath>
#include <cstddef>

namespace
{
bool IsIdentity(const DirectX::XMFLOAT4X4& matrix)
{
    for (int row = 0; row < 4; ++row)
    {
        for (int column = 0; column < 4; ++column)
        {
            const float expected = (row == column) ? 1.0f : 0.0f;
            if (std::fabs(matrix.m[row][column] - expected) > 1e-5f)
            {
                return false;
            }
        }
    }
    return true;
}
} // namespace

MMDLAB_TEST(Scene.StaticFloor, BuildsReflectiveFloor)
{
    const MmdLab::Model floor = MmdLab::BuildReflectiveFloorModel();

    MMDLAB_CHECK_EQUAL(floor.name, "ReflectiveFloor");
    MMDLAB_CHECK_EQUAL(floor.mesh.vertices.size(), std::size_t{4});
    MMDLAB_CHECK_EQUAL(floor.mesh.indices.size(), std::size_t{6});
    MMDLAB_CHECK_EQUAL(floor.mesh.materials.size(), std::size_t{1});
    MMDLAB_CHECK_EQUAL(floor.mesh.drawPackets.size(), std::size_t{1});
    MMDLAB_CHECK_EQUAL(floor.mesh.refBones.size(), std::size_t{1});
    MMDLAB_CHECK_EQUAL(floor.mesh.refBones[0], std::uint16_t{0});
    MMDLAB_CHECK_EQUAL(floor.skeleton.bones.size(), std::size_t{1});
    MMDLAB_CHECK_EQUAL(floor.skinning.size(), floor.mesh.vertices.size());
    MMDLAB_CHECK_EQUAL(floor.bindPose.inverseBind.size(), std::size_t{1});

    // A single root bone at the origin with a +Y tail collapses to an identity bind pose, so the
    // identity skinning palette reproduces the quad unchanged through the skinned vertex shader.
    MMDLAB_CHECK(IsIdentity(floor.bindPose.bindRotation[0]));
    MMDLAB_CHECK(IsIdentity(floor.bindPose.localBind[0]));
    MMDLAB_CHECK(IsIdentity(floor.bindPose.inverseBind[0]));
}
