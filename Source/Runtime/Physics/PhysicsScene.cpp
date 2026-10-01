#include "Runtime/Physics/PhysicsScene.h"

#include "tracy/Tracy.hpp"

#include <btBulletDynamicsCommon.h>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <vector>

namespace MmdLab
{
namespace
{
// MMD's length unit is roughly 8 cm, and it scales gravity to match (9.8 m/s^2 * 10).
constexpr float kGravity = -98.0f;
// MMD steps its physics at a fixed 120 Hz, catching up with at most this many sub-steps per
// frame; a longer hitch is dropped instead of simulated.
constexpr float kFixedTimeStep = 1.0f / 120.0f;
constexpr int kMaxSubSteps = 10;

// XMMATRIX is row-vector with the basis in rows; btTransform is column-vector with the basis in
// columns. The two share one memory layout, which is exactly the OpenGL column-major layout
// Bullet reads and writes.
btTransform ToBullet(const DirectX::XMMATRIX& matrix)
{
    DirectX::XMFLOAT4X4 stored;
    DirectX::XMStoreFloat4x4(&stored, matrix);
    btTransform transform;
    transform.setFromOpenGLMatrix(&stored.m[0][0]);
    return transform;
}

DirectX::XMMATRIX FromBullet(const btTransform& transform)
{
    DirectX::XMFLOAT4X4 stored;
    transform.getOpenGLMatrix(&stored.m[0][0]);
    return DirectX::XMLoadFloat4x4(&stored);
}

// A PMX rigid-body or joint transform: Euler angles applied Z, then X, then Y (MMD's order,
// which is DirectXMath's roll-pitch-yaw), then the model-space position. PMX stores both in the
// model's native frame, the same frame the skeleton is evaluated in, so no handedness flip.
DirectX::XMMATRIX PmxTransform(const float (&position)[3], const float (&rotation)[3])
{
    using namespace DirectX;
    XMMATRIX transform = XMMatrixRotationRollPitchYaw(rotation[0], rotation[1], rotation[2]);
    transform.r[3] = XMVectorSet(position[0], position[1], position[2], 1.0f);
    return transform;
}

std::unique_ptr<btCollisionShape> CreateShape(const BodySetup& setup)
{
    switch (setup.shape)
    {
    case BodyShape::Sphere:
        return std::make_unique<btSphereShape>(setup.size[0]);
    case BodyShape::Box:
        return std::make_unique<btBoxShape>(btVector3(setup.size[0], setup.size[1], setup.size[2]));
    case BodyShape::Capsule:
        return std::make_unique<btCapsuleShape>(setup.size[0], setup.size[1]);
    }
    return std::make_unique<btSphereShape>(setup.size[0]);
}

btVector3 ToBullet(const float (&values)[3])
{
    return btVector3(values[0], values[1], values[2]);
}

// Applies a joint's limits and per-axis springs to a six-degree-of-freedom constraint.
void ConfigureSpring(btGeneric6DofSpringConstraint& constraint, const ConstraintSetup& setup)
{
    constraint.setLinearLowerLimit(ToBullet(setup.linearLowerLimit));
    constraint.setLinearUpperLimit(ToBullet(setup.linearUpperLimit));
    constraint.setAngularLowerLimit(ToBullet(setup.angularLowerLimit));
    constraint.setAngularUpperLimit(ToBullet(setup.angularUpperLimit));
    for (int axis = 0; axis < 3; ++axis)
    {
        if (setup.linearStiffness[axis] != 0.0f)
        {
            constraint.enableSpring(axis, true);
            constraint.setStiffness(axis, setup.linearStiffness[axis]);
        }
        if (setup.angularStiffness[axis] != 0.0f)
        {
            constraint.enableSpring(axis + 3, true);
            constraint.setStiffness(axis + 3, setup.angularStiffness[axis]);
        }
    }
}

// The ground plane's collision group: above the sixteen PMX groups, so no PMX mask names it.
constexpr int kGroundGroup = 1 << 16;

// Broadphase pair filter. The ground collides with every simulated body whatever its PMX mask
// (as in MMD); two static or kinematic bodies never pair, since neither can respond (Bullet
// would otherwise narrowphase them and warn); every other pair uses the PMX group/mask test in
// both directions.
struct PmxOverlapFilter final : btOverlapFilterCallback
{
    bool needBroadphaseCollision(btBroadphaseProxy* proxyA, btBroadphaseProxy* proxyB) const override
    {
        const auto* objectA = static_cast<const btCollisionObject*>(proxyA->m_clientObject);
        const auto* objectB = static_cast<const btCollisionObject*>(proxyB->m_clientObject);
        if (objectA->isStaticOrKinematicObject() && objectB->isStaticOrKinematicObject())
        {
            return false;
        }
        if (((proxyA->m_collisionFilterGroup | proxyB->m_collisionFilterGroup) & kGroundGroup) != 0)
        {
            return true;
        }
        return (proxyA->m_collisionFilterGroup & proxyB->m_collisionFilterMask) != 0
            && (proxyB->m_collisionFilterGroup & proxyA->m_collisionFilterMask) != 0;
    }
};
} // namespace

struct PhysicsScene::Impl
{
    struct Body
    {
        BodyMode mode = BodyMode::FollowBone;
        BodyShape shapeKind = BodyShape::Sphere;
        float size[3] = { 0.0f, 0.0f, 0.0f };
        std::uint16_t bone = kInvalidBoneIndex;
        DirectX::XMFLOAT4X4 offset;        // Body world = offset * bone world.
        DirectX::XMFLOAT4X4 inverseOffset; // Bone world = inverseOffset * body world.
        std::unique_ptr<btCollisionShape> shape;
        std::unique_ptr<btDefaultMotionState> motionState;
        std::unique_ptr<btRigidBody> rigidBody;
    };

