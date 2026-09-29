# Repository migration: `QuakeRay` -> `QuakeRay_new`

## What happened

- The project moved to **https://github.com/sdas234f23f/QuakeRay_new**, a fork of **Novum/vkQuake** (the engine's own
  upstream line).
- All refs were migrated on 2026-09-29: **master** = `ac01b482`; branches
  `feature/detach-p0-p1` (`885de321`), `feature/legacy-renderer-removal`, `bugfix/res-apply`, `bugfix/hotfixes`,
  `feature/volumetric-clouds`, `ffx-denoiser-pilot`, `tal-anim-fix` (plus vkQuake's own `cmake`, `q2rtx-renderer`,
  `sdas234f23f-q2rtx-renderer-port`); tags `2.1.0`, `hlsl-port`, `legacy-renderer-final`, `shader-smoke`, `v0.9.1`,
  `v0.10.0`, `v0.12.3`, `v3.6.0`.
- The old **https://github.com/sdas234f23f/QuakeRay** is the legacy archive: do not push there any more.

## If your worktree shares this repository's git directory

Remotes live in the shared git configuration, so every worktree of this repository sees the same `git remote -v`.

- If `origin` still points at the old repository, either push explicitly to the new one:

      git remote add newfork https://github.com/sdas234f23f/QuakeRay_new.git   # once, if missing
      git push -u newfork <your-branch>

- Or make the new repository the default (this changes it for every worktree of this repository):

      git remote set-url origin https://github.com/sdas234f23f/QuakeRay_new.git
      git fetch origin --prune

## If you have your own clone

    git remote add newfork https://github.com/sdas234f23f/QuakeRay_new.git
    git fetch newfork
    git push -u newfork <your-branch>
    # if the branch already exists there and you rewrote it:
    git push --force-with-lease newfork <your-branch>

## Rules

- **Never force-push `master`** of the new repository - it is the mainline (`ac01b482`).
- Push only the branch you own; `--force-with-lease`, never a bare `--force`, when the branch exists remotely.
- **Commit identity**: GitHub's privacy protection may reject a push that exposes a private email address. Commit as
  `f1ames0ff <f1am3sdev.github@protonmail.com>` - the address the owner uses for the project.
- Update repository links in the documents you touch (clone URLs in `readme.md` and the like); historical links in
  `changelog.md` may stay as they are.
- Verify the new remote with `git ls-remote https://github.com/sdas234f23f/QuakeRay_new.git` - it must show the
  migrated branches and tags.
