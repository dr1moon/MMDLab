#include "Runtime/Animation/SkeletonPose.h"

#include "tracy/Tracy.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <string_view>

namespace MmdLab
{
namespace
{
// The shoulder, arm, elbow, wrist, and finger bones (plus the arm twist/dummy bones) use
// Blender's dedicated "auto local axis" roll, mirroring mmd_tools' has_auto_local_axis sets
// (AUTO_LOCAL_AXIS_ARMS + AUTO_LOCAL_AXIS_SEMI_STANDARD_ARMS).
bool HasAutoLocalAxis(const std::string& name)
{
    static const std::string_view arms[] = {
        "左肩", "左腕", "左ひじ", "左手首", "右腕", "右肩", "右ひじ", "右手首",
        "左腕捩", "左手捩", "左肩P", "左ダミー", "右腕捩", "右手捩", "右肩P", "右ダミー",
    };
    for (const std::string_view arm : arms)
    {
        if (name == arm)
        {
            return true;
        }
    }
    static const std::string_view fingers[] = { "親指", "人指", "中指", "薬指", "小指" };
    for (const std::string_view finger : fingers)
    {
        if (name.find(finger) != std::string::npos)
        {
            return true;
        }
    }
    return false;
}

// Builds the bind rotation for a bone. Bones with explicit local axes (PMX LocalCoordinate)
// use them directly (X and Z stored, Y = Z x X); the rest point their local +Y along the
// head -> tail direction with a stable default roll, falling back to +Y for a degenerate tail.
DirectX::XMMATRIX BindRotation(const Bone& bone)
{
    using namespace DirectX;

    XMVECTOR x;
    XMVECTOR y;
    XMVECTOR z;

    if (bone.hasLocalAxes)
    {
        x = XMVector3Normalize(XMVectorSet(bone.localX[0], bone.localX[1], bone.localX[2], 0.0f));
        z = XMVector3Normalize(XMVectorSet(bone.localZ[0], bone.localZ[1], bone.localZ[2], 0.0f));
        y = XMVector3Normalize(XMVector3Cross(z, x));
        z = XMVector3Normalize(XMVector3Cross(x, y)); // Re-orthonormalize (x, y, z).
    }
    else
    {
        y = XMVectorSet(
            bone.tail[0] - bone.position[0],
            bone.tail[1] - bone.position[1],
            bone.tail[2] - bone.position[2],
            0.0f);
        const XMVECTOR length = XMVector3Length(y);
        if (XMVectorGetX(length) < 1e-5f)
        {
            y = XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);
        }
        else
        {
            y = XMVector3Normalize(y);
        }

        if (HasAutoLocalAxis(bone.name) && std::fabs(XMVectorGetZ(y)) <= 0.999f)
        {
            // Auto roll (Blender's update_auto_bone_roll): the local +Z is the head -> tail
            // direction rotated -90 degrees in the horizontal plane, with one Z-axis sign flip to
            // match the runtime's handedness, then +X = +Y x +Z.
            z = XMVector3Normalize(XMVectorSet(-XMVectorGetY(y), XMVectorGetX(y), 0.0f, 0.0f));
            x = XMVector3Cross(y, z);
        }
        else
        {
            // Default roll: keep the local +X aligned with the model's +X (right), then derive +Z
            // right-handed. A +Z fallback keeps the cross products well-conditioned when the bone
            // points along +X.
            XMVECTOR xReference = XMVectorSet(1.0f, 0.0f, 0.0f, 0.0f);
            if (std::fabs(XMVectorGetX(XMVector3Dot(y, xReference))) > 0.999f)
            {
                xReference = XMVectorSet(0.0f, 0.0f, 1.0f, 0.0f);
            }

            x = XMVector3Normalize(XMVector3Cross(y, XMVector3Cross(xReference, y)));
            z = XMVector3Cross(x, y); // Makes (x, y, z) right-handed.
        }
    }

