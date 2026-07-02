bl_info = {
    "name": "Raytracer Renderer",
    "author": "Raytracer Course Project",
    "version": (0, 1, 0),
    "blender": (5, 1, 0),
    "location": "Render Properties > Render Engine",
    "description": "Render the current Blender scene with the course raytracer",
    "category": "Render",
}

import array
import json
import math
import os
import struct
import subprocess
import tempfile
import time
import urllib.error
import urllib.parse
import urllib.request
from pathlib import Path

import bpy
from bpy.props import (
    BoolProperty,
    EnumProperty,
    FloatProperty,
    FloatVectorProperty,
    IntProperty,
    PointerProperty,
    StringProperty,
)
from mathutils import Vector


ENGINE_ID = "RAYTRACER"
PROTOCOL_VERSION = 1
GEOMETRY_TYPES = {"MESH", "CURVE", "SURFACE", "FONT", "META"}


def rt_point(v):
    return [float(v.x), float(v.z), float(-v.y)]


def rt_vector(v):
    return [float(v.x), float(v.z), float(-v.y)]


def socket_by_name(node, names):
    for name in names:
        socket = node.inputs.get(name)
        if socket is not None:
            return socket
    for socket in node.inputs:
        if socket.name in names or getattr(socket, "identifier", "") in names:
            return socket
    return None


def socket_default_color(socket, default):
    if socket is None:
        return [float(default[0]), float(default[1]), float(default[2])]
    value = getattr(socket, "default_value", default)
    try:
        return [float(value[0]), float(value[1]), float(value[2])]
    except (TypeError, IndexError):
        return [float(default[0]), float(default[1]), float(default[2])]


def socket_float(node, names, default):
    socket = socket_by_name(node, names)
    if socket is None or socket.is_linked:
        return float(default)
    value = socket.default_value
    try:
        return float(value)
    except TypeError:
        return float(default)


def socket_color(node, names, default):
    socket = socket_by_name(node, names)
    if socket is None or socket.is_linked:
        return [float(default[0]), float(default[1]), float(default[2])]
    return socket_default_color(socket, default)


def principled_base_color(principled, default):
    socket = socket_by_name(principled, ("Base Color",))
    if socket is None:
        return [float(default[0]), float(default[1]), float(default[2])]

    value = socket_default_color(socket, default)
    if socket.is_linked and max(value) <= 1e-6 and max(default) <= 1e-6:
        return [0.8, 0.8, 0.8]
    if socket.is_linked and max(value) <= 1e-6:
        return [float(default[0]), float(default[1]), float(default[2])]
    return value


def socket_default_vector(socket, default):
    if socket is None:
        return [float(default[0]), float(default[1]), float(default[2])]
    value = getattr(socket, "default_value", default)
    try:
        return [float(value[0]), float(value[1]), float(value[2])]
    except (TypeError, IndexError):
        return [float(default[0]), float(default[1]), float(default[2])]


def clamp_float(value, lo, hi):
    return float(max(lo, min(hi, value)))


def first_linked_node(socket):
    if socket is None or not socket.is_linked or not socket.links:
        return None
    return socket.links[0].from_node


def first_linked_socket(socket):
    if socket is None or not socket.is_linked or not socket.links:
        return None
    return socket.links[0].from_socket


def linked_source(socket):
    if socket is None or not socket.is_linked or not socket.links:
        return (None, None)

    link = socket.links[0]
    node = link.from_node
    source_socket = link.from_socket
    guard = 0
    while node is not None and node.type == "REROUTE" and guard < 32:
        guard += 1
        input_socket = input_by_name(node, ("Input",))
        if input_socket is None or not input_socket.is_linked or not input_socket.links:
            return (None, None)
        link = input_socket.links[0]
        node = link.from_node
        source_socket = link.from_socket
    return (node, source_socket)


def input_by_name(node, names):
    return socket_by_name(node, names)


def unwrap_reroute(node):
    while node is not None and node.type == "REROUTE":
        node = first_linked_node(input_by_name(node, ("Input",)))
    return node


def mapping_transform_from_socket(vector_socket):
    transform = {
        "uv_scale": [1.0, 1.0],
        "uv_offset": [0.0, 0.0],
        "uv_rotation": 0.0,
    }
    node = unwrap_reroute(first_linked_node(vector_socket))
    if node is None or node.type != "MAPPING":
        return transform

    location = socket_default_vector(input_by_name(node, ("Location", "位置")), (0.0, 0.0, 0.0))
    rotation = socket_default_vector(input_by_name(node, ("Rotation", "旋转")), (0.0, 0.0, 0.0))
    scale = socket_default_vector(input_by_name(node, ("Scale", "缩放")), (1.0, 1.0, 1.0))
    transform["uv_scale"] = [float(scale[0]), float(scale[1])]
    transform["uv_offset"] = [float(location[0]), float(location[1])]
    transform["uv_rotation"] = float(math.degrees(rotation[2]))
    return transform


def texture_coord_source_from_socket(vector_socket, default="uv"):
    node, linked_socket = linked_source(vector_socket)
    if node is None:
        return default
    socket_name = str(getattr(linked_socket, "name", ""))
    socket_identifier = str(getattr(linked_socket, "identifier", ""))
    if node.type == "TEX_COORD":
        name = (socket_name or socket_identifier).lower()
        if "generated" in name or "object" in name or "生成" in name or "物体" in name:
            return "generated"
        if "uv" in name:
            return "uv"
    if node.type == "MAPPING":
        return texture_coord_source_from_socket(input_by_name(node, ("Vector", "矢量")), default)
    return default


def socket_default_scalar(socket, default):
    if socket is None:
        return float(default)
    try:
        return float(getattr(socket, "default_value", default))
    except TypeError:
        return float(default)


def note_unsupported_texture(unsupported, node):
    if unsupported is None or node is None:
        return
    name = str(getattr(node, "type", "UNKNOWN"))
    if name not in unsupported:
        unsupported.append(name)


def scalar_texture_from_input(socket, default, unsupported=None):
    texture = texture_from_socket(socket, None, unsupported)
    if texture is not None:
        return texture
    value = socket_default_scalar(socket, default)
    return {"type": "solid", "color": [value, value, value]}


def color_texture_from_input(socket, default, unsupported=None):
    texture = texture_from_socket(socket, None, unsupported)
    if texture is not None:
        return texture
    return {"type": "solid", "color": socket_default_color(socket, default)}


