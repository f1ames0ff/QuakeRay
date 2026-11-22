#!/usr/bin/env python3
"""Pack QuakeRay engine assets into id1/qray.pkz and optionally stage id1/ad data on Linux."""

from __future__ import annotations

import argparse
from pathlib import Path
import shutil
import struct
import sys
import zipfile


def unpack_quake_pak(pak_path: Path, dest_dir: Path) -> None:
    if not pak_path.is_file():
        return
    with pak_path.open("rb") as stream:
        header = stream.read(12)
        if len(header) != 12 or header[:4] != b"PACK":
            raise ValueError(f"{pak_path} is not a valid Quake PACK file")
        dir_ofs, dir_len = struct.unpack("<II", header[4:])
        file_len = pak_path.stat().st_size
        if dir_ofs + dir_len > file_len:
            raise ValueError(f"{pak_path} directory exceeds file length")
        count = dir_len // 64
        stream.seek(dir_ofs)
        entries = []
        seen: set[str] = set()
        for _ in range(count):
            rec = stream.read(64)
            if len(rec) < 64:
                break
            name = rec[:56].split(b"\x00", 1)[0].decode("ascii", errors="replace").replace("\\", "/")
            pos, length = struct.unpack("<II", rec[56:64])
            if not name or name.endswith("/"):
                continue
            if pos + length > file_len:
                raise ValueError(f"{pak_path} entry {name!r} exceeds file bounds")
            if name in seen:
                continue
            seen.add(name)
            entries.append((name, pos, length))

        for name, pos, length in entries:
            out_path = (dest_dir / name).resolve()
            if not out_path.is_relative_to(dest_dir.resolve()):
                raise ValueError(f"Unsafe PACK entry path: {name!r}")
            out_path.parent.mkdir(parents=True, exist_ok=True)
            stream.seek(pos)
            out_path.write_bytes(stream.read(length))


def pack_engine_assets(repo: Path, build_dir: Path, *, require_shaders: bool = False) -> bool:
    repo = repo.resolve()
    build_dir = build_dir.resolve()
    game_dir = build_dir / "id1"
    game_dir.mkdir(parents=True, exist_ok=True)

    for stale in ("materials", "textures", "progs", "mdl_skins", "shaders", "gfx"):
        stale_path = game_dir / stale
        if stale_path.is_dir() and not stale_path.is_symlink():
            shutil.rmtree(stale_path)
    for stale_root in (build_dir / "gfx", build_dir / "vkquake.pak"):
        if stale_root.is_dir() and not stale_root.is_symlink():
            shutil.rmtree(stale_root)
        elif stale_root.is_file() or stale_root.is_symlink():
            stale_root.unlink()

    materials_src = repo / "renderer/Source/materials.yaml"
    if materials_src.is_file():
        shutil.copy2(materials_src, game_dir / "qray.materials.yaml")

    shader_dir = repo / "renderer/Build"
    spv_files = sorted(shader_dir.glob("*.spv")) if shader_dir.is_dir() else []
    pkz_path = game_dir / "qray.pkz"
    if not spv_files:
        if require_shaders or not pkz_path.is_file():
            raise RuntimeError(
                f"No compiled .spv shaders found in {shader_dir}; set QR_DXC or place dxc on PATH to build shaders."
            )
        print(f"warning: no .spv shaders in {shader_dir}; keeping existing {pkz_path}")
        return False

    stage = build_dir / "qray_stage"
    if stage.exists():
        shutil.rmtree(stage)
    stage.mkdir(parents=True, exist_ok=True)

    try:
        stage_shaders = stage / "shaders"
        stage_shaders.mkdir(parents=True, exist_ok=True)
        for spv in spv_files:
            shutil.copy2(spv, stage_shaders / spv.name)

        asset_root = repo / "gfx"
        textures_src = asset_root / "textures"
        if textures_src.is_dir():
            shutil.copytree(textures_src, stage / "textures", dirs_exist_ok=True)
        for legacy in ("progs", "mdl_skins"):
            legacy_src = textures_src / legacy
            if legacy_src.is_dir():
                shutil.copytree(legacy_src, stage / legacy, dirs_exist_ok=True)

        for tex_name in ("BlueNoise_LDR_RGBA_128.png", "WaterNormal_n.png"):
            tex_src = textures_src / tex_name
            if tex_src.is_file():
                dst = stage / "textures" / tex_name
                dst.parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(tex_src, dst)

        stage_gfx = stage / "gfx"
        stage_gfx.mkdir(parents=True, exist_ok=True)
        if asset_root.is_dir():
            for item in asset_root.iterdir():
                if item.is_file():
                    shutil.copy2(item, stage_gfx / item.name)
        font_src = repo / "third_party/imgui/fonts/Roboto-Regular.ttf"
        if font_src.is_file():
            shutil.copy2(font_src, stage_gfx / "Roboto-Regular.ttf")

        unpack_quake_pak(repo / "Quake/vkquake.pak", stage)

        tmp_pkz = game_dir / "qray.pkz.tmp"
        if tmp_pkz.exists():
            tmp_pkz.unlink()
        with zipfile.ZipFile(tmp_pkz, "w", compression=zipfile.ZIP_DEFLATED, compresslevel=6) as zf:
            for file_path in sorted(stage.rglob("*")):
                if file_path.is_file():
                    zf.write(file_path, file_path.relative_to(stage).as_posix())
        tmp_pkz.replace(pkz_path)
        print(f"Packed {len(spv_files)} shader(s) and engine assets into {pkz_path}")
        return True
    finally:
        if stage.exists():
            shutil.rmtree(stage, ignore_errors=True)


def stage_id1_paks(build_dir: Path, id1_dir: Path) -> None:
    perf_dir = Path(__file__).resolve().parents[1] / "tests/perf"
    if str(perf_dir) not in sys.path:
        sys.path.insert(0, str(perf_dir))
    from machine_guard import _find_case_insensitive_file, stage_file_hardlink_or_copy  # noqa: PLC0415

    target_id1 = build_dir / "id1"
    target_id1.mkdir(parents=True, exist_ok=True)
    for pak_name in ("pak0.pak", "pak1.pak"):
        src = _find_case_insensitive_file(id1_dir, pak_name)
        if src is not None:
            action = stage_file_hardlink_or_copy(src, target_id1 / pak_name)
            print(f"id1/{pak_name}: {action}")


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="Pack Linux engine assets and optionally stage id1/ad data.")
    parser.add_argument("--repo", required=True, type=Path)
    parser.add_argument("--build-dir", required=True, type=Path)
    parser.add_argument("--require-shaders", action="store_true")
    parser.add_argument("--id1-dir", type=Path, default=None)
    parser.add_argument("--ad-dir", type=Path, default=None)
    parser.add_argument("--ad-saves-dir", type=Path, default=None)
    parser.add_argument("--ad-materials-dir", type=Path, default=None)
    args = parser.parse_args(argv)

    pack_engine_assets(args.repo, args.build_dir, require_shaders=args.require_shaders)
    if args.id1_dir and args.id1_dir.is_dir():
        stage_id1_paks(args.build_dir, args.id1_dir)
    if args.ad_dir or args.ad_saves_dir or args.ad_materials_dir:
        perf_dir = args.repo / "tests/perf"
        if str(perf_dir) not in sys.path:
            sys.path.insert(0, str(perf_dir))
        from machine_guard import stage_ad_assets  # noqa: PLC0415

        actions = stage_ad_assets(
            args.build_dir,
            ad_dir=args.ad_dir,
            saves_dir=args.ad_saves_dir,
            materials_dir=args.ad_materials_dir,
        )
        print(f"ad assets: {actions}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
