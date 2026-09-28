#include "Runtime/Animation/SkeletonPose.h"

#include <algorithm>
#include <cmath>
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

// Rotation that maps unit vector `from` onto unit vector `to`. Returns identity for near-parallel
// inputs and a 180-degree rotation about an arbitrary perpendicular for near-opposite inputs.
DirectX::XMVECTOR RotationBetweenUnitVectors(DirectX::XMVECTOR from, DirectX::XMVECTOR to)
{
    using namespace DirectX;
    const float dot = XMVectorGetX(XMVector3Dot(from, to));
    if (dot > 0.99999f)
    {
        return XMQuaternionIdentity();
    }
    if (dot < -0.99999f)
    {
        XMVECTOR axis = XMVector3Cross(from, XMVectorSet(1.0f, 0.0f, 0.0f, 0.0f));
        if (XMVectorGetX(XMVector3LengthSq(axis)) < 1e-6f)
        {
            axis = XMVector3Cross(from, XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f));
        }
        return XMQuaternionRotationAxis(XMVector3Normalize(axis), XM_PI);
    }
    // q = (1 + dot, from x to), normalized.
    return XMQuaternionNormalize(XMVectorSetW(XMVector3Cross(from, to), 1.0f + dot));
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

// Single-link IK ("LookAt" / damped track): rotate the link bone so the target bone points at the
// IK control bone. The toe chains use it: rotating the ankle aims the toe at the toe-IK control.
void SolveLookAtIk(const IkChain& chain, const Skeleton& skeleton,
    const std::vector<DirectX::XMMATRIX>& local, std::vector<DirectX::XMMATRIX>& world)
{
    using namespace DirectX;

    const std::size_t count = skeleton.bones.size();
    const std::uint16_t link = chain.links[0];
    const std::uint16_t target = chain.targetBoneIndex;
    const std::uint16_t ik = chain.ikBoneIndex;
    if (link >= count || target >= count || ik >= count)
    {
        return;
    }

    const XMVECTOR pivot = world[link].r[3];   // The link's head (the ankle).
    const XMVECTOR tip = world[target].r[3];   // The target's head (the toe).
    const XMVECTOR goal = world[ik].r[3];      // Where the target should point.

    const XMVECTOR currentDelta = tip - pivot;
    const XMVECTOR desiredDelta = goal - pivot;
    if (XMVectorGetX(XMVector3LengthSq(currentDelta)) < 1e-10f
        || XMVectorGetX(XMVector3LengthSq(desiredDelta)) < 1e-10f)
    {
        return; // Degenerate toe or target; nothing to aim.
    }

    // Rotate the link so the pivot -> tip direction aligns with pivot -> goal.
    const XMVECTOR currentDir = XMVector3Normalize(currentDelta);
    const XMVECTOR desiredDir = XMVector3Normalize(desiredDelta);
    const XMMATRIX delta = XMMatrixRotationQuaternion(
        RotationBetweenUnitVectors(currentDir, desiredDir));

    XMMATRIX newLink = world[link];
    newLink.r[3] = XMVectorSet(0.0f, 0.0f, 0.0f, 1.0f);
    newLink = XMMatrixMultiply(newLink, delta);
    newLink.r[3] = pivot;
    world[link] = newLink;

    PropagateWorld(skeleton, local, world, link, kInvalidBoneIndex);
}