    XMMATRIX basis = XMMatrixIdentity();
    basis.r[0] = x;
    basis.r[1] = y;
    basis.r[2] = z;
    basis.r[3] = XMVectorSet(0.0f, 0.0f, 0.0f, 1.0f);
    return basis;
}

// Recomputes world matrices for `bone`'s descendants (skipping `skip`) after an IK solve changed
// `bone`'s world matrix. `local` holds the per-bone local transforms, parallel to Skeleton::bones.
void PropagateWorld(const Skeleton& skeleton, const std::vector<DirectX::XMMATRIX>& local,
    std::vector<DirectX::XMMATRIX>& world, const std::uint16_t bone, const std::uint16_t skip)
{
    if (bone >= skeleton.children.size())
    {
        return;
    }
    for (const std::uint16_t child : skeleton.children[bone])
    {
        if (child == skip)
        {
            continue;
        }
        world[child] = DirectX::XMMatrixMultiply(local[child], world[bone]);
        PropagateWorld(skeleton, local, world, child, kInvalidBoneIndex);
    }
}

// The bind world transform of `bone` (bind rotation about the absolute head position), or the
// identity for kInvalidBoneIndex.
DirectX::XMMATRIX BindWorld(const Skeleton& skeleton, const BindPose& bindPose, const std::uint16_t bone)
{
    using namespace DirectX;
    if (bone == kInvalidBoneIndex || bone >= skeleton.bones.size())
    {
        return XMMatrixIdentity();
    }
    XMMATRIX world = XMLoadFloat4x4(&bindPose.bindRotation[bone]);
    const float* head = skeleton.bones[bone].position;
    world.r[3] = XMVectorSet(head[0], head[1], head[2], 1.0f);
    return world;
}

// The bone's world transform in the MMD frame: the runtime bind rotation removed, so the axes are
// the model axes carried through the animated parent chain. PMX IK angle limits and VMD rotations
// are both authored in this frame, so the IK solver works in it.
DirectX::XMMATRIX MmdWorld(const BindPose& bindPose, const std::vector<DirectX::XMMATRIX>& world,
    const std::uint16_t bone)
{
    using namespace DirectX;
    return XMMatrixMultiply(XMMatrixTranspose(XMLoadFloat4x4(&bindPose.bindRotation[bone])), world[bone]);
}

constexpr float kPi = 3.14159265358979f;
constexpr float kTwoPi = 2.0f * kPi;

float NormalizeAngle(float angle)
{
    angle = std::fmod(angle, kTwoPi);
    return angle < 0.0f ? angle + kTwoPi : angle;
}

float DiffAngle(const float a, const float b)
{
    const float diff = NormalizeAngle(a) - NormalizeAngle(b);
    if (diff > kPi)
    {
        return diff - kTwoPi;
    }
    if (diff < -kPi)
    {
        return diff + kTwoPi;
    }
    return diff;
}

// The Euler rotation the IK limit clamp uses: Z applied first, then Y, then X (the column-vector
// Rx * Ry * Rz of the reference MMD runtimes, written here in row-vector order).
DirectX::XMVECTOR QuaternionFromEulerXyz(const DirectX::XMFLOAT3& euler)
{
    using namespace DirectX;
    return XMQuaternionRotationMatrix(XMMatrixMultiply(
        XMMatrixMultiply(XMMatrixRotationZ(euler.z), XMMatrixRotationY(euler.y)), XMMatrixRotationX(euler.x)));
}

// Decomposes a rotation into the Euler angles of QuaternionFromEulerXyz. Of the equivalent
// solutions, returns the one closest to `before` so the per-iteration limit clamp stays
// continuous; at the gimbal singularity (y = +-90 degrees) x is pinned to zero first.
DirectX::XMFLOAT3 DecomposeEulerXyz(const DirectX::XMMATRIX& rotation, const DirectX::XMFLOAT3& before)
{
    using namespace DirectX;
    XMFLOAT4X4 m;
    XMStoreFloat4x4(&m, rotation);

    // Row-vector Rz * Ry * Rx: m[2][0] = sin(y), m[2][1] = -sin(x)cos(y), m[2][2] = cos(x)cos(y),
    // m[1][0] = -cos(y)sin(z), m[0][0] = cos(y)cos(z).
    XMFLOAT3 r;
    const float sy = std::clamp(m.m[2][0], -1.0f, 1.0f);
    r.y = std::asin(sy);
    if (1.0f - std::fabs(sy) < 1e-6f)
    {
        r.x = 0.0f;
        r.z = std::atan2(m.m[0][1], m.m[1][1]);
    }
    else
    {
        r.x = std::atan2(-m.m[2][1], m.m[2][2]);
        r.z = std::atan2(-m.m[1][0], m.m[0][0]);
    }

    const auto Error = [&](const XMFLOAT3& candidate)
    {
        return std::fabs(DiffAngle(candidate.x, before.x)) + std::fabs(DiffAngle(candidate.y, before.y))
            + std::fabs(DiffAngle(candidate.z, before.z));
    };
    const XMFLOAT3 candidates[] = {
        { r.x + kPi, kPi - r.y, r.z + kPi }, { r.x + kPi, kPi - r.y, r.z - kPi },
        { r.x + kPi, -kPi - r.y, r.z + kPi }, { r.x + kPi, -kPi - r.y, r.z - kPi },
        { r.x - kPi, kPi - r.y, r.z + kPi }, { r.x - kPi, kPi - r.y, r.z - kPi },
        { r.x - kPi, -kPi - r.y, r.z + kPi }, { r.x - kPi, -kPi - r.y, r.z - kPi },
    };
    float bestError = Error(r);
    for (const XMFLOAT3& candidate : candidates)
    {
        const float error = Error(candidate);
        if (error < bestError)
        {
            bestError = error;
            r = candidate;
        }
    }
    return r;
}

// A link free on exactly one axis with the other two locked (the standard knee: X only) is solved
// as a hinge about that axis instead of by the Euler clamp, which stalls on a straight limb. The
// "locked" test (min or max is zero) follows the reference MMD runtimes.
int HingeAxisIndex(const IkLink& link)
{
    if (!link.hasLimit)
    {
        return -1;
    }
    const auto Free = [&](const int axis) { return link.limitMin[axis] != 0.0f || link.limitMax[axis] != 0.0f; };
    const auto Locked = [&](const int axis) { return link.limitMin[axis] == 0.0f || link.limitMax[axis] == 0.0f; };
    for (int axis = 0; axis < 3; ++axis)
    {
        if (Free(axis) && Locked((axis + 1) % 3) && Locked((axis + 2) % 3))
        {
            return axis;
        }
    }
    return -1;
}

// Per-link solver state. `rotation` is the link's MMD local rotation (its VMD rotation with the
// IK correction folded in); `position` is its head plus VMD offset in bind model space, which IK
// never changes.
struct IkLinkState
{
    std::uint16_t bone = kInvalidBoneIndex;
    DirectX::XMFLOAT4 animRotation;
    DirectX::XMFLOAT4 rotation;
    DirectX::XMFLOAT4 savedRotation;
    DirectX::XMFLOAT3 position;
    DirectX::XMFLOAT3 previousEuler = { 0.0f, 0.0f, 0.0f };
    float hingeAngle = 0.0f;
};

// Rebuilds a link's local and world transforms from its MMD local rotation and re-propagates its
// descendants. local = R_bind * R_mmd * T(position) * inverseBind(parent), the composition the
// VMD animator uses, so the palette stays inverseBind * world.
void WriteIkLink(const Skeleton& skeleton, const BindPose& bindPose, const IkLinkState& state,
    std::vector<DirectX::XMMATRIX>& local, std::vector<DirectX::XMMATRIX>& world)
{
    using namespace DirectX;
    const std::uint16_t bone = state.bone;
    const std::uint16_t parent = skeleton.bones[bone].parentIndex;
    const bool hasParent = parent != kInvalidBoneIndex && parent < skeleton.bones.size();

    XMMATRIX posed = XMMatrixMultiply(XMLoadFloat4x4(&bindPose.bindRotation[bone]),
        XMMatrixRotationQuaternion(XMQuaternionNormalize(XMLoadFloat4(&state.rotation))));
    posed.r[3] = XMVectorSetW(XMLoadFloat3(&state.position), 1.0f);
    local[bone] = hasParent ? XMMatrixMultiply(posed, XMLoadFloat4x4(&bindPose.inverseBind[parent])) : posed;
    world[bone] = hasParent ? XMMatrixMultiply(local[bone], world[parent]) : local[bone];
    PropagateWorld(skeleton, local, world, bone, kInvalidBoneIndex);
}

// One cyclic coordinate descent (CCD) sweep over the chain, tip to root, following the PMX IK
// definition: each link turns so the target moves toward the IK bone by at most the chain's
// per-iteration angle, then the link's angle limits are applied.
void SolveIkIteration(const IkChain& chain, const Skeleton& skeleton, const BindPose& bindPose,
    std::vector<IkLinkState>& states, const int iteration,
    std::vector<DirectX::XMMATRIX>& local, std::vector<DirectX::XMMATRIX>& world)
{
    using namespace DirectX;
    const XMVECTOR ikPos = world[chain.ikBoneIndex].r[3];

    for (std::size_t k = 0; k < states.size(); ++k)
    {
        IkLinkState& state = states[k];
        const IkLink& link = chain.links[k];
        if (state.bone == chain.targetBoneIndex)
        {
            continue;
        }

        // Both points in the link's own (rotated) MMD frame, so a rotation `delta` found here
        // composes as delta * R_link. A hinge keeps R_link = hinge(angle) * R_vmd, and turns about
        // the same axis commute, so delta * R_link = hinge(angle + delta) * R_vmd exactly.
        const XMMATRIX inverseLink = XMMatrixInverse(nullptr, MmdWorld(bindPose, world, state.bone));
        const XMVECTOR toIk = XMVector3TransformCoord(ikPos, inverseLink);
        const XMVECTOR toTarget = XMVector3TransformCoord(world[chain.targetBoneIndex].r[3], inverseLink);
        if (XMVectorGetX(XMVector3LengthSq(toIk)) < 1e-12f || XMVectorGetX(XMVector3LengthSq(toTarget)) < 1e-12f)
        {
            continue;
        }
        const XMVECTOR ikDir = XMVector3Normalize(toIk);
        const XMVECTOR targetDir = XMVector3Normalize(toTarget);
        const float angle = std::min(
            std::acos(std::clamp(XMVectorGetX(XMVector3Dot(targetDir, ikDir)), -1.0f, 1.0f)),
            chain.limitAngle);

        const int hingeAxis = HingeAxisIndex(link);
        if (hingeAxis >= 0)
        {
            // Hinge: turn about the single free axis in whichever direction brings the target
            // closer, and accumulate the angle so the limit bounds the total bend. The bend is a
            // pure rotation about the limit axis, so the knee cannot twist off its hinge (no roll
            // is introduced, unlike a free swing).
            const XMVECTOR axis = XMVectorSetByIndex(XMVectorZero(), 1.0f, static_cast<std::size_t>(hingeAxis));
            const float dotPositive = XMVectorGetX(XMVector3Dot(
                XMVector3Rotate(targetDir, XMQuaternionRotationAxis(axis, angle)), ikDir));
            const float dotNegative = XMVectorGetX(XMVector3Dot(
                XMVector3Rotate(targetDir, XMQuaternionRotationAxis(axis, -angle)), ikDir));
            float newAngle = state.hingeAngle + (dotPositive > dotNegative ? angle : -angle);

            const float minAngle = link.limitMin[hingeAxis];
            const float maxAngle = link.limitMax[hingeAxis];
            if (iteration == 0 && (newAngle < minAngle || newAngle > maxAngle))
            {
                // From a nearly straight limb both bend directions look alike; take the one the
                // limit allows (the knee bends forward) instead of clamping back to straight.
                if (-newAngle > minAngle && -newAngle < maxAngle)
                {
                    newAngle = -newAngle;
                }
                else
                {
                    const float half = 0.5f * (minAngle + maxAngle);
                    if (std::fabs(half - newAngle) > std::fabs(half + newAngle))
                    {
                        newAngle = -newAngle;
                    }
                }
            }
            state.hingeAngle = std::clamp(newAngle, minAngle, maxAngle);
            XMStoreFloat4(&state.rotation, XMQuaternionMultiply(
                XMQuaternionRotationAxis(axis, state.hingeAngle), XMLoadFloat4(&state.animRotation)));
        }
        else
        {
            const XMVECTOR cross = XMVector3Cross(targetDir, ikDir);
            if (angle < 1e-5f || XMVectorGetX(XMVector3LengthSq(cross)) < 1e-12f)
            {
                continue;
            }
            XMVECTOR rotation = XMQuaternionMultiply(
                XMQuaternionRotationAxis(XMVector3Normalize(cross), angle), XMLoadFloat4(&state.rotation));
            if (link.hasLimit)
            {
                // Clamp each Euler angle to the limit, and its change this iteration to the
                // chain's per-iteration angle.
                const XMFLOAT3 euler = DecomposeEulerXyz(XMMatrixRotationQuaternion(rotation), state.previousEuler);
                const float raw[3] = { euler.x, euler.y, euler.z };
                const float before[3] = { state.previousEuler.x, state.previousEuler.y, state.previousEuler.z };
                float clamped[3];
                for (int axis = 0; axis < 3; ++axis)
                {
                    const float limited = std::clamp(raw[axis], link.limitMin[axis], link.limitMax[axis]);
                    clamped[axis] = before[axis] + std::clamp(limited - before[axis], -chain.limitAngle, chain.limitAngle);
                }
                state.previousEuler = XMFLOAT3(clamped[0], clamped[1], clamped[2]);
                rotation = QuaternionFromEulerXyz(state.previousEuler);
            }
            XMStoreFloat4(&state.rotation, rotation);
        }

        WriteIkLink(skeleton, bindPose, state, local, world);
    }
}

// Solves one PMX IK chain with CCD, starting from the animated pose. Each iteration that brings
// the target closer is kept; the first one that does not is rolled back and ends the solve.
void SolveIkChain(const IkChain& chain, const Skeleton& skeleton, const BindPose& bindPose,
    std::vector<IkLinkState>& states, std::vector<DirectX::XMMATRIX>& local,
    std::vector<DirectX::XMMATRIX>& world)
{
    ZoneScopedN("Animation.IK.Chain");
    using namespace DirectX;
    const std::size_t count = skeleton.bones.size();
    if (chain.ikBoneIndex >= count || chain.targetBoneIndex >= count || chain.links.empty())
    {
        return;
    }

    states.clear();
    for (const IkLink& link : chain.links)
    {
        if (link.boneIndex >= count)
        {
            return;
        }
        // Recover the VMD rotation and head position from the local transform:
        // local * bindWorld(parent) = R_bind * R_vmd * T(position).
        const std::uint16_t parent = skeleton.bones[link.boneIndex].parentIndex;
        const XMMATRIX posed = XMMatrixMultiply(local[link.boneIndex], BindWorld(skeleton, bindPose, parent));
        const XMMATRIX bindRotation = XMLoadFloat4x4(&bindPose.bindRotation[link.boneIndex]);

        IkLinkState state;
        state.bone = link.boneIndex;
        XMStoreFloat4(&state.animRotation, XMQuaternionNormalize(XMQuaternionRotationMatrix(
            XMMatrixMultiply(XMMatrixTranspose(bindRotation), posed))));
        state.rotation = state.animRotation;
        state.savedRotation = state.animRotation;
        XMStoreFloat3(&state.position, posed.r[3]);
        states.push_back(state);
    }

    float bestDistance = std::numeric_limits<float>::max();
    for (int iteration = 0; iteration < chain.loopCount; ++iteration)
    {
        SolveIkIteration(chain, skeleton, bindPose, states, iteration, local, world);

        const float distance = XMVectorGetX(XMVector3Length(
            world[chain.targetBoneIndex].r[3] - world[chain.ikBoneIndex].r[3]));
        if (distance < bestDistance)
        {
            bestDistance = distance;
            for (IkLinkState& state : states)
            {
                state.savedRotation = state.rotation;
            }
            continue;
        }

        // Root to tip, so each link is rebuilt on its already-restored parent.
        for (auto it = states.rbegin(); it != states.rend(); ++it)
        {
            it->rotation = it->savedRotation;
            WriteIkLink(skeleton, bindPose, *it, local, world);
        }
        break;
    }
}

// Solves the IK chains the asset declared, in declaration order. `ikEnabled` is parallel to
// `skeleton.ikChains`; a null pointer solves every chain, otherwise a false entry skips that
// chain. The link local transforms are rewritten so the later passes build on the solved pose.
void SolveIk(const Skeleton& skeleton, const BindPose& bindPose,
    std::vector<DirectX::XMMATRIX>& local, std::vector<DirectX::XMMATRIX>& world,
    const std::vector<bool>* ikEnabled)
{
    ZoneScopedN("Animation.IK");
    std::vector<IkLinkState> states;
    for (std::size_t i = 0; i < skeleton.ikChains.size(); ++i)
    {
        if (ikEnabled != nullptr && i < ikEnabled->size() && !(*ikEnabled)[i])
        {
            continue;
        }
        SolveIkChain(skeleton.ikChains[i], skeleton, bindPose, states, local, world);
    }
}

// Projects a bone's rotation onto its fixed axis (PMX FixedAxis / 軸制限). Twist bones may only
// rotate around the limb's longitudinal axis, so the off-axis part of the VMD rotation is dropped.
void ApplyFixedAxis(const Skeleton& skeleton, const BindPose& bindPose,
    const std::vector<DirectX::XMMATRIX>& local, std::vector<DirectX::XMMATRIX>& world)
{
    ZoneScopedN("Animation.FixedAxis");
    using namespace DirectX;
    const std::size_t count = skeleton.bones.size();
    for (std::size_t i = 0; i < count; ++i)
    {
        const Bone& bone = skeleton.bones[i];
        if (!bone.hasFixedAxis)
        {
            continue;
        }

        const std::uint16_t parent = bone.parentIndex;
        const XMMATRIX parentWorld = (parent != kInvalidBoneIndex && static_cast<std::size_t>(parent) < count)
            ? world[static_cast<std::size_t>(parent)]
            : XMMatrixIdentity();

        const XMMATRIX ownLocal = XMMatrixMultiply(world[i], XMMatrixInverse(nullptr, parentWorld));
        const XMVECTOR ownPos = ownLocal.r[3];

        // Its VMD rotation (model space): the local rotation with the bind removed and the
        // parent's bind restored.
        const XMVECTOR ownLocalQuat = XMQuaternionRotationMatrix(ownLocal);
        const XMVECTOR bindQuat = XMQuaternionRotationMatrix(XMLoadFloat4x4(&bindPose.bindRotation[i]));
        const XMVECTOR parentBindQuat = (parent != kInvalidBoneIndex && static_cast<std::size_t>(parent) < count)
            ? XMQuaternionRotationMatrix(XMLoadFloat4x4(&bindPose.bindRotation[static_cast<std::size_t>(parent)]))
            : XMQuaternionIdentity();
        const XMVECTOR vmdQuat = XMQuaternionMultiply(
            XMQuaternionMultiply(XMQuaternionInverse(bindQuat), ownLocalQuat), parentBindQuat);

        // Decompose into axis-angle and keep only the component along the fixed axis.
        const XMVECTOR v = XMVectorSetW(vmdQuat, 0.0f);
        const float vLen = XMVectorGetX(XMVector3Length(v));
        if (vLen < 1e-6f)
        {
            continue; // Near-identity rotation; nothing to project.
        }
        const XMVECTOR axis = v / vLen;
        const float angle = 2.0f * std::acos(std::clamp(XMVectorGetW(vmdQuat), -1.0f, 1.0f));
        const XMVECTOR fixedAxis = XMVector3Normalize(
            XMVectorSet(bone.fixedAxis[0], bone.fixedAxis[1], bone.fixedAxis[2], 0.0f));
        const XMVECTOR projectedVmd = XMQuaternionRotationAxis(
            fixedAxis, angle * XMVectorGetX(XMVector3Dot(axis, fixedAxis)));

        const XMVECTOR newLocalQuat = XMQuaternionMultiply(
            XMQuaternionMultiply(bindQuat, projectedVmd), XMQuaternionInverse(parentBindQuat));
        XMMATRIX newLocal = XMMatrixRotationQuaternion(XMQuaternionNormalize(newLocalQuat));
        newLocal.r[3] = ownPos;
        world[i] = XMMatrixMultiply(newLocal, parentWorld);
        PropagateWorld(skeleton, local, world, static_cast<std::uint16_t>(i), kInvalidBoneIndex);
    }
}

// Applies the "付与" translation (移動付与): the bone's world position shifts by a fraction of the
// inherit parent's VMD translation offset (its world position minus its bind head).
void ApplyInheritTranslation(const Skeleton& skeleton,
    const std::vector<DirectX::XMMATRIX>& local, std::vector<DirectX::XMMATRIX>& world)
{
    ZoneScopedN("Animation.InheritTranslation");
    using namespace DirectX;
    const std::size_t count = skeleton.bones.size();
    for (std::size_t i = 0; i < count; ++i)
    {
        const Bone& bone = skeleton.bones[i];
        if (!bone.hasInheritTranslation || bone.inheritParentIndex >= count)
        {
            continue;
        }

        const Bone& mainBone = skeleton.bones[bone.inheritParentIndex];
        const XMVECTOR mainPos = world[bone.inheritParentIndex].r[3];
        const XMVECTOR mainBindHead = XMVectorSet(
            mainBone.position[0], mainBone.position[1], mainBone.position[2], 0.0f);
        const XMVECTOR mainVmdOffset = mainPos - mainBindHead;

        world[i].r[3] = world[i].r[3] + mainVmdOffset * bone.inheritInfluence;
        PropagateWorld(skeleton, local, world, static_cast<std::uint16_t>(i), kInvalidBoneIndex);
    }
}

// Applies the "付与" (grant) inheritance: each bone with the InheritRotation flag rotates by a
// scaled copy of its inherit-parent's VMD rotation (influence in 0..1, or negative for the
// inverse). Deform ("D") shadow bones copy the animation bone's rotation this way, and the arm
// twist bones split one twist across 0.25/0.5/0.75. Runs in index order so a deform bone's
// parent has already received its own grant.
void ApplyInheritRotation(const Skeleton& skeleton, const BindPose& bindPose,
    const std::vector<DirectX::XMMATRIX>& local, std::vector<DirectX::XMMATRIX>& world)
{
    ZoneScopedN("Animation.InheritRotation");
    using namespace DirectX;

    const std::size_t count = skeleton.bones.size();
    for (std::size_t i = 0; i < count; ++i)
    {
        const Bone& bone = skeleton.bones[i];
        if (!bone.hasInheritRotation || bone.inheritParentIndex >= count)
        {
            continue;
        }

        // The inherit parent's VMD rotation: its local rotation (relative to its parent) with
        // its own bind removed and the parent's bind restored, leaving only the animation.
        const std::uint16_t main = bone.inheritParentIndex;
        const Bone& mainBone = skeleton.bones[main];
        const std::uint16_t mainParent = mainBone.parentIndex;
        const XMMATRIX mainParentWorld = (mainParent != kInvalidBoneIndex
            && static_cast<std::size_t>(mainParent) < count)
            ? world[static_cast<std::size_t>(mainParent)]
            : XMMatrixIdentity();
        const XMVECTOR mainLocalQuat = XMQuaternionRotationMatrix(
            XMMatrixMultiply(world[main], XMMatrixInverse(nullptr, mainParentWorld)));
        const XMVECTOR mainBindQuat = XMQuaternionRotationMatrix(XMLoadFloat4x4(&bindPose.bindRotation[main]));
        const XMVECTOR mainParentBindQuat = (mainParent != kInvalidBoneIndex
            && static_cast<std::size_t>(mainParent) < count)
            ? XMQuaternionRotationMatrix(XMLoadFloat4x4(&bindPose.bindRotation[static_cast<std::size_t>(mainParent)]))
            : XMQuaternionIdentity();
        const XMVECTOR mainVmdQuat = XMQuaternionMultiply(
            XMQuaternionMultiply(XMQuaternionInverse(mainBindQuat), mainLocalQuat),
            mainParentBindQuat);

        const XMVECTOR grantQuat = bone.inheritInfluence >= 0.0f
            ? XMQuaternionSlerp(XMQuaternionIdentity(), mainVmdQuat, bone.inheritInfluence)
            : XMQuaternionSlerp(XMQuaternionIdentity(), XMQuaternionInverse(mainVmdQuat), -bone.inheritInfluence);

        // Conjugate the grant into the bone's own frame and prepend it to the local transform, so
        // the granted rotation pivots about the bone's head in model space.
        const XMMATRIX bindRotation = XMLoadFloat4x4(&bindPose.bindRotation[i]);
        const XMMATRIX grantMatrix = XMMatrixRotationQuaternion(XMQuaternionNormalize(grantQuat));
        const XMMATRIX conjugated = XMMatrixMultiply(
            XMMatrixMultiply(bindRotation, grantMatrix), XMMatrixInverse(nullptr, bindRotation));
        const XMMATRIX newLocal = XMMatrixMultiply(conjugated, local[i]);

        const XMMATRIX parentWorld = (bone.parentIndex != kInvalidBoneIndex
            && static_cast<std::size_t>(bone.parentIndex) < count)
            ? world[static_cast<std::size_t>(bone.parentIndex)]
            : XMMatrixIdentity();
        world[i] = XMMatrixMultiply(newLocal, parentWorld);
        PropagateWorld(skeleton, local, world, static_cast<std::uint16_t>(i), kInvalidBoneIndex);
    }
}
} // namespace

