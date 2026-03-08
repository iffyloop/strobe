# Effects Order in SDF Point-Warping

This app uses SDF point-warping, not a traditional vertex pipeline.

## Why this is confusing at first

In a traditional vertex/object pipeline, people usually think in object transform order:

- Scale
- Rotate
- Translate

That is often written as an object/world transform like `T * R * S` (matrix conventions vary, but the intent is the same).

In SDF point-warping, we do not move vertices. We evaluate the SDF at a sample point `p`, and to evaluate a transformed shape we transform **the point** into the shape's local space.

That means we must apply the **inverse transform chain**.

If forward object transform is:

`M = T * R * S`

then local sampling uses:

`p_local = M^-1 * p_world = S^-1 * R^-1 * T^-1 * p_world`

So translation must be undone before rotation, and rotation before scale in the point-warp path.

## What the app does now

To keep the UI familiar for people coming from traditional non-SDF graphics, effects are shown and created in **SRT order**:

1. Scale
2. Rotate
3. Translate

During compile, the app reverses effect order internally before emitting effect ops for shader evaluation.

So:

- **UI order (authoring):** SRT
- **Evaluation order (internal point-warp):** TRS inverse application sequence

This gives familiar editing ergonomics while preserving correct SDF behavior.