// Two-link IK (analytic two-bone solve): [joint, upper] reaches its target so the end bone lands
// on the IK bone's position; the knee bends toward the thigh's local -X axis (the forward
// direction the auto-roll establishes).
void SolveTwoBoneIk(const IkChain& chain, const Skeleton& skeleton, const BindPose& bindPose,
    const std::vector<DirectX::XMMATRIX>& local, std::vector<DirectX::XMMATRIX>& world)
{
    using namespace DirectX;

    const std::size_t count = skeleton.bones.size();
    const std::uint16_t upper = chain.links[1];      // thigh
    const std::uint16_t joint = chain.links[0];      // knee
    const std::uint16_t end = chain.targetBoneIndex; // ankle
    const std::uint16_t ik = chain.ikBoneIndex;
    if (upper >= count || joint >= count || end >= count || ik >= count)
    {
        return;
    }

    const XMVECTOR rootPos = world[upper].r[3];
    const XMVECTOR jointPos = world[joint].r[3];
    const XMVECTOR endPos = world[end].r[3];
    const XMVECTOR effector = world[ik].r[3];

    const float upperLen = XMVectorGetX(XMVector3Length(jointPos - rootPos));
    const float lowerLen = XMVectorGetX(XMVector3Length(endPos - jointPos));
    const float maxLen = upperLen + lowerLen;

    const XMVECTOR desiredDelta = effector - rootPos;
    float desiredLength = XMVectorGetX(XMVector3Length(desiredDelta));
    XMVECTOR desiredDir;
    if (desiredLength < 1e-5f)
    {
        desiredLength = 1e-5f;
        desiredDir = XMVectorSet(1.0f, 0.0f, 0.0f, 0.0f);
    }
    else
    {
        desiredDir = desiredDelta / desiredLength;
    }

    // Pole: the knee bend direction. The auto-roll points the thigh's bind local +X toward the
    // model's back, so the knee bends toward -X ("forward"). Using the bind axis (not the
    // animated world X) keeps the pole fixed as the thigh's VMD rotation swings, so the knee
    // cannot flip side to side; it still follows the body's orientation via the parent chain.
    const std::uint16_t upperParent = skeleton.bones[upper].parentIndex;
    const XMMATRIX upperParentWorld = (upperParent != kInvalidBoneIndex
        && static_cast<std::size_t>(upperParent) < count)
        ? world[static_cast<std::size_t>(upperParent)]
        : XMMatrixIdentity();
    const XMVECTOR bindX = XMLoadFloat4x4(&bindPose.bindRotation[upper]).r[0];
    const XMVECTOR pole = XMVector3Normalize(XMVector3TransformNormal(XMVectorNegate(bindX), upperParentWorld));

    XMVECTOR bendDir = pole - XMVector3Dot(pole, desiredDir) * desiredDir;
    if (XMVectorGetX(XMVector3LengthSq(bendDir)) < 1e-10f)
    {
        // The pole is collinear with the reach direction (knee/elbow pointing straight at the
        // target): pick a stable perpendicular instead. XMVector3Orthogonal chooses the best axis
        // up front, mirroring Unreal's FindBestAxisVectors fallback rather than a hardcoded cross.
        bendDir = XMVector3Orthogonal(desiredDir);
    }
    else
    {
        bendDir = XMVector3Normalize(bendDir);
    }

    XMVECTOR outJointPos;
    XMVECTOR outEndPos;
    if (desiredLength >= maxLen)
    {
        outEndPos = rootPos + maxLen * desiredDir;
        outJointPos = rootPos + upperLen * desiredDir;
    }
    else
    {
        const float twoAB = 2.0f * upperLen * desiredLength;
        const float cosAngle = (twoAB != 0.0f)
            ? (upperLen * upperLen + desiredLength * desiredLength - lowerLen * lowerLen) / twoAB
            : 0.0f;
        const float angle = std::acos(std::clamp(cosAngle, -1.0f, 1.0f));
        const float jointLineDist = upperLen * std::sin(angle);
        const float projSq = upperLen * upperLen - jointLineDist * jointLineDist;
        float projDist = projSq > 0.0f ? std::sqrt(projSq) : 0.0f;
        if (cosAngle < 0.0f)
        {
            projDist = -projDist;
        }
        outJointPos = rootPos + projDist * desiredDir + jointLineDist * bendDir;
        outEndPos = effector;
    }

    // Upper (thigh): rotate so its local +Y points root -> joint, keep the root position.
    const XMVECTOR oldUpperDir = XMVector3Normalize(jointPos - rootPos);
    const XMVECTOR newUpperDir = XMVector3Normalize(outJointPos - rootPos);
    const XMMATRIX deltaUpper = XMMatrixRotationQuaternion(
        RotationBetweenUnitVectors(oldUpperDir, newUpperDir));
    XMMATRIX newUpper = world[upper];
    newUpper.r[3] = XMVectorSet(0.0f, 0.0f, 0.0f, 1.0f);
    newUpper = XMMatrixMultiply(newUpper, deltaUpper);
    newUpper.r[3] = rootPos;

    // Joint (knee): rotate so its local +Y points joint -> end, position at the new joint.
    const XMVECTOR oldJointDir = XMVector3Normalize(endPos - jointPos);
    const XMVECTOR newJointDir = XMVector3Normalize(outEndPos - outJointPos);
    const XMMATRIX deltaJoint = XMMatrixRotationQuaternion(
        RotationBetweenUnitVectors(oldJointDir, newJointDir));
    XMMATRIX newJoint = world[joint];
    newJoint.r[3] = XMVectorSet(0.0f, 0.0f, 0.0f, 1.0f);
    newJoint = XMMatrixMultiply(newJoint, deltaJoint);
    newJoint.r[3] = outJointPos;

    // End (ankle): keep its rotation, move its head to the effector.
    XMMATRIX newEnd = world[end];
    newEnd.r[3] = outEndPos;

    world[upper] = newUpper;
    world[joint] = newJoint;
    world[end] = newEnd;

    // Re-propagate the descendants of the three adjusted bones.
    PropagateWorld(skeleton, local, world, end, kInvalidBoneIndex);
    PropagateWorld(skeleton, local, world, joint, end);
    PropagateWorld(skeleton, local, world, upper, joint);
}

