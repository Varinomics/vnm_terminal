# vnm_terminal Documentation

This directory contains stable orientation and reference material.

## First-Read Path

1. [Developer orientation](developer_orientation.md)
2. [Architecture](architecture.md)
3. [Public surface](public_surface.md)
4. [Repository guide](repository_guide.md)

These files are written for a first-time engineer that needs to navigate
the repository without reading every implementation detail.

## Time-Budgeted Reading

- 1 minute: read the top-level `README.md`.
- 5 minutes: read [Developer orientation](developer_orientation.md).
- 15 minutes: read [Architecture](architecture.md) and
  [Public surface](public_surface.md).
- Before changing build, tests, fixtures, or dependencies: read
  [Repository guide](repository_guide.md) plus the relevant reference material
  below.
- Before changing terminal escape behavior: read
  [Terminal sequence matrix](terminal_sequence_matrix.md).
- Before importing or comparing against external terminal projects: read
  [Terminal conformance oracles](terminal_conformance_oracles.md) and
  [Terminal reference inventory](terminal_reference_inventory.md).
- Before changing Unicode width behavior: read
  [Unicode width policy](unicode_width_policy.md).
- Before changing Qt rendering dependencies or text rendering: read
  [Qt rendering policy](qt_rendering_policy.md).

## Reference Material

- [Terminal sequence matrix](terminal_sequence_matrix.md) records supported,
  ignored, and rejected terminal sequences.
- [Terminal conformance oracles](terminal_conformance_oracles.md) records the
  conformance policy and reference-source boundaries.
- [Terminal reference inventory](terminal_reference_inventory.md) records
  reference projects and licensing posture.
- [Unicode width policy](unicode_width_policy.md) records generated table
  policy, source data, and terminal cell-width rules.
- [Qt rendering policy](qt_rendering_policy.md) records Qt module, licensing,
  public API, and Scene Graph constraints.
- [Repository guide](repository_guide.md) records supported platforms, build
  options, fixture formats, generated artifacts, and operational entry points.

## Maintenance Rule

Documentation should describe stable contracts, policy, repository structure,
and supported operational workflows. Keep temporary working notes outside this
directory.
