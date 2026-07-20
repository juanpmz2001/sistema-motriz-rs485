# Configuration Schemas

`robot-profile.schema.json` is the normative authoring and transport shape for
robot profile schema `1.0.0`.

Current firmware does not implement this schema yet. Until `PROF-002..005` are
complete, passing desktop JSON Schema validation does not make a document
activatable. Firmware must additionally validate IDs/references, hardware
capabilities, GPIO/resources, kinematic compatibility, SVD48 catalog completeness
and compiled safety ceilings.