BindPose BuildBindPose(const Skeleton& skeleton)
{
    using namespace DirectX;

    BindPose bind;
    const std::size_t count = skeleton.bones.size();
    bind.localBind.resize(count);
    bind.bindRotation.resize(count);
    bind.inverseBind.resize(count);

    // The PMX bone position is absolute (model space), not a parent-relative offset, so the
    // world transform is the bind rotation composed with the absolute translation, with no parent
    // accumulation. The parent-relative local transform (which the VMD animator builds on) is the
    // world transform mapped back into the parent's frame.
    std::vector<XMMATRIX> world(count);
    for (std::size_t i = 0; i < count; ++i)
    {
        const Bone& bone = skeleton.bones[i];

        const XMMATRIX translation = XMMatrixTranslation(bone.position[0], bone.position[1], bone.position[2]);
        const XMMATRIX rotation = BindRotation(bone);
        XMStoreFloat4x4(&bind.bindRotation[i], rotation);
        // Row-vector: rotate about the bone head, then translate to the absolute head position.
        world[i] = XMMatrixMultiply(rotation, translation);

        const XMMATRIX parentWorld = (bone.parentIndex != kInvalidBoneIndex && static_cast<std::size_t>(bone.parentIndex) < count)
            ? world[static_cast<std::size_t>(bone.parentIndex)]
            : XMMatrixIdentity();
        const XMMATRIX inverseParent = XMMatrixInverse(nullptr, parentWorld);
        XMStoreFloat4x4(&bind.localBind[i], XMMatrixMultiply(world[i], inverseParent));

        const XMMATRIX inverse = XMMatrixInverse(nullptr, world[i]);
        XMStoreFloat4x4(&bind.inverseBind[i], inverse);
    }

    return bind;
}

