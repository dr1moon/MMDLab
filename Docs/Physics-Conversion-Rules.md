# Physics conversion rules (PMX to Bullet)

This document codifies how a PMX model's rigid bodies and joints map to the
Bullet physics objects in `Source/Runtime/Physics/PhysicsScene.cpp`. The rules
are drawn from the MikuMikuDance (MMD) data conventions and from mmd_tools, the
Blender add-on at `D:\blender_mmd_tools` (not part of this repository), which is
the de-facto reference implementation of PMX-to-Bullet conversion: Blender's
rigid-body world is Bullet, so its mapping is the closest thing to a canonical
specification. Where MMDLab deviates or is still undecided, the status column
says so.

## Coordinate frame

PMX stores positions and Euler rotations in a Y-up, Z-forward frame, the same
frame MMDLab's skeleton and DirectXMath evaluation use. No handedness or axis
permutation is applied on the physics path.

mmd_tools applies a `.xzy` axis permutation and sign flips to convert PMX into
Blender's Z-up, Y-forward frame (and swaps the angular minimum/maximum limits as
a consequence). That conversion is a property of Blender's frame, not of the
format, and does not apply to MMDLab.

Rotations are Euler angles in radians applied Z, then X, then Y
(roll-pitch-yaw), MMD's order. `PhysicsScene::PmxTransform` expresses this with
`XMMatrixRotationRollPitchYaw`.

## Rigid body

One PMX rigid body becomes one `btRigidBody` with a `btCollisionShape` and a
`btDefaultMotionState`.

| PMX field | Meaning | mmd_tools | MMDLab | Status |
|---|---|---|---|---|
| Shape byte 0/1/2 | Sphere / box / capsule | `SPHERE`/`BOX`/`CAPSULE` | `BodyShape` | Done |
| Mode byte 0 | Static: the body is kinematic and follows its bone | `MODE_STATIC`, `kinematic=True` | `BodyMode::FollowBone`, `CF_KINEMATIC_OBJECT`, zero mass | Done |
| Mode byte 1 | Dynamic: simulated; drives its bone's position and rotation | `MODE_DYNAMIC`, `COPY_TRANSFORMS` | `BodyMode::Physics` | Done |
| Mode byte 2 | Dynamic bone: simulated; drives its bone's rotation only, keeping the animated position | `MODE_DYNAMIC_BONE`, `COPY_ROTATION` | `BodyMode::PhysicsWithBonePosition` | Done |
| Bone index `-1` | No bone (attach to the model root) | `None` | `kInvalidBoneIndex` | Done |
| Mass | Body mass; 0 for a kinematic body | `rb.mass` | `btRigidBodyConstructionInfo` mass | Done |
| Linear damping | `velocity_attenuation` | `rb.linear_damping` | `info.m_linearDamping` | Done |
| Angular damping | `rotation_attenuation` | `rb.angular_damping` | `info.m_angularDamping` | Done |
| Restitution | `bounce` | `rb.restitution` | `info.m_restitution` | Done |
| Friction | `friction` | `rb.friction` | `info.m_friction` | Done |
| Collision group 0-15 | The body's own group | `collision_group_number` | `1u << group` as the Bullet filter group | Done |
| Collision mask, 16 bits | Bit g set = collides with group g | Inverted to a per-group ignore list | Passed as the Bullet filter mask | Done |

### Collision pairing

Two bodies collide only when each side's mask names the other side's group:

```cpp
(groupA & maskB) != 0 && (groupB & maskA) != 0
```

`PmxOverlapFilter` in `PhysicsScene.cpp` implements this test in both
directions, and additionally forces a collision with the ground plane
(`kGroundGroup`, bit 16) regardless of the PMX masks, as MMD does. Static or
kinematic pairs never collide. mmd_tools reaches the same outcome by building
explicit non-collision constraints; the bidirectional test above is the direct
equivalent.

## Joint

One PMX joint becomes one `btGeneric6DofSpringConstraint`. The joint frame is
`position`/`rotation` in model space at bind pose; it is converted into each
body's local frame with `bodyWorld.inverse() * joint`.

| PMX field | Meaning | MMDLab | Status |
|---|---|---|---|
| Type byte | PMX 2.0 has only 0 (`Spring6DOF`); PMX 2.1 adds 6-DOF, point-to-point, cone-twist, slider, and hinge | All types collapse to `btGeneric6DofSpringConstraint` | Done (matches mmd_tools, which ignores the type byte) |
| Body index A / B | The two rigid bodies joined | `setup.bodyA` / `setup.bodyB` | Done |
| Body index `-1` | "No body": the joint connects the other body to a fixed point in model space | Throws (`PmxFile.cpp` guards `bodyA < 0 \|\| bodyB < 0`) | Gap |
| Position / rotation | Joint frame in model space | `PmxTransform` | Done |
| Linear lower / upper limit | Translation limits along the joint frame | `setLinearLowerLimit` / `setLinearUpperLimit` | Done |
| Angular lower / upper limit | Rotation limits around the joint frame | `setAngularLowerLimit` / `setAngularUpperLimit` | Done |
| Linear spring constant | Per-axis spring stiffness; 0 = no spring | `enableSpring` + `setStiffness` when non-zero | Done |
| Angular spring constant | Per-axis spring stiffness; 0 = no spring | `enableSpring(axis + 3)` + `setStiffness` when non-zero | Done |

