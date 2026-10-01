# Physics self-joint: a body constrained to its own bone

## Summary

The viewer aborted during startup when the selected model set contained a
self-joint. Bullet's `btDiscreteDynamicsWorld::addConstraint` hits this assert:

```cpp
void btDiscreteDynamicsWorld::addConstraint(btTypedConstraint* constraint, bool disableCollisionsBetweenLinkedBodies)
{
    m_constraints.push_back(constraint);
    //Make sure the two bodies of a type constraint are different (possibly add this to the btTypedConstraint constructor?)
    btAssert(&constraint->getRigidBodyA() != &constraint->getRigidBodyB());
```

The assert fires because one joint resolves its two body indices to the same
`btRigidBody`. On Windows the process then terminates with exit code 3, with no
log line.

## Root cause

A self-joint (`bodyA == bodyB`) is not invalid input; it is MMD's idiom for
"connect a rigid body to its own bone". Such joints are named with a "補"
(auxiliary) suffix and act as a spring that pulls a body back toward the bone it
is attached to. The affected bodies are "錘" (weight/sinker) rigid bodies in
`Physics` mode, each also chained to a parent body by a normal joint; the
self-joint adds the spring-back to the bone so hair, ribbons, and cloth sway
instead of sagging.

The affected models (verified in both the source `.pmx` and the cooked `.mmdl`):

- `尼可/尼可`: 1 self-joint (右DressLetA01補).
- `沃雅妮莎/沃雅妮莎`: 9 self-joints.
- `沃雅妮莎/沃雅妮莎_人鱼`: 10 self-joints.

Yae Miko (八重神子) is **not** affected (347 rigid bodies, 458 joints, 0
self-joints). An earlier version of this note mis-attributed the crash to it;
the viewer loads every model under `Models/` in parallel, so the assert was
fired by one of the models above while Yae Miko's `[Physics]` line happened to
be the last one logged.

The runtime passed `bodyA == bodyB` straight into
`btGeneric6DofSpringConstraint` (`Source/Runtime/Physics/PhysicsScene.cpp`),
which Bullet rejects because a constraint must join two distinct bodies.

## Fix

`PhysicsScene` now implements the body-to-bone semantics. When it meets a
self-joint, it builds a kinematic anchor body that sits exactly on the joint's
bone, adds it to the world with collision group/mask 0 (so it collides with
nothing), and constrains the body to that anchor with the joint's 6-DOF spring.
The anchor is driven from the animated pose in `Simulate`, alongside the
`FollowBone` bodies, so the auxiliary spring tracks the bone as the model moves
(see `btRigidBody::saveKinematicState`, which pulls a kinematic body's transform
back from its motion state each step).

If a self-joint body has no bone (`kInvalidBoneIndex`), the joint is degenerate
and is dropped; no model in the project hits this case.

## Repro

1. Build `Debug`.
2. Run `MmdViewer.exe` with `Project` as the working directory and a frame
   limit, e.g. `MmdViewer.exe --frames 120`.
3. The viewer loads and simulates all models, including the three above, and
   exits cleanly instead of aborting.

## See also

[Physics-Conversion-Rules.md](Physics-Conversion-Rules.md) is the full catalog
of PMX-to-Bullet conversion rules; this self-joint case is one entry in it.
