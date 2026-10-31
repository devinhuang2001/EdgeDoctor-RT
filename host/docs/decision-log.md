# Decision log

## D-001 - Independent application project

- Date: 2026-08-30
- Decision: create `EdgeDoctor-RT` as a new application project and keep the
  existing EdgePet sources and its modified BSP working copy unchanged.
- Reason: the workspace contains uncommitted EdgePet sensor work. Reusing it
  would violate baseline preservation and could destroy user-owned changes.

## D-002 - Dataset version pin

- Date: 2026-08-30
- Decision: pin KAIST to DOI `10.17632/ztmf3m7h5x.6` while retaining the
  dataset identifier `ztmf3m7h5x` in manifests.
- Reason: the source document named the dataset but did not pin a version. A
  fixed DOI is required for reproducibility.

## D-003 - EdgePack v1 header

- Date: 2026-08-30
- Decision: use a 64-byte little-endian header with explicit sample counts,
  chunk duration, payload bytes, and CRC32 in addition to the proposal fields.
- Reason: bounds checking and corrupted-packet rejection cannot be implemented
  safely when payload length and integrity are implicit.

## D-004 - KAIST source contradiction (open)

- Date: 2026-08-30
- Evidence: every Mendeley version 1-6 points to the same `acoustic.zip`
  (`47,205,730` bytes, SHA-256
  `5dd1371ccec00729498e2d151df13593f6400fdced8895481ff6a8a945a5962f`).
  The verified archive contains only `0Nm_Normal`, `0Nm_BPFI_03/10`, and
  `0Nm_BPFO_03/10`.
- Impact: this source cannot support synchronized five-class multimodal data,
  three-load multimodal LOLO, or leakage-safe held-out Normal recordings as
  written in the source specification.
- Temporary action: keep all validators strict, continue implementing the
  format/runtime/firmware, and do not generate synthetic accuracy evidence.
  Dataset substitution or a scope correction requires an explicit final
  decision before ML acceptance can pass.

## D-005 - Same-name modalities are not synchronized (open)

- Date: 2026-08-30
- Evidence: the verified MAT metadata records different absolute acquisition
  starts for same-name files. For example, `0Nm_BPFI_03` acoustic starts at
  `2021-07-05 16:03:30.869785`, while vibration starts at
  `2021-06-28 11:32:36.799212`. The publisher also states that acoustic was
  collected only at zero load to avoid brake noise.
- Impact: filename-level condition matching cannot satisfy the 20 ms
  synchronized audio/vibration requirement. Arbitrarily aligning these files
  would create false multimodal examples and invalid accuracy evidence.
- Action: manifest validation compares Test.Lab absolute-time metadata and
  rejects non-synchronized pairs. No real EdgePack benchmark is generated from
  these condition-only matches.

## D-006 - Isolated a locally modified SDK dependency

- Date: 2026-08-30
- Evidence: the previously extracted v1.4.0 SDK had an EdgePet-specific
  `edgepet_app_touch` call added to `libraries/Common/board/ports/lvgl/`
  `lv_port_indev.c`; the same file inside the pinned v1.4.0 ZIP has no such
  symbol. The first EdgeDoctor BSP link therefore failed with an unresolved
  EdgePet reference.
- Action: extracted a separate clean SDK dependency tree directly from the
  pinned ZIP and retargeted only the EdgeDoctor BSP junctions. The modified
  SDK tree and the user's EdgePet working project remain untouched.