void EvaluateSkeletonPose(
    const Skeleton& skeleton,
    const BindPose& bindPose,
    const BonePose* motionPose,
    std::vector<DirectX::XMFLOAT4X4>& outPalette,
    std::vector<DirectX::XMMATRIX>& scratchWorld,
    const std::vector<bool>* ikEnabled)
{
    ZoneScopedN("Animation.EvaluateSkeletonPose");
    using namespace DirectX;

    const std::size_t count = skeleton.bones.size();
    outPalette.resize(count);
    scratchWorld.resize(count);

    const bool hasMotion = motionPose != nullptr && motionPose->local.size() == count;

    // Load the per-bone local transforms (motion or bind) so the IK pass can re-propagate
    // descendants from them after adjusting a chain.
    std::vector<XMMATRIX> local(count);
    {
        ZoneScopedN("Animation.Evaluate.LoadLocalPose");
        for (std::size_t i = 0; i < count; ++i)
        {
            local[i] = hasMotion
                ? XMLoadFloat4x4(&motionPose->local[i])
                : XMLoadFloat4x4(&bindPose.localBind[i]);
        }
    }

    // Forward kinematics: parent-relative local transforms accumulated up the hierarchy.
    {
        ZoneScopedN("Animation.Evaluate.ForwardKinematics");
        for (std::size_t i = 0; i < count; ++i)
        {
            const Bone& bone = skeleton.bones[i];
            const XMMATRIX parentWorld = (bone.parentIndex != kInvalidBoneIndex && static_cast<std::size_t>(bone.parentIndex) < count)
                ? scratchWorld[static_cast<std::size_t>(bone.parentIndex)]
                : XMMatrixIdentity();
            scratchWorld[i] = XMMatrixMultiply(local[i], parentWorld);
        }
    }

    // Solve the IK chains on top of the animated pose (the bind pose needs no solving).
    if (hasMotion)
    {
        SolveIk(skeleton, bindPose, local, scratchWorld, ikEnabled);
    }

    // Apply the per-bone constraints and "付与" grants. Axis constraints run first so the
    // rotation grants copy the already-constrained twist rotation; translation grants follow.
    ApplyFixedAxis(skeleton, bindPose, local, scratchWorld);
    ApplyInheritTranslation(skeleton, local, scratchWorld);
    ApplyInheritRotation(skeleton, bindPose, local, scratchWorld);

    // Skinning matrix = inverseBind * world: re-project the model-space vertex into the bone's
    // bind space, then out through the (possibly IK-adjusted) animated world transform.
    {
        ZoneScopedN("Animation.Evaluate.BuildSkinningPalette");
        for (std::size_t i = 0; i < count; ++i)
        {
            const XMMATRIX inverseBind = XMLoadFloat4x4(&bindPose.inverseBind[i]);
            XMStoreFloat4x4(&outPalette[i], XMMatrixMultiply(inverseBind, scratchWorld[i]));
        }
    }
}
} // namespace MmdLab