    const Skeleton& skeleton;
    std::unique_ptr<btDefaultCollisionConfiguration> configuration;
    std::unique_ptr<btCollisionDispatcher> dispatcher;
    std::unique_ptr<btDbvtBroadphase> broadphase;
    std::unique_ptr<btSequentialImpulseConstraintSolver> solver;
    std::unique_ptr<btDiscreteDynamicsWorld> world;
    std::vector<Body> bodies;
    std::vector<std::unique_ptr<btGeneric6DofSpringConstraint>> constraints;

    // A kinematic anchor that a self-joint (bodyA == bodyB) constrains its body to. MMD uses a
    // self-joint as a "補助" (auxiliary) spring that pulls a body back toward its own bone; the
    // anchor sits exactly on that bone and tracks it each frame, so the spring follows the bone.
    struct Anchor
    {
        std::uint16_t bone = kInvalidBoneIndex;
        std::unique_ptr<btCollisionShape> shape;
        std::unique_ptr<btDefaultMotionState> motionState;
        std::unique_ptr<btRigidBody> rigidBody;
    };
    std::vector<Anchor> anchors;

    PmxOverlapFilter overlapFilter;
    std::unique_ptr<btStaticPlaneShape> groundShape;
    std::unique_ptr<btRigidBody> ground;
    bool groundEnabled = false;

    // Bones in parent-before-child order, the simulated body driving each bone (or -1), and
    // whether a bone is simulated or descends from a simulated bone.
    std::vector<std::uint16_t> order;
    std::vector<std::int32_t> boneBody;
    std::vector<bool> affected;
    std::vector<DirectX::XMMATRIX> relative; // Scratch: animated parent-relative transforms.
    bool initialized = false;

    explicit Impl(const Skeleton& source)
        : skeleton(source)
    {
    }

    ~Impl()
    {
        for (const auto& constraint : constraints)
        {
            world->removeConstraint(constraint.get());
        }
        for (const Body& body : bodies)
        {
            world->removeRigidBody(body.rigidBody.get());
        }
        for (const Anchor& anchor : anchors)
        {
            world->removeRigidBody(anchor.rigidBody.get());
        }
        if (groundEnabled)
        {
            world->removeRigidBody(ground.get());
        }
    }

