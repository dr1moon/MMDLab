# Physics self-joint assert on Yae Miko (八重神子)

## Summary

The viewer aborts during startup when the selected level contains the Yae Miko
(八重神子) model. Bullet's `btDiscreteDynamicsWorld::addConstraint` hits this
assert:

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

A joint whose `bodyA == bodyB` (a self-joint) is never filtered out. Each stage
of the pipeline checks that the body indices are in range, but not that they are
distinct:

- `Source/Runtime/Asset/PmxFile.cpp` (PMX -> `.mmdl` joint parse): guards
  `bodyA < 0 || bodyB < 0 || bodyA >= bodyCount || bodyB >= bodyCount` and
  otherwise stores the joint.
- `Source/Runtime/Asset/MmdlFile.cpp` (`.mmdl` -> `PhysicsAsset`): guards
  `constraint.bodyA >= bodyCount || constraint.bodyB >= bodyCount`.
- `Source/Runtime/Physics/PhysicsScene.cpp` (the Bullet constraint pass):
  guards `setup.bodyA >= impl.bodies.size() || setup.bodyB >= impl.bodies.size()`
  (line 237), then does:

  ```cpp
  btRigidBody& bodyA = *impl.bodies[setup.bodyA].rigidBody;  // line 241
  btRigidBody& bodyB = *impl.bodies[setup.bodyB].rigidBody;  // line 242
  ...
  impl.world->addConstraint(constraint.get());               // line 267 -> assert
  ```

  When `setup.bodyA == setup.bodyB`, `bodyA` and `bodyB` are the same object and
  `addConstraint` asserts.

The trigger model is data-dependent: Yae Miko declares 347 rigid bodies and 458
joints (see the `[Physics]` startup line), and one of those joints has
`bodyA == bodyB`. The previously exercised physics model, Hu Tao (胡桃), has no
such joint, so the case was never hit.

## Suggested fix

Decide whether a self-joint is invalid input to skip, or a data-cooking bug to
fix at the source. The safest place to guard is the Bullet pass, since it is the
last stage and protects the runtime regardless of where the bad joint came from:

```cpp
// PhysicsScene.cpp, in the constraint loop (after the bounds check):
if (setup.bodyA == setup.bodyB)
{
    continue;
}
```

Optionally also skip self-joints when parsing the PMX (`PmxFile.cpp`) and when
reading the `.mmdl` (`MmdlFile.cpp`), so the cooked asset is already clean.
Note the `.mmdl` version bump this would imply if the reader/writer behaviour is
changed.

## Repro

1. Build `Debug`.
2. Run the viewer with `Project` as the working directory and select a level
   containing Yae Miko (八重神子), e.g. `MmdViewer.exe --frames 600`.
3. The process aborts (exit code 3) shortly after the physics line
   `[Physics] '八重神子': 347 rigid bodies, 458 joints` appears.
