#!/usr/bin/env bash
set -euo pipefail
cd $HOME/training/ministral14b
# Based on the existing vLLM image already known-good on CUDA13.
docker build -t openclaw-ministral14b-train:latest -f Dockerfile .