def texture_from_socket(socket, material, unsupported=None):
    node, linked_socket = linked_source(socket)
    if node is None:
        return None
    linked_socket_name = getattr(linked_socket, "name", "") if linked_socket is not None else ""

    if node.type == "VALUE":
        value = socket_default_scalar(linked_socket, 0.0)
        return {"type": "solid", "color": [value, value, value]}

    if node.type == "VALTORGB":
        fac_socket = input_by_name(node, ("Fac", "Factor", "系数"))
        source = texture_from_socket(fac_socket, material, unsupported)
        if source is None:
            fac_value = socket_float(node, ("Fac", "Factor", "系数"), 0.0)
            source = {
                "type": "solid",
                "color": [float(fac_value), float(fac_value), float(fac_value)],
            }

        stops = []
        color_ramp = getattr(node, "color_ramp", None)
        if color_ramp is not None:
            for element in color_ramp.elements:
                color = element.color
                stops.append({
                    "position": float(element.position),
                    "color": [float(color[0]), float(color[1]), float(color[2])],
                })
        return {
            "type": "color_ramp",
            "source": source,
            "stops": stops,
            "interpolation": str(getattr(color_ramp, "interpolation", "LINEAR")).lower() if color_ramp is not None else "linear",
        }

    if node.type == "MATH":
        input_a = node.inputs[0] if len(node.inputs) > 0 else None
        input_b = node.inputs[1] if len(node.inputs) > 1 else None
        return {
            "type": "math",
            "operation": str(getattr(node, "operation", "ADD")).lower(),
            "a": scalar_texture_from_input(input_a, 0.0, unsupported),
            "b": scalar_texture_from_input(input_b, 0.0, unsupported),
            "clamp": bool(getattr(node, "use_clamp", False)),
        }

    if node.type in {"MIX_RGB", "MIX"}:
        blend_type = str(getattr(node, "blend_type", "MIX")).upper()
        if node.type == "MIX_RGB" and blend_type != "MIX":
            note_unsupported_texture(unsupported, node)
            return None
        data_type = str(getattr(node, "data_type", "RGBA")).upper()
        fac_socket = input_by_name(node, ("Fac", "Factor", "系数"))
        color1_socket = input_by_name(node, ("Color1", "A", "颜色1"))
        color2_socket = input_by_name(node, ("Color2", "B", "颜色2"))
        if node.type == "MIX" and data_type in {"FLOAT", "VALUE"}:
            color1 = scalar_texture_from_input(color1_socket, 0.0, unsupported)
            color2 = scalar_texture_from_input(color2_socket, 1.0, unsupported)
        else:
            color1 = color_texture_from_input(color1_socket, (0.0, 0.0, 0.0), unsupported)
            color2 = color_texture_from_input(color2_socket, (1.0, 1.0, 1.0), unsupported)
        return {
            "type": "mix",
            "factor": scalar_texture_from_input(fac_socket, 0.5, unsupported),
            "color1": color1,
            "color2": color2,
        }

    if node.type == "INVERT":
        fac_socket = input_by_name(node, ("Fac", "Factor", "系数"))
        color_socket = input_by_name(node, ("Color", "颜色"))
        return {
            "type": "invert",
            "factor": scalar_texture_from_input(fac_socket, 1.0, unsupported),
            "color": color_texture_from_input(color_socket, (1.0, 1.0, 1.0), unsupported),
        }

    if node.type == "MAP_RANGE":
        interpolation = str(getattr(node, "interpolation_type", "LINEAR")).upper()
        if interpolation not in {"LINEAR", ""}:
            note_unsupported_texture(unsupported, node)
        value_socket = input_by_name(node, ("Value", "值"))
        from_min_socket = input_by_name(node, ("From Min", "源最小值"))
        from_max_socket = input_by_name(node, ("From Max", "源最大值"))
        to_min_socket = input_by_name(node, ("To Min", "目标最小值"))
        to_max_socket = input_by_name(node, ("To Max", "目标最大值"))
        return {
            "type": "map_range",
            "value": scalar_texture_from_input(value_socket, 0.0, unsupported),
            "from_min": socket_default_scalar(from_min_socket, 0.0),
            "from_max": socket_default_scalar(from_max_socket, 1.0),
            "to_min": socket_default_scalar(to_min_socket, 0.0),
            "to_max": socket_default_scalar(to_max_socket, 1.0),
            "clamp": bool(getattr(node, "clamp", getattr(node, "use_clamp", True))),
        }

    if node.type == "TEX_CHECKER":
        vector_socket = input_by_name(node, ("Vector", "矢量"))
        transform = mapping_transform_from_socket(vector_socket)
        coord = texture_coord_source_from_socket(vector_socket, "generated")
        if linked_socket_name in ("Fac", "Factor", "系数"):
            color1 = [0.0, 0.0, 0.0]
            color2 = [1.0, 1.0, 1.0]
        else:
            color1 = socket_color(node, ("Color1", "Color 1", "颜色1"), (0.8, 0.8, 0.8))
            color2 = socket_color(node, ("Color2", "Color 2", "颜色2"), (0.2, 0.2, 0.2))
        scale = socket_float(node, ("Scale", "缩放"), 5.0)
        return {
            "type": "checker",
            "color1": color1,
            "color2": color2,
            "scale": float(scale),
            "coord": coord,
            **transform,
        }

    if node.type == "TEX_NOISE":
        vector_socket = input_by_name(node, ("Vector", "矢量"))
        return {
            "type": "noise",
            "scale": socket_float(node, ("Scale", "缩放"), 5.0),
            "detail": socket_float(node, ("Detail", "细节"), 2.0),
            "roughness": socket_float(node, ("Roughness", "粗糙度"), 0.5),
            "distortion": socket_float(node, ("Distortion", "扭曲"), 0.0),
            "coord": texture_coord_source_from_socket(vector_socket, "generated"),
            **mapping_transform_from_socket(vector_socket),
        }

    if node.type == "TEX_IMAGE":
        image = getattr(node, "image", None)
        if image is None:
            return None
        path = bpy.path.abspath(image.filepath, library=getattr(image, "library", None))
        if not path:
            return None
        vector_socket = input_by_name(node, ("Vector", "矢量"))
        return {
            "type": "image",
            "path": path,
            "interpolation": str(getattr(node, "interpolation", "Linear")).lower(),
            "extension": str(getattr(node, "extension", "REPEAT")).lower(),
            "color_space": str(getattr(getattr(image, "colorspace_settings", None), "name", "")),
            "coord": texture_coord_source_from_socket(vector_socket, "uv"),
            **mapping_transform_from_socket(vector_socket),
        }

    if node.type == "RGB":
        color_socket = node.outputs.get("Color") if hasattr(node.outputs, "get") else None
        return {
            "type": "solid",
            "color": socket_default_color(color_socket, (0.8, 0.8, 0.8)),
        }

    note_unsupported_texture(unsupported, node)
    return None


