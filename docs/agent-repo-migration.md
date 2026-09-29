# Repository migration: to `sdas234f23f/QuakeRay`

## What happened

- The project's canonical repository is now **https://github.com/sdas234f23f/QuakeRay**, a fork of
  **Novum/vkQuake** (the engine's own upstream line). It was created as `QuakeRay_new` and renamed to
  `QuakeRay`; links to the old name redirect.
- All refs were migrated on 2026-09-29: **master** = `ac01b482`; branches
  `feature/detach-p0-p1` (`885de321`, then `7a7903c7`, then `2444e6e2`), `feature/legacy-renderer-removal`,
  `bugfix/res-apply`, `bugfix/hotfixes`, `feature/volumetric-clouds`, `ffx-denoiser-pilot`, `tal-anim-fix` (plus
  vkQuake's own `cmake`, `q2rtx-renderer`, `sdas234f23f-q2rtx-renderer-port`); tags `2.1.0`, `hlsl-port`,
  `legacy-renderer-final`, `shader-smoke`, `v0.9.1`, `v0.10.0`, `v0.12.3`, `v3.6.0`.
- The repository the project lived in before the move (the older fork lineage) is retired as an archive: never
  push to a remote that points at it.

## If your worktree shares this repository's git directory

Remotes live in the shared git configuration, so every worktree of this repository sees the same `git remote -v`.

- Push to the canonical repository explicitly:

      git remote add newfork https://github.com/sdas234f23f/QuakeRay.git   # once, if missing
      git push -u newfork <your-branch>

- Or make it the default (this changes it for every worktree of this repository):

      git remote set-url origin https://github.com/sdas234f23f/QuakeRay.git
      git fetch origin --prune

## If you have your own clone

    git remote add newfork https://github.com/sdas234f23f/QuakeRay.git
    git fetch newfork
    git push -u newfork <your-branch>
    # if the branch already exists there and you rewrote it:
    git push --force-with-lease newfork <your-branch>

## Rules

- Use the **HTTPS** URL (or the SSH one, `git@github.com:sdas234f23f/QuakeRay.git`, only where the machine has a
  GitHub key configured).
- **Never force-push `master`** of the canonical repository - it is the mainline (`ac01b482`).
- Push only the branch you own; `--force-with-lease`, never a bare `--force`, when the branch exists remotely.
- **Commit identity**: GitHub's privacy protection may reject a push that exposes a private email address. Commit as
  `f1ames0ff <f1am3sdev.github@protonmail.com>` - the address the owner uses for the project.
- Update repository links in the documents you touch (clone URLs in `readme.md` and the like); historical links in
  `changelog.md` may stay as they are.
- Verify the remote with `git ls-remote https://github.com/sdas234f23f/QuakeRay.git` - it must show the migrated
  branches and tags.
