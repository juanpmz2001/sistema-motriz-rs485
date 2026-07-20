# Robot Profile Examples

Files in this directory demonstrate the canonical JSON shape.

`robot-profile-differential-2wd-one-svd48.json` confirms only this topology:

- one SVD48;
- M1 and M2 control the two traction motors; channel-to-side assignment remains
  unverified;
- two driven wheels and two passive casters;
- differential kinematics.

It is intentionally `status:"draft"` and `activation_allowed:false`. Numeric
channel mapping, dimensions, signs, pinout, electrical limits, RC mapping and controller parameter
maps are placeholders until measured on the physical robot. Do not flip the flag
without completing the inventory and elevated validation in
`docs/process/04_OFF_GROUND_TEST_MATRIX.md`.