def transformed_normal(matrix, normal):
    try:
        normal_matrix = matrix.to_3x3().inverted().transposed()
    except ValueError:
        normal_matrix = matrix.to_3x3()
    world_normal = normal_matrix @ normal
    if world_normal.length_squared <= 1e-12:
        world_normal = normal
    return rt_vector(world_normal.normalized())


def material_to_rt(material):
    if material is None:
        return {"type": "pbr", "albedo": [0.8, 0.8, 0.8], "metallic": 0.0, "roughness": 0.55}

    base = [float(material.diffuse_color[0]), float(material.diffuse_color[1]), float(material.diffuse_color[2])]
    alpha = float(material.diffuse_color[3]) if len(material.diffuse_color) > 3 else 1.0
    metallic = 0.0
    roughness = 0.55
    ior = 1.5
    transmission = 0.0
    emission = [0.0, 0.0, 0.0]
    emission_strength = 0.0
    unsupported_textures = []

    if material.use_nodes and material.node_tree is not None:
        principled = None
        for node in material.node_tree.nodes:
            if node.type == "BSDF_PRINCIPLED":
                principled = node
                break
        if principled is not None:
            base_socket = socket_by_name(principled, ("Base Color",))
            base_texture = texture_from_socket(base_socket, material, unsupported_textures)
            base = base_texture if base_texture is not None else principled_base_color(principled, base)
            alpha = socket_float(principled, ("Alpha",), alpha)
            metallic_socket = socket_by_name(principled, ("Metallic",))
            metallic_texture = texture_from_socket(metallic_socket, material, unsupported_textures)
            metallic = metallic_texture if metallic_texture is not None else socket_float(principled, ("Metallic",), metallic)
            roughness_socket = socket_by_name(principled, ("Roughness",))
            roughness_texture = texture_from_socket(roughness_socket, material, unsupported_textures)
            roughness = roughness_texture if roughness_texture is not None else socket_float(principled, ("Roughness",), roughness)
            ior = socket_float(principled, ("IOR", "折射率"), ior)
            transmission = socket_float(
                principled,
                ("Transmission Weight", "Transmission", "Transmission Weight", "透射", "透射权重"),
                transmission,
            )
            emission = socket_color(principled, ("Emission Color", "Emission"), emission)
            emission_strength = socket_float(principled, ("Emission Strength",), emission_strength)

    if emission_strength > 0.0:
        result = {
            "type": "emissive",
            "emission": [
                float(emission[0] * emission_strength),
                float(emission[1] * emission_strength),
                float(emission[2] * emission_strength),
            ],
        }
        if unsupported_textures:
            result["unsupported_textures"] = unsupported_textures
        return result

    if alpha < 0.35 or transmission > 0.5:
        result = {
            "type": "dielectric",
            "ior": max(1.0, float(ior)),
            "albedo": base if isinstance(base, dict) else [
                clamp_float(base[0], 0.0, 1.0),
                clamp_float(base[1], 0.0, 1.0),
                clamp_float(base[2], 0.0, 1.0),
            ],
        }
        if unsupported_textures:
            result["unsupported_textures"] = unsupported_textures
        return result

    result = {
        "type": "pbr",
        "albedo": base,
        "metallic": metallic if isinstance(metallic, dict) else clamp_float(metallic, 0.0, 1.0),
        "roughness": roughness if isinstance(roughness, dict) else clamp_float(roughness, 0.001, 1.0),
    }
    if unsupported_textures:
        result["unsupported_textures"] = unsupported_textures
    return result


def export_camera(scene, depsgraph, width, height):
    camera = scene.camera
    if camera is None:
        raise RuntimeError("Scene has no active camera")

    camera_eval = camera.evaluated_get(depsgraph)
    matrix = camera_eval.matrix_world
    location = matrix.to_translation()
    forward = matrix.to_quaternion() @ Vector((0.0, 0.0, -1.0))
    up = matrix.to_quaternion() @ Vector((0.0, 1.0, 0.0))

    lookfrom = Vector(rt_point(location))
    lookat = lookfrom + Vector(rt_vector(forward))
    vup = Vector(rt_vector(up))
    vfov = math.degrees(camera_eval.data.angle_y)
    focus_dist = camera_eval.data.dof.focus_distance
    if focus_dist <= 0.0:
        focus_dist = 1.0

    frame = camera_eval.data.view_frame(scene=scene)
    world_frame = [matrix @ corner for corner in frame]
    frame_center = sum(world_frame, Vector((0.0, 0.0, 0.0))) / 4.0
    plane_dist = abs((frame_center - location).dot(forward.normalized()))
    frame_scale = focus_dist / plane_dist if plane_dist > 1e-8 else 1.0

    lower_left = location + (world_frame[2] - location) * frame_scale
    lower_right = location + (world_frame[1] - location) * frame_scale
    upper_left = location + (world_frame[3] - location) * frame_scale
    horizontal = lower_right - lower_left
    vertical = upper_left - lower_left

    return {
        "lookfrom": [float(lookfrom.x), float(lookfrom.y), float(lookfrom.z)],
        "lookat": [float(lookat.x), float(lookat.y), float(lookat.z)],
        "vup": [float(vup.x), float(vup.y), float(vup.z)],
        "vfov": float(vfov),
        "frame": {
            "lower_left": rt_point(lower_left),
            "horizontal": rt_vector(horizontal),
            "vertical": rt_vector(vertical),
        },
        "aperture": float(camera_eval.data.dof.aperture_fstop if camera_eval.data.dof.use_dof else 0.0),
        "focus_dist": float(focus_dist),
    }


