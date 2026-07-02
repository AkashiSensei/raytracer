#!/usr/bin/env python3
import argparse
import json
import sys
from collections import Counter
from pathlib import Path


TEXTURE_KEYS = {
    "albedo",
    "metallic",
    "roughness",
    "texture",
    "albedo_texture",
    "source",
    "a",
    "b",
    "factor",
    "color",
    "color1",
    "color2",
    "value",
}


def scene_path(path):
    path = Path(path)
    if path.is_dir():
        direct = path / "scene.rt.json"
        if direct.exists():
            return direct
        candidates = sorted(
            (child for child in path.iterdir() if child.is_dir() and (child / "scene.rt.json").exists()),
            key=lambda child: child.stat().st_mtime,
            reverse=True,
        )
        if candidates:
            return candidates[0] / "scene.rt.json"
        path = direct
    return path


def walk(value, path="$"):
    yield path, value
    if isinstance(value, dict):
        for key, child in value.items():
            yield from walk(child, f"{path}.{key}")
    elif isinstance(value, list):
        for index, child in enumerate(value):
            yield from walk(child, f"{path}[{index}]")


def texture_nodes(value, path="$"):
    if isinstance(value, dict):
        node_type = value.get("type")
        if isinstance(node_type, str) and (
            node_type in {
                "checker",
                "checkerboard",
                "image",
                "texture",
                "color_ramp",
                "colorramp",
                "ramp",
                "math",
                "mix",
                "noise",
                "noise_texture",
                "invert",
                "map_range",
                "maprange",
                "solid",
                "color",
            }
        ):
            yield path, value
        for key, child in value.items():
            if key in TEXTURE_KEYS:
                yield from texture_nodes(child, f"{path}.{key}")
            elif isinstance(child, (dict, list)):
                yield from texture_nodes(child, f"{path}.{key}")
    elif isinstance(value, list):
        for index, child in enumerate(value):
            yield from texture_nodes(child, f"{path}[{index}]")


def main():
    parser = argparse.ArgumentParser(description="Inspect raytracer Blender texture export JSON.")
    parser.add_argument("path", help="scene.rt.json path or debug cache directory")
    parser.add_argument("--expect-checker", action="store_true", help="exit nonzero if no checker texture is found")
    parser.add_argument("--fail-on-unsupported", action="store_true", help="exit nonzero if unsupported texture nodes are reported")
    args = parser.parse_args()

    path = scene_path(args.path)
    with path.open("r", encoding="utf-8") as f:
        root = json.load(f)

    nodes = list(texture_nodes(root))
    type_counts = Counter(str(node.get("type", "")) for _, node in nodes)
    coord_counts = Counter(str(node.get("coord", "unspecified")) for _, node in nodes if isinstance(node, dict))
    transformed = [
        (p, n) for p, n in nodes
        if isinstance(n, dict) and any(k in n for k in ("uv_scale", "uv_offset", "uv_rotation"))
    ]
    unsupported = []
    mixed_coords = []
    for path_text, value in walk(root):
        if isinstance(value, dict):
            if value.get("unsupported_textures"):
                unsupported.append((path_text, value["unsupported_textures"]))
            if value.get("mixed_texture_coords"):
                mixed_coords.append((path_text, value["mixed_texture_coords"]))

    print(f"scene: {path}")
    print(f"texture_nodes: {len(nodes)}")
    print("types:")
    for key, count in sorted(type_counts.items()):
        print(f"  {key}: {count}")
    print("coords:")
    for key, count in sorted(coord_counts.items()):
        print(f"  {key}: {count}")
    print(f"transformed_textures: {len(transformed)}")
    for path_text, node in transformed[:8]:
        bits = []
        for key in ("type", "uv_scale", "uv_offset", "uv_rotation", "coord"):
            if key in node:
                bits.append(f"{key}={node[key]}")
        print(f"  {path_text}: {', '.join(bits)}")

    if unsupported:
        print("unsupported_textures:")
        for path_text, values in unsupported:
            print(f"  {path_text}: {values}")
    if mixed_coords:
        print("mixed_texture_coords:")
        for path_text, values in mixed_coords:
            print(f"  {path_text}: {values}")

    failed = False
    if args.expect_checker and not any(t in type_counts for t in ("checker", "checkerboard")):
        print("ERROR: expected at least one checker texture", file=sys.stderr)
        failed = True
    if args.fail_on_unsupported and unsupported:
        print("ERROR: unsupported texture nodes were exported", file=sys.stderr)
        failed = True
    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(main())