    DirectX::XMMATRIX BoneWorld(const std::vector<DirectX::XMMATRIX>& poses, const std::uint16_t bone) const
    {
        return bone < poses.size() ? poses[bone] : DirectX::XMMatrixIdentity();
    }
};

PhysicsScene::PhysicsScene(const PhysicsAsset& asset, const Skeleton& skeleton, const BindPose& bindPose)
    : impl_(std::make_unique<Impl>(skeleton))
{
    ZoneScopedN("Physics.Build");
    using namespace DirectX;
    Impl& impl = *impl_;

    impl.configuration = std::make_unique<btDefaultCollisionConfiguration>();
    impl.dispatcher = std::make_unique<btCollisionDispatcher>(impl.configuration.get());
    impl.broadphase = std::make_unique<btDbvtBroadphase>();
    impl.solver = std::make_unique<btSequentialImpulseConstraintSolver>();
    impl.world = std::make_unique<btDiscreteDynamicsWorld>(
        impl.dispatcher.get(), impl.broadphase.get(), impl.solver.get(), impl.configuration.get());
    impl.world->setGravity(btVector3(0.0f, kGravity, 0.0f));
    impl.world->getPairCache()->setOverlapFilterCallback(&impl.overlapFilter);

    // The floor at y = 0 (MMD's ground), so cloth and hair do not sink through it.
    impl.groundShape = std::make_unique<btStaticPlaneShape>(btVector3(0.0f, 1.0f, 0.0f), 0.0f);
    impl.ground = std::make_unique<btRigidBody>(
        btRigidBody::btRigidBodyConstructionInfo(0.0f, nullptr, impl.groundShape.get()));

    const std::size_t boneCount = skeleton.bones.size();
    impl.bodies.reserve(asset.bodies.size());
    for (const BodySetup& setup : asset.bodies)
    {
        Impl::Body body;
        body.mode = setup.mode;
        body.shapeKind = setup.shape;
        for (int axis = 0; axis < 3; ++axis)
        {
            body.size[axis] = setup.size[axis];
        }
        body.bone = setup.boneIndex < boneCount ? setup.boneIndex : kInvalidBoneIndex;

        const XMMATRIX bodyBind = PmxTransform(setup.position, setup.rotation);
        const XMMATRIX offset = body.bone != kInvalidBoneIndex
            ? XMMatrixMultiply(bodyBind, XMLoadFloat4x4(&bindPose.inverseBind[body.bone]))
            : bodyBind;
        XMStoreFloat4x4(&body.offset, offset);
        XMStoreFloat4x4(&body.inverseOffset, XMMatrixInverse(nullptr, offset));

        body.shape = CreateShape(setup);
        const bool kinematic = setup.mode == BodyMode::FollowBone;
        const btScalar mass = kinematic ? 0.0f : setup.mass;
        btVector3 inertia(0.0f, 0.0f, 0.0f);
        if (mass != 0.0f)
        {
            body.shape->calculateLocalInertia(mass, inertia);
        }
        body.motionState = std::make_unique<btDefaultMotionState>(ToBullet(bodyBind));

        btRigidBody::btRigidBodyConstructionInfo info(mass, body.motionState.get(), body.shape.get(), inertia);
        info.m_linearDamping = setup.linearDamping;
        info.m_angularDamping = setup.angularDamping;
        info.m_restitution = setup.restitution;
        info.m_friction = setup.friction;
        info.m_additionalDamping = true;
        body.rigidBody = std::make_unique<btRigidBody>(info);
        if (kinematic)
        {
            body.rigidBody->setCollisionFlags(
                body.rigidBody->getCollisionFlags() | btCollisionObject::CF_KINEMATIC_OBJECT);
        }
        body.rigidBody->setActivationState(DISABLE_DEACTIVATION);
        body.rigidBody->setSleepingThresholds(0.01f, XMConvertToRadians(0.1f));

        // PMX: bit g of the mask set = collides with group g; PmxOverlapFilter tests it both ways.
        impl.world->addRigidBody(body.rigidBody.get(),
            static_cast<int>(1u << setup.group), static_cast<int>(setup.collisionMask));
        impl.bodies.push_back(std::move(body));
    }

    impl.constraints.reserve(asset.constraints.size());
    for (const ConstraintSetup& setup : asset.constraints)
    {
        if (setup.bodyA >= impl.bodies.size() || setup.bodyB >= impl.bodies.size())
        {
            continue;
        }

        // The joint frame in model space at bind pose, shared by both paths below.
        const btTransform joint = ToBullet(PmxTransform(setup.position, setup.rotation));

        // A self-joint (bodyA == bodyB) is MMD's "connect a body to its own bone" idiom: a
        // "補助" (auxiliary) spring that pulls the body back toward its animated bone. Bullet
        // cannot connect a body to itself, so constrain it to a kinematic anchor that tracks the
        // body's bone instead.
        if (setup.bodyA == setup.bodyB)
        {
            const Impl::Body& body = impl.bodies[setup.bodyA];
            if (body.bone == kInvalidBoneIndex)
            {
                continue; // No bone to anchor to; the joint is degenerate.
            }

            Impl::Anchor anchor;
            anchor.bone = body.bone;
            anchor.shape = std::make_unique<btSphereShape>(0.0f); // Never collides.
            const XMMATRIX boneBind = XMMatrixInverse(nullptr, XMLoadFloat4x4(&bindPose.inverseBind[anchor.bone]));
            anchor.motionState = std::make_unique<btDefaultMotionState>(ToBullet(boneBind));
            btRigidBody::btRigidBodyConstructionInfo info(0.0f, anchor.motionState.get(), anchor.shape.get());
            anchor.rigidBody = std::make_unique<btRigidBody>(info);
            anchor.rigidBody->setCollisionFlags(
                anchor.rigidBody->getCollisionFlags() | btCollisionObject::CF_KINEMATIC_OBJECT);
            anchor.rigidBody->setActivationState(DISABLE_DEACTIVATION);
            impl.world->addRigidBody(anchor.rigidBody.get(), 0, 0); // Group/mask 0: collides with nothing.

            btRigidBody& bodyRigid = *impl.bodies[setup.bodyA].rigidBody;
            const btTransform frameBody = bodyRigid.getWorldTransform().inverse() * joint;
            const btTransform frameAnchor = anchor.rigidBody->getWorldTransform().inverse() * joint;
            auto constraint = std::make_unique<btGeneric6DofSpringConstraint>(
                bodyRigid, *anchor.rigidBody, frameBody, frameAnchor, true);
            ConfigureSpring(*constraint, setup);
            impl.world->addConstraint(constraint.get());
            impl.constraints.push_back(std::move(constraint));
            impl.anchors.push_back(std::move(anchor));
            continue;
        }

        btRigidBody& bodyA = *impl.bodies[setup.bodyA].rigidBody;
        btRigidBody& bodyB = *impl.bodies[setup.bodyB].rigidBody;
        const btTransform frameA = bodyA.getWorldTransform().inverse() * joint;
        const btTransform frameB = bodyB.getWorldTransform().inverse() * joint;

        auto constraint = std::make_unique<btGeneric6DofSpringConstraint>(bodyA, bodyB, frameA, frameB, true);
        ConfigureSpring(*constraint, setup);
        impl.world->addConstraint(constraint.get());
        impl.constraints.push_back(std::move(constraint));
    }

    // Parent-before-child traversal, and the bones the simulation drives. When several simulated
    // bodies share a bone, the last one wins, as in MMD.
    impl.order.reserve(boneCount);
    std::vector<std::uint16_t> stack;
    for (std::size_t i = boneCount; i-- > 0;)
    {
        const std::uint16_t parent = skeleton.bones[i].parentIndex;
        if (parent == kInvalidBoneIndex || parent >= boneCount)
        {
            stack.push_back(static_cast<std::uint16_t>(i));
        }
    }
    while (!stack.empty())
    {
        const std::uint16_t bone = stack.back();
        stack.pop_back();
        impl.order.push_back(bone);
        const std::span<const std::uint16_t> children = skeleton.Children(bone);
        stack.insert(stack.end(), children.rbegin(), children.rend());
    }

    impl.boneBody.assign(boneCount, -1);
    for (std::size_t b = 0; b < impl.bodies.size(); ++b)
    {
        const Impl::Body& body = impl.bodies[b];
        if (body.mode != BodyMode::FollowBone && body.bone != kInvalidBoneIndex)
        {
            impl.boneBody[body.bone] = static_cast<std::int32_t>(b);
        }
    }
    impl.affected.assign(boneCount, false);
    for (const std::uint16_t bone : impl.order)
    {
        const std::uint16_t parent = skeleton.bones[bone].parentIndex;
        impl.affected[bone] = impl.boneBody[bone] >= 0 || (parent < boneCount && impl.affected[parent]);
    }
    impl.relative.resize(boneCount);
    SetGroundEnabled(true);
}

PhysicsScene::~PhysicsScene() = default;

void PhysicsScene::SetGroundEnabled(const bool enabled)
{
    Impl& impl = *impl_;
    if (enabled == impl.groundEnabled)
    {
        return;
    }
    if (enabled)
    {
        impl.world->addRigidBody(impl.ground.get(), kGroundGroup, 0);
    }
    else
    {
        impl.world->removeRigidBody(impl.ground.get());
    }
    impl.groundEnabled = enabled;
}

bool PhysicsScene::GroundEnabled() const
{
    return impl_->groundEnabled;
}

std::size_t PhysicsScene::BodyCount() const
{
    return impl_->bodies.size();
}

void PhysicsScene::Reset(const std::vector<DirectX::XMMATRIX>& world)
{
    ZoneScopedN("Physics.Reset");
    using namespace DirectX;
    Impl& impl = *impl_;
    for (Impl::Body& body : impl.bodies)
    {
        const btTransform transform = ToBullet(
            XMMatrixMultiply(XMLoadFloat4x4(&body.offset), impl.BoneWorld(world, body.bone)));
        body.rigidBody->setWorldTransform(transform);
        body.rigidBody->setInterpolationWorldTransform(transform);
        body.motionState->setWorldTransform(transform);
        body.rigidBody->setLinearVelocity(btVector3(0.0f, 0.0f, 0.0f));
        body.rigidBody->setAngularVelocity(btVector3(0.0f, 0.0f, 0.0f));
        body.rigidBody->setInterpolationLinearVelocity(btVector3(0.0f, 0.0f, 0.0f));
        body.rigidBody->setInterpolationAngularVelocity(btVector3(0.0f, 0.0f, 0.0f));
        body.rigidBody->clearForces();
    }
    impl.initialized = true;
}

void PhysicsScene::Simulate(const float deltaSeconds, std::vector<DirectX::XMMATRIX>& world)
{
    ZoneScopedN("Physics.Simulate");
    using namespace DirectX;
    Impl& impl = *impl_;
    const std::size_t boneCount = impl.skeleton.bones.size();
    if (world.size() != boneCount || impl.bodies.empty())
    {
        return;
    }
    if (!impl.initialized)
    {
        Reset(world);
    }

    // Drive the kinematic bodies from the animated pose. Bullet reads a kinematic body's target
    // from its motion state and derives the velocity that pushes the simulated bodies.
    for (Impl::Body& body : impl.bodies)
    {
        if (body.mode == BodyMode::FollowBone)
        {
            body.motionState->setWorldTransform(ToBullet(
                XMMatrixMultiply(XMLoadFloat4x4(&body.offset), impl.BoneWorld(world, body.bone))));
        }
    }
    // Drive the self-joint anchors the same way; each sits exactly on its bone, so the auxiliary
    // spring tracks the animated bone as the model moves.
    for (Impl::Anchor& anchor : impl.anchors)
    {
        anchor.motionState->setWorldTransform(ToBullet(impl.BoneWorld(world, anchor.bone)));
    }

    if (deltaSeconds > 0.0f)
    {
        ZoneScopedN("Physics.Step");
        impl.world->stepSimulation(deltaSeconds, kMaxSubSteps, kFixedTimeStep);
    }

    // Capture the animated parent-relative transforms of the affected bones before any of them
    // is overwritten, then rebuild them parent-first on top of the simulated bones.
    for (const std::uint16_t bone : impl.order)
    {
        const std::uint16_t parent = impl.skeleton.bones[bone].parentIndex;
        if (impl.affected[bone] && parent < boneCount)
        {
            impl.relative[bone] = XMMatrixMultiply(world[bone], XMMatrixInverse(nullptr, world[parent]));
        }
    }
    for (const std::uint16_t bone : impl.order)
    {
        if (!impl.affected[bone])
        {
            continue;
        }
        const std::uint16_t parent = impl.skeleton.bones[bone].parentIndex;
        const bool parentAffected = parent < boneCount && impl.affected[parent];
        const XMMATRIX animated = parentAffected ? XMMatrixMultiply(impl.relative[bone], world[parent]) : world[bone];

        const std::int32_t bodyIndex = impl.boneBody[bone];
        if (bodyIndex < 0)
        {
            world[bone] = animated;
            continue;
        }
        const Impl::Body& body = impl.bodies[static_cast<std::size_t>(bodyIndex)];
        btTransform simulated;
        body.motionState->getWorldTransform(simulated);
        XMMATRIX boneWorld = XMMatrixMultiply(XMLoadFloat4x4(&body.inverseOffset), FromBullet(simulated));
        if (body.mode == BodyMode::PhysicsWithBonePosition)
        {
            boneWorld.r[3] = animated.r[3];
        }
        world[bone] = boneWorld;
    }
}

DirectX::XMMATRIX PhysicsScene::BodyWorld(const std::size_t index) const
{
    btTransform transform;
    impl_->bodies[index].motionState->getWorldTransform(transform);
    return FromBullet(transform);
}

void PhysicsScene::WriteDebugBodies(const std::span<PhysicsDebugBody> out) const
{
    const std::size_t count = std::min(out.size(), impl_->bodies.size());
    for (std::size_t i = 0; i < count; ++i)
    {
        const Impl::Body& body = impl_->bodies[i];
        PhysicsDebugBody& debug = out[i];
        btTransform transform;
        body.motionState->getWorldTransform(transform);
        DirectX::XMStoreFloat4x4(&debug.world, FromBullet(transform));
        debug.shape = body.shapeKind;
        debug.mode = body.mode;
        for (int axis = 0; axis < 3; ++axis)
        {
            debug.size[axis] = body.size[axis];
        }
    }
}
} // namespace MmdLab