def export_lights(depsgraph, settings):
    lights = []
    seen = set()
    intensity_scale = max(0.0, float(settings.light_intensity_scale))
    for instance in depsgraph.object_instances:
        obj = instance.object
        if obj.type != "LIGHT":
            continue
        original = getattr(obj, "original", obj)
        if getattr(original, "hide_render", False) or getattr(obj, "hide_render", False):
            continue
        key = (obj.original.as_pointer(), tuple(round(x, 8) for row in instance.matrix_world for x in row))
        if key in seen:
            continue
        seen.add(key)

        data = obj.data
        matrix = instance.matrix_world
        color = [float(data.color[0]), float(data.color[1]), float(data.color[2])]
        energy = float(getattr(data, "energy", 1.0))
        intensity = max(0.0, energy * intensity_scale)
        if data.type == "POINT":
            lights.append({
                "type": "point",
                "position": rt_point(matrix.to_translation()),
                "color": color,
                "intensity": intensity,
            })
        elif data.type == "SUN":
            direction = matrix.to_quaternion() @ Vector((0.0, 0.0, -1.0))
            lights.append({
                "type": "directional",
                "direction": rt_vector(direction),
                "color": color,
                "intensity": intensity,
            })
        elif data.type == "SPOT":
            lights.append({
                "type": "point",
                "position": rt_point(matrix.to_translation()),
                "color": color,
                "intensity": intensity,
            })
        elif data.type == "AREA":
            rotation = matrix.to_quaternion()
            shape = getattr(data, "shape", "SQUARE")
            size_x = max(1e-6, float(getattr(data, "size", 1.0)))
            size_y = max(1e-6, float(getattr(data, "size_y", size_x)))
            direction = rotation @ Vector((0.0, 0.0, -1.0))
            if shape in {"DISK", "ELLIPSE"}:
                radius = 0.5 * size_x
                if shape == "ELLIPSE":
                    radius = 0.5 * math.sqrt(size_x * size_y)
                lights.append({
                    "type": "disk",
                    "position": rt_point(matrix.to_translation()),
                    "direction": rt_vector(direction),
                    "radius": radius,
                    "color": color,
                    "intensity": intensity,
                    "visible_camera": bool(settings.area_light_camera_visible),
                    "visible_specular": True,
                })
            else:
                if shape == "SQUARE":
                    size_y = size_x
                u = rotation @ Vector((size_x, 0.0, 0.0))
                v = rotation @ Vector((0.0, size_y, 0.0))
                lights.append({
                    "type": "rect",
                    "position": rt_point(matrix.to_translation()),
                    "direction": rt_vector(direction),
                    "u": rt_vector(u),
                    "v": rt_vector(v),
                    "color": color,
                    "intensity": intensity,
                    "visible_camera": bool(settings.area_light_camera_visible),
                    "visible_specular": True,
                })
    return lights


def material_for_slot(obj, material_index):
    if 0 <= material_index < len(obj.material_slots):
        return obj.material_slots[material_index].material
    return None


def material_texture_coords(value):
    coords = set()
    if isinstance(value, dict):
        coord = value.get("coord")
        if coord in {"generated", "uv"}:
            coords.add(coord)
        for child in value.values():
            coords.update(material_texture_coords(child))
    if isinstance(value, list):
        for child in value:
            coords.update(material_texture_coords(child))
    return coords


def mesh_generated_bounds(mesh):
    if not mesh.vertices:
        return (Vector((0.0, 0.0, 0.0)), Vector((1.0, 1.0, 1.0)))
    mins = Vector((mesh.vertices[0].co.x, mesh.vertices[0].co.y, mesh.vertices[0].co.z))
    maxs = Vector((mins.x, mins.y, mins.z))
    for vertex in mesh.vertices[1:]:
        co = vertex.co
        mins.x = min(mins.x, co.x)
        mins.y = min(mins.y, co.y)
        mins.z = min(mins.z, co.z)
        maxs.x = max(maxs.x, co.x)
        maxs.y = max(maxs.y, co.y)
        maxs.z = max(maxs.z, co.z)
    return (mins, maxs)


def generated_uv(mesh, vertex_index, loop_index, bounds):
    mins, maxs = bounds
    co = mesh.vertices[vertex_index].co
    spans = Vector((
        max(maxs.x - mins.x, 1e-12),
        max(maxs.y - mins.y, 1e-12),
        max(maxs.z - mins.z, 1e-12),
    ))
    generated = (
        (co.x - mins.x) / spans.x,
        (co.y - mins.y) / spans.y,
        (co.z - mins.z) / spans.z,
    )
    normal = mesh.loops[loop_index].normal
    ax = abs(normal.x)
    ay = abs(normal.y)
    az = abs(normal.z)
    if ax >= ay and ax >= az:
        return [float(generated[1]), float(generated[2])]
    if ay >= ax and ay >= az:
        return [float(generated[0]), float(generated[2])]
    return [float(generated[0]), float(generated[1])]


def export_mesh_instance(instance, depsgraph):
    obj = instance.object
    if obj.type not in GEOMETRY_TYPES:
        return []
    original = getattr(obj, "original", obj)
    if getattr(original, "hide_render", False) or getattr(obj, "hide_render", False):
        return []

    mesh = obj.to_mesh(preserve_all_data_layers=True, depsgraph=depsgraph)
    if mesh is None:
        return []

    objects = []
    try:
        mesh.calc_loop_triangles()
        uv_layer = mesh.uv_layers.active.data if mesh.uv_layers.active else None
        generated_bounds = mesh_generated_bounds(mesh)
        groups = {}

        for tri in mesh.loop_triangles:
            material_index = int(tri.material_index)
            group = groups.get(material_index)
            if group is None:
                material_json = material_to_rt(material_for_slot(obj, material_index))
                texture_coords = material_texture_coords(material_json)
                if "generated" in texture_coords and "uv" in texture_coords:
                    material_json["mixed_texture_coords"] = sorted(texture_coords)
                group = {
                    "vertices": [],
                    "indices": [],
                    "normals": [],
                    "uvs": [],
                    "material": material_json,
                    "use_generated_uvs": texture_coords == {"generated"},
                }
                groups[material_index] = group

            for vertex_index, loop_index in zip(tri.vertices, tri.loops):
                world_co = instance.matrix_world @ mesh.vertices[vertex_index].co
                group["vertices"].append(rt_point(world_co))
                group["indices"].append(len(group["indices"]))
                group["normals"].append(transformed_normal(instance.matrix_world, mesh.loops[loop_index].normal))
                if uv_layer is not None and not group["use_generated_uvs"]:
                    uv = uv_layer[loop_index].uv
                    group["uvs"].append([float(uv.x), float(uv.y)])
                else:
                    group["uvs"].append(generated_uv(mesh, vertex_index, loop_index, generated_bounds))

        for material_index, group in groups.items():
            if not group["indices"]:
                continue
            item = {
                "type": "triangles",
                "vertices": group["vertices"],
                "indices": group["indices"],
                "material": group["material"],
            }
            if group["uvs"] is not None:
                item["uvs"] = group["uvs"]
            if group["normals"]:
                item["normals"] = group["normals"]
            objects.append(item)
    finally:
        obj.to_mesh_clear()

    return objects


def node_socket_color(node, names, default):
    socket = socket_by_name(node, names)
    if socket is None or socket.is_linked:
        return None
    return socket_default_color(socket, default)


