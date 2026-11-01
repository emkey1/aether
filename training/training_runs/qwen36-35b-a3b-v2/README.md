# qwen36-35b-a3b-v2: the MoE on the 1.0 data (started 2026-11-01)

Qwen3.6-35B-A3B fine-tuned with the released `qwen36-35b-a3b-e3`'s recipe on dataset `v2-20261101`. The dataset is
the only change. The config, with the reasoning, is `training/config/runs/qwen36-35b-a3b-v2.json`.

- **Dataset `v2-20261101`** (language 2026-10-30-1, 1.0): 2,278 instruction and 87 repair records, against v1's 1,981
  and 87.
  - Added: distilled batch `d5-20261031` (286 Aether pairs, 65 using maps) and the 11-pair cursor-sharing pack.
  - Python pairs stay capped at 220.
  - Every gate is clean, and the records are decontaminated against all 11 board manifests.
- **Recipe:** e3's. 3 epochs, no early stopping, the last step exported.
  - About 288 optimizer steps an epoch, so the epoch twins are the adapters of steps 300 and 575.
- **Grader:** the 1.0 release source (`aether-1.0-src.tar.gz`, checked against `SHA256SUMS`), built on the training
  host. Its `aether-current` is still 2026-10-21-8.
- **Bars** on the 1.0 grader, first attempt (after repair), main / held-out:
  - e3: 65 (70) / 39 (47);
  - the untrained MoE with the 1.0 guide: 60 (71) / 33 (43);
  - e3 at mxfp4 (20.9 GB, the 32 GB-Mac candidate): 64 (68) / 33 (40).

## Log

- 2026-11-01 06:22Z: training started on host-c, and scoring was armed to follow it (`score_when_ready.sh`).
  - To make room, the owner had the host's image and video generation services and its resident llama-server
    stopped. They are restarted by hand after the run.
  - The first push went to a different machine whose `/storage` is the same NFS share. It was removed before
    anything ran.
