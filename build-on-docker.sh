#!/bin/bash
# Usage: ./build-on-docker.sh [linux/amd64|linux/arm64]   (default: linux/amd64)
cd "$(dirname "$0")"
set -e
PLATFORM=${1:-linux/amd64}
TAG=cws-builder-${PLATFORM//\//-}
docker build --platform "$PLATFORM" -f docker/Dockerfile -t "$TAG" docker
docker run --rm --platform "$PLATFORM" -v "$(pwd)":/cWS "$TAG" bash -c "cd /cWS && make"