def node_socket_float(node, names, default):
    socket = socket_by_name(node, names)
    if socket is None or socket.is_linked:
        return None
    try:
        return float(socket.default_value)
    except TypeError:
        return float(default)


def world_display_color(scene):
    if scene.world is None:
        return [0.0, 0.0, 0.0]
    return [
        float(scene.world.color[0]),
        float(scene.world.color[1]),
        float(scene.world.color[2]),
    ]


def find_world_background(world):
    if world is None or not world.use_nodes or world.node_tree is None:
        return (None, None, None)

    output_node = None
    for node in world.node_tree.nodes:
        if node.type == "OUTPUT_WORLD" and getattr(node, "is_active_output", True):
            output_node = node
            break
    if output_node is None:
        for node in world.node_tree.nodes:
            if node.type == "OUTPUT_WORLD":
                output_node = node
                break
    if output_node is None:
        return (None, None, None)

    surface = socket_by_name(output_node, ("Surface", "表(曲)面"))
    if surface is None or not surface.is_linked:
        return (output_node, surface, None)

    background = surface.links[0].from_node
    if background.type != "BACKGROUND":
        return (output_node, surface, None)
    return (output_node, surface, background)


def world_surface_color(scene):
    if scene.world is None:
        return [0.0, 0.0, 0.0]
    world = scene.world
    if not world.use_nodes or world.node_tree is None:
        return world_display_color(scene)

    _, _, background = find_world_background(world)
    if background is None:
        return world_display_color(scene)

    color = node_socket_color(background, ("Color", "颜色"), world_display_color(scene))
    strength = node_socket_float(background, ("Strength", "强度"), 1.0)
    if color is None:
        color = world_display_color(scene)
    if strength is None:
        strength = 1.0
    return [
        float(color[0]) * strength,
        float(color[1]) * strength,
        float(color[2]) * strength,
    ]


def export_background(scene, settings):
    if settings.background_mode == "SKY":
        return {"type": "sky"}
    if settings.background_mode == "WORLD":
        return {"type": "solid", "color": world_surface_color(scene)}
    if settings.background_mode == "COLOR":
        return {
            "type": "solid",
            "color": [
                float(settings.background_color[0]),
                float(settings.background_color[1]),
                float(settings.background_color[2]),
            ],
        }
    return {"type": "solid", "color": [0.0, 0.0, 0.0]}


def export_ambient(settings):
    if not settings.ambient_enabled:
        return [0.0, 0.0, 0.0]
    strength = float(settings.ambient_strength) * max(0.0, float(settings.light_intensity_scale))
    return [
        float(settings.ambient_color[0]) * strength,
        float(settings.ambient_color[1]) * strength,
        float(settings.ambient_color[2]) * strength,
    ]


def export_scene_package(scene, depsgraph, settings):
    scale = scene.render.resolution_percentage / 100.0
    width = max(1, int(scene.render.resolution_x * scale))
    height = max(1, int(scene.render.resolution_y * scale))

    objects = []
    for instance in depsgraph.object_instances:
        objects.extend(export_mesh_instance(instance, depsgraph))

    return {
        "protocol": PROTOCOL_VERSION,
        "source": "blender",
        "image": {
            "width": width,
            "height": height,
            "samples": int(settings.samples),
            "max_depth": int(settings.max_depth),
            "output": "blender.ppm",
        },
        "camera": export_camera(scene, depsgraph, width, height),
        "background": export_background(scene, settings),
        "lighting": {"ambient": export_ambient(settings)},
        "lights": export_lights(depsgraph, settings),
        "objects": objects,
    }


def create_debug_cache(settings):
    cache_root = settings.debug_cache_dir.strip()
    if not cache_root:
        return None
    root = Path(bpy.path.abspath(cache_root)).expanduser()
    root.mkdir(parents=True, exist_ok=True)
    stamp = time.strftime("%Y%m%d_%H%M%S")
    suffix = int((time.time() % 1.0) * 1000)
    for attempt in range(100):
        cache = root / f"raytracer_{stamp}_{suffix:03d}_{attempt:02d}"
        if not cache.exists():
            cache.mkdir(parents=True, exist_ok=False)
            return cache
    raise RuntimeError("Cannot create debug cache directory under: " + str(root))


def write_debug_json(cache, name, value):
    if cache is None:
        return
    with (cache / name).open("w", encoding="utf-8") as f:
        json.dump(value, f, indent=2)


def write_debug_text(cache, name, value):
    if cache is None:
        return
    with (cache / name).open("w", encoding="utf-8") as f:
        f.write(value)


def write_debug_bytes(cache, name, value):
    if cache is None:
        return
    with (cache / name).open("wb") as f:
        f.write(value)


class RenderPixels:
    def __init__(self, width, height, pixels):
        self.width = width
        self.height = height
        self.pixels = pixels

    def blender_rect(self):
        rect = []
        row_stride = self.width * 4
        for y in range(self.height - 1, -1, -1):
            row = y * row_stride
            for x in range(self.width):
                i = row + x * 4
                rect.append((self.pixels[i], self.pixels[i + 1], self.pixels[i + 2], self.pixels[i + 3]))
        return rect


class RendererClient:
    def render(self, package, engine, settings, debug_cache=None):
        raise NotImplementedError


def render_schedule_protocol(settings):
    return "sample_passes" if settings.render_schedule == "SAMPLE_PASSES" else "rows"