### Body index edge cases

A joint's two body indices are signed and may name the same body, no body, or
distinct bodies. The three cases and their intended meaning:

1. **Distinct bodies** (`bodyA != bodyB`, both valid): the normal case, a
   six-degrees-of-freedom (6-DOF) spring between two rigid bodies.
2. **Self-joint** (`bodyA == bodyB`): MMD's "connect a body to its own bone"
   idiom. The affected bodies are named with a `錘` (weight/sinker) suffix and
   the joints with a `補` (auxiliary) suffix; the joint is a spring that pulls
   the body back toward the bone it is attached to. `PhysicsScene` implements
   this by constraining the body to a kinematic anchor body that tracks the
   body's bone each frame. mmd_tools has no explicit rule for this case.
3. **No body** (`bodyA == -1` or `bodyB == -1`): this endpoint references no
   rigid body. The provisional reading is that it anchors the other body to a
   fixed point at the joint's location (a WorldAnchor), matching mmd_tools,
   which maps `-1` to an unconnected constraint side (`None`). This is a
   hypothesis, not confirmed semantics; see [Open decisions](#open-decisions).
   MMDLab currently rejects `-1` during PMX parsing.

### Constraint endpoint resolution

The three body-index cases reduce to resolving each joint side to an endpoint
type and pairing the two. The planned abstraction (only the self-joint branch is
implemented today) is:

```cpp
enum class ConstraintEndpointType
{
    RigidBody,   // A PMX rigid body.
    WorldAnchor, // A fixed point at the joint transform; PROVISIONAL for -1.
    BoneAnchor,  // The bone a rigid body is attached to (the self-joint case).
};
```

| Case | Endpoint pair | Status |
|---|---|---|
| `bodyA != bodyB`, both valid | RigidBody ↔ RigidBody | Done |
| `bodyA == bodyB` | BoneAnchor ↔ RigidBody | Done, tested |
| `bodyA == -1` | WorldAnchor ↔ RigidBody | Not implemented; hypothesis |
| `bodyB == -1` | RigidBody ↔ WorldAnchor | Not implemented; hypothesis |

## Constraint softness

MMD's Bullet 2.75 treats joints as *soft*: a body may still move elastically
even when a translation limit is locked (`minimum == maximum`). This is what
breast, hair, and cloth physics rely on. Blender's Bullet treats the same limits
as *hard*, freezing the body; mmd_tools documents this in
`mmd_tools/core/rigid_body.py` and notes that replicating MMD requires matching
its `btContactSolverInfo`, constraint-solver type, and spring parameters.

MMDLab uses `btGeneric6DofSpringConstraint` with the default
`btSequentialImpulseConstraintSolver`, which yields hard behavior at locked
limits. Matching MMD's soft behavior is a separate fidelity item, not a format
conversion gap.

## Simulation settings

The non-format constants that follow MMD's own conventions are fixed in
`PhysicsScene.cpp`: gravity `-98` units/s² (9.8 m/s² scaled by 10), a 120 Hz
fixed step with at most 10 sub-steps per frame, and a ground plane at `y = 0`.

## Open decisions

### Joint endpoint with rigid-body index == -1

Confirmed:

- PMX permits -1 as a null rigid-body reference.
- It means that this endpoint does not reference a PMX rigid body.
- blender_mmd_tools maps -1 to None and represents the constraint as having a
  fixed/world-side endpoint.
- No bone-anchor derivation is present in blender_mmd_tools.

Local corpus:

- 33 PMX files scanned.
- 0 joints use -1.
- Therefore the local corpus provides no behavioral evidence.

Current hypothesis:

- Treat -1 as a WorldAnchor at the PMX joint transform.

Status:

- NOT CONFIRMED against the reference MMD runtime.
- Do not encode this as definitive MMD semantics yet.

Future validation:

- Find a real PMX containing a -1 joint.
- Compare behavior against MMD/reference implementation.

### Soft constraints

Decide whether to replicate MMD's soft-limit behavior (custom solver info or
spring parameters) or accept hard limits for now.

### Joint types 1-5 (PMX 2.1)

Currently collapsed to a 6-DOF spring. If a model using them ever appears,
decide whether the collapse is acceptable.
