#include "Runtime/Animation/MorphPose.h"

#include "tracy/Tracy.hpp"

#include <DirectXMath.h>

#include <cstdint>
#include <stdexcept>

namespace MmdLab
{
void ResolveMorphWeights(
    const MorphSet& set,
    const std::vector<float>& directWeights,
    std::vector<float>& outResolved,
    std::vector<MorphResolveEntry>& stack)
{
    ZoneScopedN("ResolveMorphWeights");
    outResolved.assign(set.morphs.size(), 0.0f);
    stack.clear();

    // Group morphs are expanded with an explicit stack (rather than recursion) so a malformed
    // cyclic group morph cannot overflow the call stack; the depth bound turns a cycle into an
    // error instead of an infinite loop.
    const std::size_t count = std::min(directWeights.size(), set.morphs.size());
    for (std::size_t i = 0; i < count; ++i)
    {
        if (directWeights[i] == 0.0f)
        {
            continue;
        }
        stack.push_back({ static_cast<std::uint32_t>(i), directWeights[i], 0 });
        while (!stack.empty())
        {
            const MorphResolveEntry entry = stack.back();
            stack.pop_back();
            if (entry.depth > 32)
            {
                throw std::runtime_error("Group morph cycle or excessive nesting.");
            }
            if (entry.index >= set.morphs.size())
            {
                continue;
            }

            const Morph& morph = set.morphs[entry.index];
            if (morph.kind == MorphKind::Group)
            {
                for (const GroupMorphItem& item : morph.groupItems)
                {
                    if (item.ratio != 0.0f && item.morphIndex < set.morphs.size())
                    {
                        stack.push_back({ item.morphIndex, entry.weight * item.ratio, entry.depth + 1 });
                    }
                }
            }
            else
            {
                outResolved[entry.index] += entry.weight;
            }
        }
    }
}

void ApplyBoneMorphs(
    const MorphSet& set,
    const std::vector<float>& resolvedWeights,
    BonePose& pose)
{
    ZoneScopedN("ApplyBoneMorphs");
    using namespace DirectX;

    const std::size_t count = std::min(resolvedWeights.size(), set.morphs.size());
    for (std::size_t m = 0; m < count; ++m)
    {
        const Morph& morph = set.morphs[m];
        if (morph.kind != MorphKind::Bone)
        {
            continue;
        }
        const float weight = resolvedWeights[m];
        if (weight == 0.0f)
        {
            continue;
        }

        for (const BoneMorphDelta& delta : morph.boneDeltas)
        {
            if (delta.boneIndex == kInvalidBoneIndex || static_cast<std::size_t>(delta.boneIndex) >= pose.local.size())
            {
                continue;
            }

            XMMATRIX local = XMLoadFloat4x4(&pose.local[delta.boneIndex]);

            const XMMATRIX translation = XMMatrixTranslation(
                weight * delta.positionDelta[0],
                weight * delta.positionDelta[1],
                weight * delta.positionDelta[2]);
            const XMVECTOR rotation = XMQuaternionSlerp(
                XMQuaternionIdentity(),
                XMQuaternionNormalize(XMVectorSet(
                    delta.rotationDelta[0], delta.rotationDelta[1], delta.rotationDelta[2], delta.rotationDelta[3])),
                weight);
            const XMMATRIX offset = XMMatrixMultiply(translation, XMMatrixRotationQuaternion(rotation));

            local = XMMatrixMultiply(local, offset);
            XMStoreFloat4x4(&pose.local[delta.boneIndex], local);
        }
    }
}

void AccumulateVertexMorphDeltas(
    const MorphSet& set,
    const std::vector<float>& resolvedWeights,
    std::span<float> outDeltas)
{
    ZoneScopedN("AccumulateVertexMorphDeltas");
    const std::size_t count = std::min(resolvedWeights.size(), set.morphs.size());
    for (std::size_t m = 0; m < count; ++m)
    {
        const Morph& morph = set.morphs[m];
        if (morph.kind != MorphKind::Vertex)
        {
            continue;
        }
        const float weight = resolvedWeights[m];
        if (weight == 0.0f)
        {
            continue;
        }

        for (const VertexMorphDelta& delta : morph.vertexDeltas)
        {
            // The caller sizes outDeltas to vertexCount * 3; delta.vertexIndex is validated
            // against the vertex count when the .mmdl is read.
            const std::size_t base = static_cast<std::size_t>(delta.vertexIndex) * 3;
            outDeltas[base + 0] += weight * delta.positionDelta[0];
            outDeltas[base + 1] += weight * delta.positionDelta[1];
            outDeltas[base + 2] += weight * delta.positionDelta[2];
        }
    }
}
} // namespace MmdLab