def progressive_update_interval(package, settings):
    image = package.get("image", {})
    if settings.render_schedule == "SAMPLE_PASSES":
        return 1 if settings.backend == "REMOTE" else 16
    height = int(image.get("height", 1))
    return max(1, height // 20)


class LocalSubprocessRenderer(RendererClient):
    def __init__(self, bridge_path):
        self.bridge_path = bridge_path

    def render_in_directory(self, package, engine, settings, work_dir, debug_cache=None):
        scene_path = Path(work_dir) / "scene.rt.json"
        result_path = Path(work_dir) / "result.rgba32f"
        if not scene_path.exists():
            with scene_path.open("w", encoding="utf-8") as f:
                json.dump(package, f, separators=(",", ":"))

        cmd = [
            self.bridge_path,
            "--scene", str(scene_path),
            "--out-float", str(result_path),
            "--threads", str(max(1, int(settings.threads))),
            "--render-schedule", render_schedule_protocol(settings),
        ]
        if settings.progressive_preview:
            partial_dir = Path(work_dir) / "partials"
            cmd.extend([
                "--partial-dir", str(partial_dir),
                "--partial-update-interval", str(progressive_update_interval(package, settings)),
            ])
        if settings.direct_only:
            cmd.append("--direct-only")
        write_debug_text(debug_cache, "command.txt", " ".join(cmd) + "\n")

        process = subprocess.Popen(
            cmd,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )
        stderr_lines = []
        try:
            while True:
                line = process.stderr.readline()
                if line:
                    stderr_lines.append(line.rstrip())
                    if line.startswith("PROGRESS "):
                        try:
                            engine.update_progress(float(line.split()[1]))
                        except (IndexError, ValueError):
                            pass
                    elif line.startswith("PARTIAL "):
                        try:
                            parts = line.split(" ", 2)
                            progress = float(parts[1])
                            pixels = read_rgba32f(parts[2].strip())
                            engine.update_render_pixels(pixels)
                            engine.update_progress(max(0.0, min(0.98, progress)))
                        except Exception as exc:
                            stderr_lines.append("PARTIAL preview failed: " + str(exc))
                    if engine.test_break():
                        process.terminate()
                        raise RuntimeError("Render cancelled")
                elif process.poll() is not None:
                    break

            stdout = process.stdout.read() if process.stdout else ""
            returncode = process.wait()
            write_debug_text(debug_cache, "bridge.stderr.log", "\n".join(stderr_lines) + ("\n" if stderr_lines else ""))
            write_debug_text(debug_cache, "bridge.stdout.log", stdout)
            if returncode != 0:
                details = "\n".join(stderr_lines[-12:])
                if stdout.strip():
                    details += "\n" + stdout.strip()
                raise RuntimeError("Raytracer bridge failed\n" + details)

            return read_rgba32f(result_path)
        finally:
            if process.poll() is None:
                process.terminate()

    def render(self, package, engine, settings, debug_cache=None):
        if not self.bridge_path:
            raise RuntimeError("Raytracer bridge path is empty")
        if not os.path.exists(self.bridge_path):
            raise RuntimeError("Raytracer bridge not found: " + self.bridge_path)

        if debug_cache is not None:
            return self.render_in_directory(package, engine, settings, debug_cache, debug_cache)

        with tempfile.TemporaryDirectory(prefix="raytracer_blender_") as tmp:
            return self.render_in_directory(package, engine, settings, Path(tmp), None)


class RemoteHttpRenderer(RendererClient):
    def __init__(self, endpoint):
        self.endpoint = endpoint

    def base_url(self):
        if not self.endpoint:
            raise RuntimeError("Remote renderer endpoint is empty")
        base = self.endpoint.rstrip("/")
        if base.endswith("/render"):
            base = base[:-len("/render")]
        return base

    def open_json(self, request, timeout):
        try:
            with urllib.request.urlopen(request, timeout=timeout) as response:
                return json.loads(response.read().decode("utf-8"))
        except urllib.error.HTTPError as exc:
            details = exc.read().decode("utf-8", errors="replace").strip()
            raise RuntimeError("Remote renderer failed: HTTP " + str(exc.code) + ("\n" + details if details else ""))
        except urllib.error.URLError as exc:
            raise RuntimeError("Remote renderer unavailable: " + str(exc.reason))

    def request_timeout(self, deadline):
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            raise RuntimeError("Remote renderer timed out")
        return max(1, min(10, int(remaining)))

    def url(self, path, params=None):
        result = self.base_url() + path
        if params:
            result += "?" + urllib.parse.urlencode(params)
        return result

    def cancel_job(self, job_id, deadline):
        quoted = urllib.parse.quote(job_id, safe="")
        request = urllib.request.Request(
            self.url("/jobs/" + quoted + "/cancel"),
            data=b"",
            method="POST",
        )
        try:
            self.open_json(request, self.request_timeout(deadline))
        except RuntimeError:
            pass

    def render(self, package, engine, settings, debug_cache=None):
        deadline = time.monotonic() + max(1, int(settings.remote_timeout))
        render_params = {
            "threads": max(1, int(settings.threads)),
            "direct_only": "1" if settings.direct_only else "0",
            "render_schedule": render_schedule_protocol(settings),
        }
        if settings.progressive_preview:
            render_params["partial_update_interval"] = progressive_update_interval(package, settings)
        params = urllib.parse.urlencode(render_params)
        write_debug_text(debug_cache, "remote_request.txt", self.url("/jobs") + "?" + params + "\n")

        body = json.dumps(package, separators=(",", ":")).encode("utf-8")
        create_request = urllib.request.Request(
            self.url("/jobs") + "?" + params,
            data=body,
            headers={
                "Content-Type": "application/json",
                "Accept": "application/json",
                "X-Raytracer-Protocol": str(PROTOCOL_VERSION),
            },
            method="POST",
        )

        if engine.test_break():
            raise RuntimeError("Render cancelled")

        create_response = self.open_json(create_request, self.request_timeout(deadline))
        write_debug_json(debug_cache, "remote_create_response.json", create_response)
        job_id = create_response.get("job_id")
        if not job_id:
            raise RuntimeError("Remote renderer did not return a job id")

        quoted_job_id = urllib.parse.quote(job_id, safe="")
        progress_url = self.url("/jobs/" + quoted_job_id + "/progress")
        partial_url = self.url("/jobs/" + quoted_job_id + "/partial")
        result_url = self.url("/jobs/" + quoted_job_id + "/result")
        engine.update_progress(0.0)
        progress_lines = []
        last_partial_seq = 0

        while True:
            if engine.test_break():
                self.cancel_job(job_id, deadline)
                raise RuntimeError("Render cancelled")

            progress_request = urllib.request.Request(progress_url, headers={"Accept": "application/json"})
            status = self.open_json(progress_request, self.request_timeout(deadline))
            state = status.get("status", "unknown")
            progress = float(status.get("progress", 0.0))
            engine.update_progress(max(0.0, min(0.98, progress)))
            progress_lines.append(json.dumps(status, separators=(",", ":")))
            write_debug_text(debug_cache, "remote_progress.log", "\n".join(progress_lines) + "\n")

            partial_seq = int(status.get("partial_seq", 0))
            if settings.progressive_preview and partial_seq > last_partial_seq:
                partial_request = urllib.request.Request(partial_url, headers={"Accept": "application/octet-stream"})
                try:
                    with urllib.request.urlopen(partial_request, timeout=self.request_timeout(deadline)) as response:
                        partial = response.read()
                    last_partial_seq = partial_seq
                    write_debug_bytes(debug_cache, "remote_partial_%06d.rgba32f" % partial_seq, partial)
                    engine.update_render_pixels(parse_rgba32f(partial))
                except urllib.error.HTTPError as exc:
                    if exc.code not in (202, 404):
                        details = exc.read().decode("utf-8", errors="replace").strip()
                        raise RuntimeError(
                            "Remote renderer failed: HTTP " + str(exc.code) + ("\n" + details if details else "")
                        )
                except urllib.error.URLError as exc:
                    raise RuntimeError("Remote renderer unavailable: " + str(exc.reason))

            if state == "done":
                break
            if state == "cancelled":
                raise RuntimeError("Remote render cancelled")
            if state == "error":
                raise RuntimeError("Remote renderer failed: " + str(status.get("error", "unknown error")))

            time.sleep(0.5)

        if engine.test_break():
            self.cancel_job(job_id, deadline)
            raise RuntimeError("Render cancelled")

        result_request = urllib.request.Request(result_url, headers={"Accept": "application/octet-stream"})
        try:
            with urllib.request.urlopen(result_request, timeout=self.request_timeout(deadline)) as response:
                result = response.read()
        except urllib.error.HTTPError as exc:
            details = exc.read().decode("utf-8", errors="replace").strip()
            raise RuntimeError("Remote renderer failed: HTTP " + str(exc.code) + ("\n" + details if details else ""))
        except urllib.error.URLError as exc:
            raise RuntimeError("Remote renderer unavailable: " + str(exc.reason))

        engine.update_progress(0.99)
        write_debug_bytes(debug_cache, "result.rgba32f", result)
        return parse_rgba32f(result)


def parse_rgba32f(data):
    if len(data) < 16 or data[:8] != b"RTRGBAF1":
        raise RuntimeError("Invalid raytracer result data")
    width, height = struct.unpack("<II", data[8:16])
    expected = 16 + width * height * 4 * 4
    if len(data) < expected:
        raise RuntimeError("Truncated raytracer result data")
    pixels = array.array("f")
    pixels.frombytes(data[16:expected])
    if len(pixels) != width * height * 4:
        raise RuntimeError("Invalid raytracer pixel data")
    return RenderPixels(width, height, pixels)


def read_rgba32f(path):
    with open(path, "rb") as f:
        return parse_rgba32f(f.read())


def default_bridge_path():
    addon_path = Path(__file__).resolve()
    fallback = addon_path.parent / "raytracer_blender_bridge"
    for root in addon_path.parents:
        candidate = root / "raytracer_blender_bridge"
        if candidate.exists():
            return str(candidate)
        if (root / "CMakeLists.txt").exists() and (root / "src").exists():
            fallback = candidate
    return str(fallback)


def make_client(settings):
    if settings.backend == "LOCAL":
        path = settings.bridge_path.strip() or default_bridge_path()
        return LocalSubprocessRenderer(path)
    return RemoteHttpRenderer(settings.remote_endpoint.strip())


class RaytracerSettings(bpy.types.PropertyGroup):
    backend: EnumProperty(
        name="后端",
        items=[
            ("LOCAL", "本地", "使用本机 raytracer_blender_bridge 可执行文件"),
            ("REMOTE", "远程", "通过 HTTP 连接 raytracer_server 渲染当前场景"),
        ],
        default="LOCAL",
    )
    bridge_path: StringProperty(
        name="桥接程序",
        subtype="FILE_PATH",
        default="",
        description="raytracer_blender_bridge 的路径；留空时使用仓库内默认路径",
    )
    remote_endpoint: StringProperty(
        name="远程地址",
        default="http://localhost:8080",
        description="raytracer_server 的 HTTP 地址，例如 http://server:8080",
    )
    remote_timeout: IntProperty(name="远程超时", default=600, min=1, max=86400)
    samples: IntProperty(name="采样数", default=16, min=1, max=4096)
    max_depth: IntProperty(name="最大深度", default=16, min=1, max=256)
    threads: IntProperty(name="线程数", default=8, min=1, max=128)
    direct_only: BoolProperty(name="仅直接光照", default=False)
    render_schedule: EnumProperty(
        name="采样调度",
        items=[
            ("ROWS", "按行完成", "完成一批像素行后刷新预览；最终像素在写入前已完成全部采样"),
            ("SAMPLE_PASSES", "全图累积", "每轮给整张图增加部分采样，预览会逐步降噪"),
        ],
        default="ROWS",
        description="控制渲染任务的采样顺序；全图累积更接近 Cycles 的渐进预览",
    )
    progressive_preview: BoolProperty(
        name="渐进预览",
        default=True,
        description="渲染期间逐步刷新 Render Result；本地和远程后端均支持",
    )
    light_intensity_scale: FloatProperty(
        name="灯光强度倍率",
        default=0.03,
        min=0.0,
        max=100.0,
        precision=4,
        description="Blender 灯光 energy 导出为 raytracer intensity 前应用的倍率",
    )
    area_light_camera_visible: BoolProperty(
        name="面光源相机可见",
        default=False,
        description="开启后 Blender Area Light 会被相机直接看见；默认仍会在玻璃和金属反射中可见",
    )
    background_mode: EnumProperty(
        name="背景",
        items=[
            ("BLACK", "黑色", "使用黑色背景"),
            ("WORLD", "World 表面", "使用 Blender World Surface 的 Background 颜色和强度作为纯色背景"),
            ("COLOR", "自定义颜色", "使用下方背景颜色"),
            ("SKY", "天空", "使用 raytracer 内置蓝白天空渐变"),
        ],
        default="BLACK",
    )
    background_color: FloatVectorProperty(
        name="背景颜色",
        subtype="COLOR",
        default=(0.0, 0.0, 0.0),
        min=0.0,
        max=1.0,
    )
    ambient_enabled: BoolProperty(name="环境光", default=False)
    ambient_color: FloatVectorProperty(
        name="环境光颜色",
        subtype="COLOR",
        default=(1.0, 1.0, 1.0),
        min=0.0,
        max=1.0,
    )
    ambient_strength: FloatProperty(name="环境光强度", default=5.0, min=0.0, max=100.0, precision=3)
    debug_cache_dir: StringProperty(
        name="调试缓存",
        subtype="DIR_PATH",
        default="",
        description="可选目录；填写后每次渲染会保存 scene JSON、日志和结果等调试缓存",
    )


class RaytracerRenderEngine(bpy.types.RenderEngine):
    bl_idname = ENGINE_ID
    bl_label = "Raytracer"
    bl_use_preview = False

    def render(self, depsgraph):
        scene = depsgraph.scene
        settings = scene.raytracer_settings
        try:
            package = export_scene_package(scene, depsgraph, settings)
            debug_cache = create_debug_cache(settings)
            write_debug_json(debug_cache, "scene.rt.json", package)
            client = make_client(settings)
            pixels = client.render(package, self, settings, debug_cache)
            if self.test_break():
                return

            self.update_render_pixels(pixels)
            self.update_progress(1.0)
        except Exception as exc:
            message = str(exc)
            self.error_set(message)
            self.report({"ERROR"}, message)

    def update_render_pixels(self, pixels):
        result = self.begin_result(0, 0, pixels.width, pixels.height)
        layer = result.layers[0].passes["Combined"]
        layer.rect = pixels.blender_rect()
        self.end_result(result)


class RENDER_PT_raytracer_settings(bpy.types.Panel):
    bl_label = "Raytracer"
    bl_space_type = "PROPERTIES"
    bl_region_type = "WINDOW"
    bl_context = "render"
    COMPAT_ENGINES = {ENGINE_ID}

    def draw(self, context):
        settings = context.scene.raytracer_settings
        layout = self.layout
        layout.use_property_split = True
        layout.use_property_decorate = False

        backend_box = layout.box()
        backend_box.label(text="后端")
        backend_box.prop(settings, "backend")
        if settings.backend == "LOCAL":
            backend_box.prop(settings, "bridge_path")
        else:
            backend_box.prop(settings, "remote_endpoint")
            backend_box.prop(settings, "remote_timeout")

        render_box = layout.box()
        render_box.label(text="渲染")
        render_box.prop(settings, "samples")
        render_box.prop(settings, "max_depth")
        render_box.prop(settings, "threads")
        render_box.prop(settings, "direct_only")
        render_box.prop(settings, "render_schedule")
        render_box.prop(settings, "progressive_preview")

        lighting_box = layout.box()
        lighting_box.label(text="光照")
        lighting_box.prop(settings, "light_intensity_scale")
        lighting_box.prop(settings, "area_light_camera_visible")
        lighting_box.prop(settings, "ambient_enabled")
        if settings.ambient_enabled:
            lighting_box.prop(settings, "ambient_color")
            lighting_box.prop(settings, "ambient_strength")

        background_box = layout.box()
        background_box.label(text="背景")
        background_box.prop(settings, "background_mode")
        if settings.background_mode == "COLOR":
            background_box.prop(settings, "background_color")

        debug_box = layout.box()
        debug_box.label(text="调试")
        debug_box.prop(settings, "debug_cache_dir")


def compatible_panels():
    builtin_engines = {
        "BLENDER_RENDER",
        "BLENDER_EEVEE",
        "BLENDER_EEVEE_NEXT",
        "BLENDER_WORKBENCH",
        "CYCLES",
    }

    # Keep Blender-owned panels only when they edit data that the exporter reads
    # or when they are generic render/display helpers. Surface panels are selected
    # as a native panel family so Blender keeps exposing supported Principled BSDF
    # sockets such as base color, metallic, roughness, alpha, transmission, IOR and
    # emission without showing duplicate engine variants.
    exact_names = {
        "RENDER_PT_output",
        "RENDER_PT_format",
        "RENDER_PT_dimensions",
        "RENDER_PT_frame_remapping",
        "RENDER_PT_stamp",
        "RENDER_PT_post_processing",
        "RENDER_PT_color_management",
        "RENDER_PT_simplify",
        "MATERIAL_PT_context_material",
        "MATERIAL_PT_preview",
        "MATERIAL_PT_viewport",
        "WORLD_PT_context_world",
        "DATA_PT_context_light",
        "DATA_PT_light",
        "DATA_PT_EEVEE_light",
        "DATA_PT_context_camera",
        "DATA_PT_lens",
        "DATA_PT_camera",
        "DATA_PT_camera_dof",
        "DATA_PT_camera_display",
        "DATA_PT_camera_safe_areas",
    }

    allowed_prefixes = ()

    available_panel_names = {panel.__name__ for panel in bpy.types.Panel.__subclasses__()}
    preferred_material_surface_panels = (
        "EEVEE_MATERIAL_PT_surface",
        "CYCLES_MATERIAL_PT_surface",
        "MATERIAL_PT_surface",
    )
    material_panel_name = next(
        (name for name in preferred_material_surface_panels if name in available_panel_names),
        None,
    )
    if material_panel_name is not None:
        exact_names.add(material_panel_name)
        allowed_prefixes += (material_panel_name,)

    preferred_world_surface_panels = (
        "EEVEE_WORLD_PT_surface",
        "CYCLES_WORLD_PT_surface",
        "WORLD_PT_surface",
    )
    world_panel_name = next(
        (name for name in preferred_world_surface_panels if name in available_panel_names),
        None,
    )
    if world_panel_name is not None:
        exact_names.add(world_panel_name)
    exact_names.add("WORLD_PT_viewport_display")

    excluded_terms = (
        "lineart",
        "line_art",
        "freestyle",
        "grease_pencil",
        "greasepencil",
        "shadow",
        "volume",
        "volumetric",
        "mist",
        "motion_blur",
        "hair",
        "passes",
        "cryptomatte",
        "sampling",
        "film",
        "performance",
        "lightprobe",
        "light_probe",
    )

    panels = []
    for panel in bpy.types.Panel.__subclasses__():
        if not hasattr(panel, "COMPAT_ENGINES"):
            continue
        if not panel.COMPAT_ENGINES.intersection(builtin_engines):
            continue

        name = panel.__name__
        lower_name = name.lower()
        if any(term in lower_name for term in excluded_terms):
            continue
        if name in exact_names or any(name.startswith(prefix) for prefix in allowed_prefixes):
            panels.append(panel)
    return panels


def remove_raytracer_from_all_panels():
    for panel in bpy.types.Panel.__subclasses__():
        if not hasattr(panel, "COMPAT_ENGINES"):
            continue
        if ENGINE_ID in panel.COMPAT_ENGINES:
            panel.COMPAT_ENGINES.remove(ENGINE_ID)


classes = (
    RaytracerSettings,
    RaytracerRenderEngine,
    RENDER_PT_raytracer_settings,
)


def register():
    for cls in classes:
        bpy.utils.register_class(cls)
    bpy.types.Scene.raytracer_settings = PointerProperty(type=RaytracerSettings)
    remove_raytracer_from_all_panels()
    for panel in compatible_panels():
        panel.COMPAT_ENGINES.add(ENGINE_ID)


def unregister():
    remove_raytracer_from_all_panels()
    del bpy.types.Scene.raytracer_settings
    for cls in reversed(classes):
        bpy.utils.unregister_class(cls)


if __name__ == "__main__":
    register()