// Solves the IK chains the asset declared, dispatching to the solver that matches the chain
// length. A 1-link chain is a LookAt (aim), a 2-link chain is an analytic two-bone solve; other
// lengths are not yet supported. `ikEnabled` is parallel to `skeleton.ikChains`; a null pointer
// solves every chain, otherwise a false entry skips that chain.
void SolveIk(const Skeleton& skeleton, const BindPose& bindPose,
    const std::vector<DirectX::XMMATRIX>& local, std::vector<DirectX::XMMATRIX>& world,
    const std::vector<bool>* ikEnabled)
{
    for (std::size_t i = 0; i < skeleton.ikChains.size(); ++i)
    {
        if (ikEnabled != nullptr && i < ikEnabled->size() && !(*ikEnabled)[i])
        {
            continue;
        }
        const IkChain& chain = skeleton.ikChains[i];
        if (chain.links.size() == 1)
        {
            SolveLookAtIk(chain, skeleton, local, world);
        }
        else if (chain.links.size() == 2)
        {
            SolveTwoBoneIk(chain, skeleton, bindPose, local, world);
        }
    }
}

// Projects a bone's rotation onto its fixed axis (PMX FixedAxis / 軸制限). Twist bones may only
// rotate around the limb's longitudinal axis, so the off-axis part of the VMD rotation is dropped.
void ApplyFixedAxis(const Skeleton& skeleton, const BindPose& bindPose,
    const std::vector<DirectX::XMMATRIX>& local, std::vector<DirectX::XMMATRIX>& world)
{
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
    using namespace DirectX;

    const std::size_t count = skeleton.bones.size();
    outPalette.resize(count);
    scratchWorld.resize(count);

    const bool hasMotion = motionPose != nullptr && motionPose->local.size() == count;

    // Load the per-bone local transforms (motion or bind) so the IK pass can re-propagate
    // descendants from them after adjusting a chain.
    std::vector<XMMATRIX> local(count);
    for (std::size_t i = 0; i < count; ++i)
    {
        local[i] = hasMotion
            ? XMLoadFloat4x4(&motionPose->local[i])
            : XMLoadFloat4x4(&bindPose.localBind[i]);
    }

    // Forward kinematics: parent-relative local transforms accumulated up the hierarchy.
    for (std::size_t i = 0; i < count; ++i)
    {
        const Bone& bone = skeleton.bones[i];
        const XMMATRIX parentWorld = (bone.parentIndex != kInvalidBoneIndex && static_cast<std::size_t>(bone.parentIndex) < count)
            ? scratchWorld[static_cast<std::size_t>(bone.parentIndex)]
            : XMMatrixIdentity();
        scratchWorld[i] = XMMatrixMultiply(local[i], parentWorld);
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
    for (std::size_t i = 0; i < count; ++i)
    {
        const XMMATRIX inverseBind = XMLoadFloat4x4(&bindPose.inverseBind[i]);
        XMStoreFloat4x4(&outPalette[i], XMMatrixMultiply(inverseBind, scratchWorld[i]));
    }
}
} // namespace MmdLab
