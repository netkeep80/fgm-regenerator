# FGM Regenerator

> Offline compiler for procedural 3D reconstruction from voxel geometry, sprites and source metadata.

**Status:** architecture/bootstrap only; no working product has been delivered.

## Responsibility

Import/normalize source data; preserve original collision constraints; reconstruct candidate geometry with deterministic algorithms and optional replaceable ML modules; emit provenance-aware FGM programs.

## Boundaries

No source converter or ML reconstructor is implemented yet; no copyrighted game data or derivative packs may be bundled.

The game-independent program format and normative semantics belong to [fgm-core](https://github.com/netkeep80/fgm-core), not to the regenerator, GPU renderer, or game rules. A cubic spatial hierarchy is an address system, **not** a requirement to display Minecraft-like cubes. Images should stay crisp at high resolutions without excessive visual detail or global blurring.

## Project family

- [fgm-core](https://github.com/netkeep80/fgm-core)
- [fgm-renderer](https://github.com/netkeep80/fgm-renderer)
- [ufo-fractal](https://github.com/netkeep80/ufo-fractal)

Existing research projects [IsoCubic](https://github.com/netkeep80/isocubic), [FractalGalaxy](https://github.com/netkeep80/FractalGalaxy) and [NNets](https://github.com/netkeep80/NNets) are related explorations, not automatically approved dependencies.

## Platform direction

C++20 for core algorithms; native and browser/WebAssembly targets; WebGPU/WGSL for optional GPU acceleration; TypeScript for the browser shell; offline neural-network experiments are optional. The canonical CPU semantics are independent of GPU floating-point implementation details.

## RepoGuard v6.0.1

[The PR workflow](.github/workflows/repo-guard.yml) runs the immutable [RepoGuard](https://github.com/netkeep80/repo-guard) release commit `74db97791e6da8a75287164f0d0b41579108f114` on **exact pull-request BASE and HEAD**. Canonical product/project requirements and relationships live in [requirements/](requirements/); [hierarchy](docs/requirements-hierarchy.md).

The check is about causal traceability, not an assertion that this unimplemented product has passed functional tests. Configure GitHub's **required status check** separately; a workflow without branch protection is not a mandatory merge gate. Technical GREEN is not author acceptance.

## First experiment

Convert an independently authored voxel/sprite fixture into FGM and validate original-view correspondence.

## License and source data

This repository uses Unlicense; see [LICENSE](LICENSE). The license covers original project code only and does not relicense third-party material. OpenXcom source code is GPLv3-or-later, and the original MicroProse/2K UFO and TFTD assets are not freely redistributable by virtue of OpenXcom's open-source license. Do not commit original sprites, music, maps, local conversions of proprietary game assets or derived learned-model datasets. Public tests must use original or license-compatible data.
