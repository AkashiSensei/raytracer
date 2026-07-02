// Scene loader: JSON -> Camera + HittableList + image params
#ifndef RT_SCENE_H
#define RT_SCENE_H

#include "raytracer/geometry/bvh.h"
#include "raytracer/geometry/hittable.h"
#include "raytracer/geometry/hittable_list.h"
#include "raytracer/geometry/sphere.h"
#include "raytracer/geometry/triangle.h"
#include "raytracer/geometry/triangle_mesh.h"
#include "raytracer/material/material.h"
#include "raytracer/math/mat4.h"
#include "raytracer/math/vec3.h"
#include "raytracer/render/camera.h"
#include "raytracer/render/image.h"
#include "raytracer/scene/glb.h"
#include "raytracer/scene/json.h"
#include "raytracer/scene/obj.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

enum class LightType {
    Point,
    Directional,
    Spot,
    Sphere,
    Rect,
    Disk
};

struct Light {
    LightType type = LightType::Directional;
    Point3 position;
    Vec3 direction = Vec3(0, -1, -1);
    Vec3 u = Vec3(1, 0, 0);
    Vec3 v = Vec3(0, 0, 1);
    Color color = Color(1, 1, 1);
    double intensity = 1.0;
    double radius = 0.25;
    double angle = 30.0;
    double soft_angle = 0.0;
    bool visible_camera = false;
    bool visible_specular = true;
    bool visible_diffuse = false;

    double area() const {
        if (type == LightType::Sphere) return 4.0 * pi * radius * radius;
        if (type == LightType::Rect) return cross(u, v).length();
        if (type == LightType::Disk) return pi * radius * radius;
        return 0.0;
    }
};

enum class EnvironmentType {
    Gradient,
    Solid,
    Texture
};

struct Environment {
    EnvironmentType type = EnvironmentType::Gradient;
    Color bottom = Color(1.0, 1.0, 1.0);
    Color top = Color(0.5, 0.7, 1.0);
    Color color = Color(1.0, 1.0, 1.0);
    double intensity = 1.0;
    std::shared_ptr<Texture> texture;
};

struct EmissiveObject {
    Hittable* geometry;
    Color emission;
};

struct Scene {
    int width = 800;
    int height = 400;
    int samples = 16;
    int max_depth = 32;
    std::string output = "out.ppm";
    ImageOutputOptions output_options;
    unsigned int seed = 0;
    bool has_seed = false;

    std::unique_ptr<Camera> camera;
    HittableList primitives;
    std::unique_ptr<Hittable> world;
    size_t primitive_count = 0;
    bool has_mesh_bounds = false;
    AABB mesh_bounds;
    Color ambient_light = Color(0.04, 0.04, 0.04);
    Environment environment;
    double firefly_clamp = infinity;
    std::vector<Light> lights;
    std::vector<EmissiveObject> emissive_objects;
    double emissive_total_area = 0.0;

    std::vector<std::unique_ptr<Material>> materials;
    std::vector<std::unique_ptr<Hittable>> objects;
    std::vector<std::unique_ptr<Hittable>> emissive_samplers;
};

struct SceneLoadOptions {
    std::string model_override;
};

inline Vec3 to_vec3(const JsonValue& arr) {
    return Vec3(arr.arrVal[0].numVal,
                arr.arrVal[1].numVal,
                arr.arrVal[2].numVal);
}

inline Vec2 to_vec2(const JsonValue& arr) {
    return Vec2(arr.arrVal[0].numVal, arr.arrVal[1].numVal);
}

inline std::string resolve_asset_path(const std::filesystem::path& base_dir,
                                      const std::string& asset_path) {
    std::filesystem::path path(asset_path);
    if (path.is_relative()) path = base_dir / path;
    return path.lexically_normal().string();
}

inline std::vector<unsigned char> decode_base64(const std::string& input) {
    auto decode_char = [](unsigned char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '+') return 62;
        if (c == '/') return 63;
        return -1;
    };

    std::vector<unsigned char> out;
    int val = 0;
    int valb = -8;
    for (unsigned char c : input) {
        if (c == '=') break;
        if (std::isspace(c)) continue;
        int d = decode_char(c);
        if (d < 0) continue;
        val = (val << 6) + d;
        valb += 6;
        if (valb >= 0) {
            out.push_back(static_cast<unsigned char>((val >> valb) & 0xFF));
            valb -= 8;
        }
    }
    return out;
}

inline Mat4 parse_transform(const JsonValue& obj) {
    if (obj.has("transform")) {
        const JsonValue& t = obj.at("transform");
        Mat4 scale = Mat4::identity();
        Mat4 rotate_axis = Mat4::identity();
        Mat4 rotate_euler = Mat4::identity();
        Mat4 translate = Mat4::identity();

        if (t.has("scale")) {
            const JsonValue& s = t.at("scale");
            if (s.isArray() && s.arrVal.size() == 3)
                scale = Mat4::scale(s.arrVal[0].numVal, s.arrVal[1].numVal, s.arrVal[2].numVal);
            else
                scale = Mat4::scale(s.numVal, s.numVal, s.numVal);
        }
        if (t.has("rotate_axis")) {
            const JsonValue& ra = t.at("rotate_axis");
            Vec3 axis = to_vec3(ra.at("axis"));
            double angle = ra.at("angle").numVal;
            rotate_axis = Mat4::rotate_axis(axis, angle);
        }
        if (t.has("rotate")) {
            Vec3 r = to_vec3(t.at("rotate"));
            rotate_euler = Mat4::rotate_z(r.z) * Mat4::rotate_y(r.y) * Mat4::rotate_x(r.x);
        }
        if (t.has("translate")) {
            Vec3 tr = to_vec3(t.at("translate"));
            translate = Mat4::translate(tr.x, tr.y, tr.z);
        }
        return translate * rotate_axis * rotate_euler * scale;
    }

    double s = obj.has("scale") ? obj.at("scale").numVal : 1.0;
    Vec3 tr = obj.has("translate") ? to_vec3(obj.at("translate")) : Vec3(0, 0, 0);
    return Mat4::translate(tr.x, tr.y, tr.z) * Mat4::scale(s, s, s);
}

inline std::shared_ptr<Texture> make_solid_texture(const Color& color) {
    return std::make_shared<SolidColorTexture>(color);
}

inline std::shared_ptr<Texture> load_texture_from_path(const std::filesystem::path& base_dir,
                                                       const std::string& texture_path) {
    return std::make_shared<ImageTexture>(resolve_asset_path(base_dir, texture_path));
}

inline std::string lower_ascii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

inline std::shared_ptr<Texture> parse_texture_json(const JsonValue& texture,
                                                   const std::filesystem::path& base_dir,
                                                   const Color& fallback = Color(1, 1, 1)) {
    if (texture.type == JsonValue::Array) {
        return make_solid_texture(to_vec3(texture));
    }
    if (texture.type == JsonValue::String) {
        return load_texture_from_path(base_dir, texture.strVal);
    }
    if (!texture.isObject()) {
        return make_solid_texture(fallback);
    }

    std::string type = texture.has("type") ? lower_ascii(texture.at("type").strVal) : "image";
    std::shared_ptr<Texture> result;
    if (type == "checker" || type == "checkerboard") {
        Color color1 = texture.has("color1") ? to_vec3(texture.at("color1")) : Color(0.8, 0.8, 0.8);
        Color color2 = texture.has("color2") ? to_vec3(texture.at("color2")) : Color(0.2, 0.2, 0.2);
        double scale = texture.has("scale") ? texture.at("scale").numVal : 5.0;
        auto checker = std::make_shared<CheckerTexture>(color1, color2, scale);
        if (texture.has("uv_scale")) checker->uv_scale = to_vec2(texture.at("uv_scale"));
        if (texture.has("uv_offset")) checker->uv_offset = to_vec2(texture.at("uv_offset"));
        if (texture.has("uv_rotation")) checker->uv_rotation = texture.at("uv_rotation").numVal;
        result = checker;
    } else if (type == "color_ramp" || type == "colorramp" || type == "ramp") {
        std::shared_ptr<Texture> source = texture.has("source")
            ? parse_texture_json(texture.at("source"), base_dir, Color(0, 0, 0))
            : make_solid_texture(Color(0, 0, 0));
        std::vector<std::pair<double, Color>> stops;
        if (texture.has("stops") && texture.at("stops").isArray()) {
            for (const JsonValue& stop : texture.at("stops").arrVal) {
                if (!stop.isObject() || !stop.has("position") || !stop.has("color")) continue;
                stops.push_back({std::clamp(stop.at("position").numVal, 0.0, 1.0),
                                 to_vec3(stop.at("color"))});
            }
        }
        auto ramp = std::make_shared<ColorRampTexture>(source, stops);
        if (texture.has("interpolation")) {
            std::string interpolation = lower_ascii(texture.at("interpolation").strVal);
            if (interpolation == "constant") ramp->interpolation = ColorRampTexture::Interpolation::Constant;
            else if (interpolation == "ease" || interpolation == "easing") ramp->interpolation = ColorRampTexture::Interpolation::Ease;
        }
        result = ramp;
    } else if (type == "math") {
        auto source_a = texture.has("a")
            ? parse_texture_json(texture.at("a"), base_dir, Color(0, 0, 0))
            : make_solid_texture(Color(0, 0, 0));
        auto source_b = texture.has("b")
            ? parse_texture_json(texture.at("b"), base_dir, Color(0, 0, 0))
            : make_solid_texture(Color(0, 0, 0));
        std::string operation = texture.has("operation")
            ? lower_ascii(texture.at("operation").strVal)
            : "add";
        MathTexture::Operation op = MathTexture::Operation::Add;
        if (operation == "subtract") op = MathTexture::Operation::Subtract;
        else if (operation == "multiply") op = MathTexture::Operation::Multiply;
        else if (operation == "divide") op = MathTexture::Operation::Divide;
        else if (operation == "minimum") op = MathTexture::Operation::Minimum;
        else if (operation == "maximum") op = MathTexture::Operation::Maximum;
        else if (operation == "power") op = MathTexture::Operation::Power;
        else if (operation == "less_than" || operation == "lessthan") op = MathTexture::Operation::LessThan;
        else if (operation == "greater_than" || operation == "greaterthan") op = MathTexture::Operation::GreaterThan;
        auto math = std::make_shared<MathTexture>(source_a, source_b, op);
        if (texture.has("clamp")) math->clamp_result = texture.at("clamp").boolVal;
        result = math;
    } else if (type == "mix") {
        auto factor = texture.has("factor")
            ? parse_texture_json(texture.at("factor"), base_dir, Color(0.5, 0.5, 0.5))
            : make_solid_texture(Color(0.5, 0.5, 0.5));
        auto color1 = texture.has("color1")
            ? parse_texture_json(texture.at("color1"), base_dir, Color(0, 0, 0))
            : make_solid_texture(Color(0, 0, 0));
        auto color2 = texture.has("color2")
            ? parse_texture_json(texture.at("color2"), base_dir, Color(1, 1, 1))
            : make_solid_texture(Color(1, 1, 1));
        result = std::make_shared<MixTexture>(factor, color1, color2);
    } else if (type == "invert") {
        auto factor = texture.has("factor")
            ? parse_texture_json(texture.at("factor"), base_dir, Color(1, 1, 1))
            : make_solid_texture(Color(1, 1, 1));
        auto color = texture.has("color")
            ? parse_texture_json(texture.at("color"), base_dir, fallback)
            : make_solid_texture(fallback);
        result = std::make_shared<InvertTexture>(factor, color);
    } else if (type == "map_range" || type == "maprange") {
        auto value = texture.has("value")
            ? parse_texture_json(texture.at("value"), base_dir, Color(0, 0, 0))
            : make_solid_texture(Color(0, 0, 0));
        auto mapped = std::make_shared<MapRangeTexture>(value);
        if (texture.has("from_min")) mapped->from_min = texture.at("from_min").numVal;
        if (texture.has("from_max")) mapped->from_max = texture.at("from_max").numVal;
        if (texture.has("to_min")) mapped->to_min = texture.at("to_min").numVal;
        if (texture.has("to_max")) mapped->to_max = texture.at("to_max").numVal;
        if (texture.has("clamp")) mapped->clamp_result = texture.at("clamp").boolVal;
        result = mapped;
    } else if (type == "noise" || type == "noise_texture") {
        auto noise = std::make_shared<NoiseTexture>();
        if (texture.has("scale")) noise->scale = texture.at("scale").numVal;
        if (texture.has("detail")) noise->detail = static_cast<int>(std::round(texture.at("detail").numVal));
        if (texture.has("roughness")) noise->roughness = texture.at("roughness").numVal;
        if (texture.has("distortion")) noise->distortion = texture.at("distortion").numVal;
        result = noise;
    } else if (type == "image" || type == "texture") {
        std::shared_ptr<ImageTexture> image;
        if (texture.has("data_base64")) {
            std::string mime = texture.has("mime_type") ? texture.at("mime_type").strVal : "image/png";
            std::vector<unsigned char> encoded = decode_base64(texture.at("data_base64").strVal);
            if (encoded.empty()) {
                throw std::runtime_error("image texture data_base64 is empty or invalid");
            }
            image = std::make_shared<ImageTexture>(encoded, mime);
        } else {
            std::string path;
            if (texture.has("path")) path = texture.at("path").strVal;
            else if (texture.has("file")) path = texture.at("file").strVal;
            else if (texture.has("texture")) path = texture.at("texture").strVal;
            if (path.empty()) {
                throw std::runtime_error(
                    "image texture requires path or data_base64; rebuild raytracer_server if using remote embedded textures");
            }
            image = std::make_shared<ImageTexture>(resolve_asset_path(base_dir, path));
        }
        if (texture.has("interpolation")) {
            std::string interpolation = lower_ascii(texture.at("interpolation").strVal);
            if (interpolation == "linear" || interpolation == "smart" || interpolation == "cubic") {
                image->interpolation = ImageTexture::Interpolation::Linear;
            }
        }
        if (texture.has("color_space")) {
            std::string color_space = lower_ascii(texture.at("color_space").strVal);
            image->decode_srgb = color_space.find("srgb") != std::string::npos;
        }
        if (texture.has("extension")) {
            std::string extension = lower_ascii(texture.at("extension").strVal);
            if (extension == "extend") image->extension = ImageTexture::Extension::Extend;
            else if (extension == "clip") image->extension = ImageTexture::Extension::Clip;
            else if (extension == "mirror" || extension == "mirror_repeat") image->extension = ImageTexture::Extension::Mirror;
            else image->extension = ImageTexture::Extension::Repeat;
        }
        result = image;
        if (texture.has("tint")) {
            result = std::make_shared<TintedTexture>(result, to_vec3(texture.at("tint")));
        }
    } else if (type == "solid" || type == "color") {
        result = make_solid_texture(texture.has("color") ? to_vec3(texture.at("color")) : fallback);
    } else {
        return make_solid_texture(fallback);
    }

    bool has_transform = texture.has("uv_scale") || texture.has("uv_offset");
    bool has_rotation = texture.has("uv_rotation");
    if ((has_transform || has_rotation) && type != "checker" && type != "checkerboard") {
        Vec2 scale = texture.has("uv_scale") ? to_vec2(texture.at("uv_scale")) : Vec2(1.0, 1.0);
        Vec2 offset = texture.has("uv_offset") ? to_vec2(texture.at("uv_offset")) : Vec2(0.0, 0.0);
        double rotation = has_rotation ? texture.at("uv_rotation").numVal : 0.0;
        result = std::make_shared<TransformedTexture>(result, scale, offset, rotation);
    }
    return result;
}

inline Vec3 safe_normalized(const Vec3& v, const Vec3& fallback) {
    double len2 = v.length_squared();
    if (len2 <= 1e-16) return fallback;
    return v / std::sqrt(len2);
}

inline Color clamp_radiance(const Color& c, double max_channel) {
    if (!std::isfinite(max_channel) || max_channel <= 0) return c;
    double peak = std::max(c.x, std::max(c.y, c.z));
    if (peak <= max_channel || peak <= 0) return c;
    return c * (max_channel / peak);
}

inline Environment parse_environment(const JsonValue& env_json,
                                     const std::filesystem::path& base_dir) {
    Environment env;

    if (env_json.isArray() && env_json.arrVal.size() >= 3) {
        env.type = EnvironmentType::Solid;
        env.color = to_vec3(env_json);
        return env;
    }

    std::string type = env_json.has("type") ? lower_ascii(env_json.at("type").strVal) : "gradient";
    if (type == "solid" || type == "color") {
        env.type = EnvironmentType::Solid;
        env.color = env_json.has("color") ? to_vec3(env_json.at("color")) : Color(1.0, 1.0, 1.0);
    } else if (type == "texture" || type == "image" || type == "hdr") {
        env.type = EnvironmentType::Texture;
        std::string path;
        if (env_json.has("texture")) path = env_json.at("texture").strVal;
        else if (env_json.has("path")) path = env_json.at("path").strVal;
        else if (env_json.has("file")) path = env_json.at("file").strVal;
        if (path.empty()) throw std::runtime_error("environment texture requires texture/path/file");
        env.texture = load_texture_from_path(base_dir, path);
    } else if (type == "gradient" || type == "sky") {
        env.type = EnvironmentType::Gradient;
        if (env_json.has("top")) env.top = to_vec3(env_json.at("top"));
        if (env_json.has("bottom")) env.bottom = to_vec3(env_json.at("bottom"));
        if (env_json.has("color")) {
            env.bottom = to_vec3(env_json.at("color"));
            env.top = env.bottom;
        }
    } else {
        throw std::runtime_error("Unknown environment type: " + type);
    }

    if (env_json.has("intensity")) env.intensity = env_json.at("intensity").numVal;
    return env;
}

inline Color scene_background(const Scene& scene, const Ray& ray) {
    const Environment& env = scene.environment;
    if (env.type == EnvironmentType::Solid) return env.color * env.intensity;

    Vec3 unit_dir = safe_normalized(ray.direction, Vec3(0, 1, 0));
    if (env.type == EnvironmentType::Texture && env.texture) {
        double u = 0.5 + std::atan2(unit_dir.z, unit_dir.x) / (2.0 * pi);
        double v = 0.5 - std::asin(std::clamp(unit_dir.y, -1.0, 1.0)) / pi;
        return env.texture->value(u, v, ray.origin + unit_dir) * env.intensity;
    }

    double t = 0.5 * (unit_dir.y + 1.0);
    return ((1 - t) * env.bottom + t * env.top) * env.intensity;
}

inline std::shared_ptr<Texture> material_texture_or_color(const JsonValue& m,
                                                          const std::filesystem::path& base_dir,
                                                          const Color& fallback) {
    if (m.has("texture")) {
        const JsonValue& texture = m.at("texture");
        return std::make_shared<TintedTexture>(
            parse_texture_json(texture, base_dir, Color(1, 1, 1)), fallback);
    }
    if (m.has("albedo_texture")) {
        return std::make_shared<TintedTexture>(
            parse_texture_json(m.at("albedo_texture"), base_dir, Color(1, 1, 1)), fallback);
    }
    return make_solid_texture(fallback);
}

inline std::shared_ptr<Texture> parse_texture_color(const JsonValue& m,
                                                    const std::string& base_key,
                                                    const Color& default_color,
                                                    const std::filesystem::path& base_dir) {
    std::string map_key = base_key + "_map";
    if (m.has(map_key)) return parse_texture_json(m.at(map_key), base_dir, default_color);
    if (m.has(base_key)) return parse_texture_json(m.at(base_key), base_dir, default_color);
    return make_solid_texture(default_color);
}

inline std::shared_ptr<Texture> parse_texture_scalar(const JsonValue& m,
                                                     const std::string& base_key,
                                                     double default_val,
                                                     const std::filesystem::path& base_dir) {
    std::string map_key = base_key + "_map";
    if (m.has(map_key)) return parse_texture_json(m.at(map_key), base_dir, Color(default_val, 0, 0));
    if (m.has(base_key) && !m.at(base_key).isNumber()) {
        return parse_texture_json(m.at(base_key), base_dir, Color(default_val, 0, 0));
    }
    double v = m.has(base_key) ? m.at(base_key).numVal : default_val;
    return make_solid_texture(Color(v, 0, 0));
}

inline Material* parse_material(const JsonValue& m,
                                Scene& scene,
                                const std::filesystem::path& base_dir = std::filesystem::current_path()) {
    const std::string& type = m.at("type").strVal;
    std::unique_ptr<Material> mat;

    if (type == "lambertian") {
        Color albedo = m.has("albedo") ? to_vec3(m.at("albedo")) : Color(1.0, 1.0, 1.0);
        mat = std::make_unique<Lambertian>(material_texture_or_color(m, base_dir, albedo));
    } else if (type == "metal") {
        double fuzz = m.has("fuzz") ? m.at("fuzz").numVal : 0.0;
        Color albedo = m.has("albedo") ? to_vec3(m.at("albedo")) : Color(0.8, 0.8, 0.8);
        mat = std::make_unique<Metal>(material_texture_or_color(m, base_dir, albedo), fuzz);
    } else if (type == "dielectric") {
        auto albedo_tex = m.has("albedo")
            ? parse_texture_json(m.at("albedo"), base_dir, Color(1.0, 1.0, 1.0))
            : make_solid_texture(Color(1.0, 1.0, 1.0));
        auto dielectric = std::make_unique<Dielectric>(m.at("ior").numVal, albedo_tex);
        if (m.has("roughness")) {
            dielectric->roughness = std::clamp(m.at("roughness").numVal, 0.0, 1.0);
        }
        if (m.has("attenuation_color")) {
            dielectric->attenuation_color = to_vec3(m.at("attenuation_color"));
        }
        if (m.has("attenuation_distance")) {
            dielectric->attenuation_distance = m.at("attenuation_distance").numVal;
        }
        mat = std::move(dielectric);
    } else if (type == "pbr") {
        auto albedo_tex = parse_texture_color(m, "albedo", Color(0.8, 0.8, 0.8), base_dir);
        auto metallic_tex = parse_texture_scalar(m, "metallic", 0.0, base_dir);
        auto roughness_tex = parse_texture_scalar(m, "roughness", 0.5, base_dir);
        double met_def = (m.has("metallic") && m.at("metallic").isNumber()) ? m.at("metallic").numVal : 0.0;
        double rough_def = (m.has("roughness") && m.at("roughness").isNumber()) ? m.at("roughness").numVal : 0.5;
        auto pbr = std::make_unique<PBR>(albedo_tex, met_def, rough_def);
        pbr->metallic = metallic_tex;
        pbr->roughness = roughness_tex;
        if (m.has("normal_map")) {
            pbr->normal = load_texture_from_path(base_dir, m.at("normal_map").strVal);
            pbr->has_normal_map = true;
        }
        mat = std::move(pbr);
    } else if (type == "emissive") {
        mat = std::make_unique<Emissive>(to_vec3(m.at("emission")));
    } else {
        throw std::runtime_error("Unknown material type: " + type);
    }

    Material* ptr = mat.get();
    scene.materials.push_back(std::move(mat));
    return ptr;
}

inline Material* ensure_material(const JsonValue& obj,
                                 Scene& scene,
                                 const std::filesystem::path& base_dir = std::filesystem::current_path()) {
    if (obj.has("material")) return parse_material(obj.at("material"), scene, base_dir);
    JsonValue fallback;
    fallback.type = JsonValue::Object;
    JsonValue type;
    type.type = JsonValue::String;
    type.strVal = "lambertian";
    JsonValue albedo;
    albedo.type = JsonValue::Array;
    for (double c : {0.7, 0.7, 0.7}) {
        JsonValue v;
        v.type = JsonValue::Number;
        v.numVal = c;
        albedo.arrVal.push_back(v);
    }
    fallback.objVal["type"] = type;
    fallback.objVal["albedo"] = albedo;
    return parse_material(fallback, scene, base_dir);
}

inline std::shared_ptr<Texture> apply_texture_transform(std::shared_ptr<Texture> texture,
                                                       const TextureTransform& transform) {
    if (!transform.active) return texture;
    return std::make_shared<TransformedTexture>(std::move(texture), transform.scale, transform.offset);
}

inline std::shared_ptr<Texture> make_loaded_texture_by_index(const ObjMeshData& mesh,
                                                             int texture_index,
                                                             const TextureTransform& transform = {}) {
    if (texture_index >= 0 &&
        static_cast<size_t>(texture_index) < mesh.textures.size()) {
        const LoadedTextureData& texture = mesh.textures[texture_index];
        std::shared_ptr<Texture> tex;
        if (!texture.encoded.empty()) {
            tex = std::make_shared<ImageTexture>(texture.encoded, texture.mime_type);
        } else if (!texture.path.empty()) {
            tex = std::make_shared<ImageTexture>(texture.path);
        } else {
            tex = make_solid_texture(Color(1, 1, 1));
        }
        return apply_texture_transform(tex, transform);
    }
    return make_solid_texture(Color(1, 1, 1));
}

inline std::shared_ptr<Texture> make_loaded_texture(const ObjMeshData& mesh,
                                                    const LoadedMaterialData& data) {
    if (data.base_color_texture >= 0) {
        return std::make_shared<TintedTexture>(
            make_loaded_texture_by_index(mesh, data.base_color_texture, data.base_color_transform), data.albedo);
    }
    return make_solid_texture(data.albedo);
}

inline std::shared_ptr<Texture> make_loaded_scalar_texture(const ObjMeshData& mesh,
                                                           int texture_index,
                                                           int channel,
                                                           double fallback,
                                                           const TextureTransform& transform = {}) {
    if (texture_index >= 0 &&
        static_cast<size_t>(texture_index) < mesh.textures.size()) {
        return std::make_shared<TextureChannel>(
            make_loaded_texture_by_index(mesh, texture_index, transform), channel);
    }
    return make_solid_texture(Color(fallback, 0, 0));
}

inline Material* add_loaded_material(const ObjMeshData& mesh,
                                      const LoadedMaterialData& data,
                                      Scene& scene) {
    std::unique_ptr<Material> mat;
    bool has_emission = data.emissive.length_squared() > 1e-12;
    if (data.transmission > 0.5 || data.transmission_texture >= 0 || data.alpha_blend) {
        auto dielectric = std::make_unique<Dielectric>(data.ior, make_loaded_texture(mesh, data));
        dielectric->attenuation_color = data.attenuation_color;
        dielectric->attenuation_distance = data.attenuation_distance;
        // Only apply GLB roughness when KHR_materials_transmission is
        // present (intentional glass material with rough transmission).
        // For alphaMode:BLEND-only assets like glass.glb, the glTF default
        // roughness of 1.0 would make the glass a blurry mess; keeping
        // roughness=0 matches the expected smooth-glass appearance and the
        // pre-transmission-rewrite baseline.
        if (data.transmission > 0.0 || data.transmission_texture >= 0) {
            dielectric->roughness = std::clamp(data.roughness, 0.0, 1.0);
        }
        dielectric->transmission = std::clamp(data.transmission > 0.0 ? data.transmission : 1.0, 0.0, 1.0);
        dielectric->double_sided = data.double_sided;
        if (data.transmission_texture >= 0) {
            dielectric->transmission_texture = make_loaded_scalar_texture(mesh, data.transmission_texture, 0, dielectric->transmission, data.transmission_transform);
        }
        if (std::isfinite(dielectric->attenuation_distance) && data.thickness_factor > 1e-6) {
            dielectric->attenuation_distance /= data.thickness_factor;
        }
        if (data.thickness_texture >= 0) {
            auto thickness_tex = make_loaded_scalar_texture(mesh, data.thickness_texture, 0, data.thickness_factor, data.thickness_transform);
            (void)thickness_tex;  // stored on dielectric if per-pixel thickness supported later
        }
        mat = std::move(dielectric);
    } else if (data.use_pbr) {
        auto pbr = std::make_unique<PBR>(make_loaded_texture(mesh, data), data.metallic, data.roughness);
        if (data.metallic_roughness_texture >= 0) {
            pbr->roughness = make_loaded_scalar_texture(mesh, data.metallic_roughness_texture, 1, data.roughness, data.metallic_roughness_transform);
            pbr->metallic = make_loaded_scalar_texture(mesh, data.metallic_roughness_texture, 2, data.metallic, data.metallic_roughness_transform);
        }
        // MTL map_Ks drives metallic, map_Ns drives roughness (inverted).
        if (data.specular_texture >= 0) {
            pbr->metallic = make_loaded_scalar_texture(mesh, data.specular_texture, 0, data.metallic);
        }
        if (data.shininess_texture >= 0) {
            pbr->roughness = make_loaded_scalar_texture(mesh, data.shininess_texture, 0, data.roughness);
        }
        if (data.normal_texture >= 0) {
            pbr->normal = make_loaded_texture_by_index(mesh, data.normal_texture, data.normal_transform);
            pbr->has_normal_map = true;
        }
        if (has_emission) {
            pbr->emission = data.emissive;
            if (data.emissive_texture >= 0) {
                pbr->emission_texture = make_loaded_texture_by_index(mesh, data.emissive_texture, data.emissive_transform);
            }
        }
        if (data.alpha_mask) {
            pbr->alpha_masked = true;
            pbr->cutoff = data.alpha_cutoff;
        }
        mat = std::move(pbr);
    } else if (has_emission) {
        if (data.emissive_texture >= 0) {
            mat = std::make_unique<Emissive>(
                data.emissive, make_loaded_texture_by_index(mesh, data.emissive_texture, data.emissive_transform));
        } else {
            mat = std::make_unique<Emissive>(data.emissive);
        }
    } else if (data.metallic > 0.5) {
        double fuzz = std::clamp(data.roughness, 0.0, 1.0);
        mat = std::make_unique<Metal>(make_loaded_texture(mesh, data), fuzz);
    } else {
        auto lambert = std::make_unique<Lambertian>(make_loaded_texture(mesh, data));
        if (data.alpha_mask) {
            lambert->alpha_masked = true;
            lambert->cutoff = data.alpha_cutoff;
        }
        mat = std::move(lambert);
    }

    Material* ptr = mat.get();
    scene.materials.push_back(std::move(mat));
    return ptr;
}

inline Vec3 tangent_for_direction(const Vec3& direction) {
    Vec3 n = safe_normalized(direction, Vec3(0, -1, 0));
    Vec3 up = std::fabs(n.y) < 0.99 ? Vec3(0, 1, 0) : Vec3(1, 0, 0);
    return safe_normalized(cross(up, n), Vec3(1, 0, 0));
}

inline Light parse_light(const JsonValue& light_json) {
    Light light;
    std::string type = light_json.has("type") ? lower_ascii(light_json.at("type").strVal) : "directional";
    if (type == "point") {
        light.type = LightType::Point;
        light.position = light_json.has("position") ? to_vec3(light_json.at("position")) : Point3(0, 3, 4);
    } else if (type == "directional") {
        light.type = LightType::Directional;
        light.direction = safe_normalized(
            light_json.has("direction") ? to_vec3(light_json.at("direction")) : Vec3(-1, -1, -1),
            Vec3(-1, -1, -1).normalized());
    } else if (type == "spot") {
        light.type = LightType::Spot;
        light.position = light_json.has("position") ? to_vec3(light_json.at("position")) : Point3(0, 3, 4);
        light.direction = safe_normalized(
            light_json.has("direction") ? to_vec3(light_json.at("direction")) : Vec3(0, -1, 0),
            Vec3(0, -1, 0));
        light.angle = light_json.has("angle") ? light_json.at("angle").numVal : 30.0;
        light.soft_angle = light_json.has("soft_angle") ? light_json.at("soft_angle").numVal : 0.0;
    } else if (type == "sphere") {
        light.type = LightType::Sphere;
        light.position = light_json.has("position") ? to_vec3(light_json.at("position")) : Point3(0, 3, 4);
        light.radius = light_json.has("radius") ? light_json.at("radius").numVal : 0.25;
    } else if (type == "rect" || type == "rectangle" || type == "quad" || type == "softbox") {
        light.type = LightType::Rect;
        light.position = light_json.has("position") ? to_vec3(light_json.at("position")) : Point3(0, 3, 0);
        light.direction = safe_normalized(
            light_json.has("direction") ? to_vec3(light_json.at("direction")) : Vec3(0, -1, 0),
            Vec3(0, -1, 0));
        if (light_json.has("u") && light_json.has("v")) {
            light.u = to_vec3(light_json.at("u"));
            light.v = to_vec3(light_json.at("v"));
        } else {
            double width = light_json.has("width") ? light_json.at("width").numVal : 1.0;
            double height = light_json.has("height") ? light_json.at("height").numVal : width;
            Vec3 tangent = tangent_for_direction(light.direction);
            Vec3 bitangent = safe_normalized(cross(light.direction, tangent), Vec3(0, 0, 1));
            light.u = width * tangent;
            light.v = height * bitangent;
        }
    } else if (type == "disk" || type == "circle") {
        light.type = LightType::Disk;
        light.position = light_json.has("position") ? to_vec3(light_json.at("position")) : Point3(0, 3, 0);
        light.direction = safe_normalized(
            light_json.has("direction") ? to_vec3(light_json.at("direction")) : Vec3(0, -1, 0),
            Vec3(0, -1, 0));
        light.radius = light_json.has("radius") ? light_json.at("radius").numVal : 0.5;
    } else {
        throw std::runtime_error("Unknown light type: " + type);
    }

    if (light_json.has("color")) light.color = to_vec3(light_json.at("color"));
    if (light_json.has("intensity")) light.intensity = light_json.at("intensity").numVal;
    if (light_json.has("visible_camera")) light.visible_camera = light_json.at("visible_camera").boolVal;
    if (light_json.has("visible_specular")) light.visible_specular = light_json.at("visible_specular").boolVal;
    if (light_json.has("visible_diffuse")) light.visible_diffuse = light_json.at("visible_diffuse").boolVal;
    return light;
}

inline std::vector<Light> default_lights() {
    Light sun;
    sun.type = LightType::Directional;
    sun.direction = Vec3(-1, -1.5, -1).normalized();
    sun.color = Color(1.0, 0.96, 0.9);
    sun.intensity = 0.9;

    Light fill;
    fill.type = LightType::Point;
    fill.position = Point3(3.0, 4.0, 5.0);
    fill.color = Color(0.8, 0.9, 1.0);
    fill.intensity = 5.0;

    return {sun, fill};
}

inline std::vector<Light> studio_softbox_lights() {
    Light key;
    key.type = LightType::Rect;
    key.position = Point3(0.0, 4.0, 3.0);
    key.direction = Vec3(0, -1, -0.8).normalized();
    key.u = Vec3(3.0, 0, 0);
    key.v = Vec3(0, 0, 2.0);
    key.color = Color(1.0, 0.96, 0.9);
    key.intensity = 20.0;

    Light rim;
    rim.type = LightType::Directional;
    rim.direction = Vec3(1.2, -1.0, 0.6).normalized();
    rim.color = Color(0.75, 0.85, 1.0);
    rim.intensity = 0.35;

    return {key, rim};
}

inline std::string scene_preset_name(const JsonValue& root) {
    if (root.has("scene") && root.at("scene").has("preset")) {
        return lower_ascii(root.at("scene").at("preset").strVal);
    }
    if (root.has("preset")) return lower_ascii(root.at("preset").strVal);
    return "";
}

inline std::vector<Light> lights_for_preset(const std::string& preset) {
    if (preset == "studio_softbox" ||
        preset == "material_studio" ||
        preset == "material_test_studio" ||
        preset == "studio") {
        return studio_softbox_lights();
    }
    if (preset.empty()) return default_lights();
    throw std::runtime_error("Unknown scene preset: " + preset);
}

inline void parse_render_settings(const JsonValue& img, Scene& scene) {
    if (img.has("width"))     scene.width     = static_cast<int>(img.at("width").numVal);
    if (img.has("height"))    scene.height    = static_cast<int>(img.at("height").numVal);
    if (img.has("samples"))   scene.samples   = static_cast<int>(img.at("samples").numVal);
    if (img.has("max_depth")) scene.max_depth = static_cast<int>(img.at("max_depth").numVal);
    if (img.has("output"))    scene.output    = img.at("output").strVal;
    if (img.has("exposure"))  scene.output_options.exposure = img.at("exposure").numVal;
    if (img.has("tone_map"))  scene.output_options.tone_map = parse_tone_map_mode(img.at("tone_map").strVal);
    if (img.has("firefly_clamp")) scene.firefly_clamp = img.at("firefly_clamp").numVal;
    if (img.has("seed")) {
        scene.seed = static_cast<unsigned int>(img.at("seed").numVal);
        scene.has_seed = true;
    }
}

struct LightSample {
    Vec3 direction;
    Color radiance;
    double pdf = 0;
    double distance = infinity;
    bool is_delta = true;
};

inline Point3 sample_light_point(const Light& light, double r1, double r2) {
    if (light.type == LightType::Sphere) {
        double z = 1.0 - 2.0 * r1;
        double phi = 2.0 * pi * r2;
        double xy = std::sqrt(std::max(0.0, 1.0 - z * z));
        return light.position + light.radius * Vec3(std::cos(phi) * xy, z, std::sin(phi) * xy);
    }
    if (light.type == LightType::Rect) {
        return light.position + (r1 - 0.5) * light.u + (r2 - 0.5) * light.v;
    }
    if (light.type == LightType::Disk) {
        // Uniform area sampling on a disk (concentric to reduce variance).
        double r = std::sqrt(std::clamp(r1, 0.0, 1.0));
        double phi = 2.0 * pi * r2;
        Vec3 local(r * std::cos(phi), r * std::sin(phi), 0.0);
        // Build an orthonormal basis around the disk's direction, using u/v if
        // they are valid, otherwise deriving them from direction.
        Vec3 n = safe_normalized(light.direction, Vec3(0, -1, 0));
        Vec3 axis = std::fabs(n.x) > 0.9 ? Vec3(0, 1, 0) : Vec3(1, 0, 0);
        Vec3 t = cross(n, axis).normalized();
        Vec3 b = cross(n, t);
        return light.position + light.radius * (t * local.x + b * local.y);
    }
    return light.position;
}

inline double spot_cone_factor(const Light& light, const Vec3& from_light_dir) {
    if (light.type != LightType::Spot) return 1.0;
    Vec3 axis = safe_normalized(light.direction, Vec3(0, -1, 0));
    double cos_theta = dot(safe_normalized(from_light_dir, axis), axis);
    double outer = std::cos(degrees_to_radians(std::max(0.0, light.angle)));
    if (cos_theta < outer) return 0.0;
    if (light.soft_angle <= 0.0) return 1.0;

    double inner_angle = std::max(0.0, light.angle - light.soft_angle);
    double inner = std::cos(degrees_to_radians(inner_angle));
    double denom = inner - outer;
    if (std::fabs(denom) <= 1e-8) return 1.0;
    return std::clamp((cos_theta - outer) / denom, 0.0, 1.0);
}

inline LightSample sample_scene_light(const Light& light,
                                      const Point3& p,
                                      double r1 = 0.5,
                                      double r2 = 0.5) {
    LightSample s;
    s.is_delta = light.type == LightType::Point ||
                 light.type == LightType::Directional ||
                 light.type == LightType::Spot;

    if (light.type == LightType::Directional) {
        s.direction = safe_normalized(-light.direction, Vec3(0, 1, 0));
        s.distance = infinity;
        s.radiance = light.color * light.intensity;
        return s;
    }

    Point3 light_point = sample_light_point(light, r1, r2);
    Vec3 to_light = light_point - p;
    double dist2 = to_light.length_squared();
    if (dist2 < 1e-8) return LightSample{};

    double dist = std::sqrt(dist2);
    s.direction = to_light / dist;
    s.distance = dist;

    double attenuation = 1.0 / dist2;
    if (light.type == LightType::Spot) {
        attenuation *= spot_cone_factor(light, -s.direction);
    } else if (light.type == LightType::Sphere) {
        attenuation *= std::max(light.area(), 1e-8);
        s.is_delta = false;
    } else if (light.type == LightType::Rect) {
        Vec3 normal = safe_normalized(cross(light.u, light.v), -light.direction);
        double facing = dot(safe_normalized(light.direction, -normal), -s.direction);
        double cos_light = std::max(0.0, std::fabs(dot(normal, -s.direction)));
        if (facing <= 0.0 || cos_light <= 0.0) return LightSample{};
        attenuation *= std::max(light.area(), 1e-8) * cos_light;
        s.pdf = dist2 / (cos_light * std::max(light.area(), 1e-8));
        s.is_delta = false;
    } else if (light.type == LightType::Disk) {
        Vec3 n = safe_normalized(light.direction, Vec3(0, -1, 0));
        double cos_light = std::max(0.0, dot(n, -s.direction));
        double facing = dot(safe_normalized(light.direction, -n), -s.direction);
        if (facing <= 0.0 || cos_light <= 0.0) return LightSample{};
        attenuation *= std::max(light.area(), 1e-8) * cos_light;
        s.pdf = dist2 / (cos_light * std::max(light.area(), 1e-8));
        s.is_delta = false;
    }

    if (attenuation <= 0.0) return LightSample{};
    s.radiance = light.color * (light.intensity * attenuation);
    return s;
}

inline LightSample sample_any_light(const Scene& scene, const Point3& p) {
    size_t n_delta = scene.lights.size();
    size_t n_area = scene.emissive_objects.size();
    size_t total = n_delta + n_area;
    if (total == 0) return LightSample{};

    double choice = random_double() * total;
    LightSample s;

    if (choice < n_delta) {
        const Light& light = scene.lights[static_cast<size_t>(choice)];
        s.pdf = 1.0 / total;
        LightSample light_sample = sample_scene_light(light, p, random_double(), random_double());
        light_sample.pdf = s.pdf;
        return light_sample;
    } else {
        size_t idx = static_cast<size_t>(choice - n_delta);
        if (idx >= n_area) return LightSample{};
        const EmissiveObject& eo = scene.emissive_objects[idx];
        double r1 = random_double(), r2 = random_double();
        Vec3 light_normal;
        Point3 light_point = eo.geometry->sample_point(r1, r2, &light_normal);

        Vec3 to_light = light_point - p;
        double dist2 = to_light.length_squared();
        if (dist2 < 1e-8) return LightSample{};
        double dist = std::sqrt(dist2);
        s.direction = to_light / dist;
        s.distance = dist;

        double cos_light = dot(light_normal, -s.direction);
        if (cos_light <= 1e-8) return LightSample{};

        double geom_area = eo.geometry->area();
        if (geom_area <= 0) return LightSample{};
        if (scene.emissive_total_area <= 0) return LightSample{};

        s.radiance = eo.emission;
        s.is_delta = false;
        s.pdf = (dist2 / cos_light) / scene.emissive_total_area * (double(n_area) / total);
    }
    return s;
}

inline std::string lower_ext(const std::string& path) {
    std::string ext = std::filesystem::path(path).extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return ext;
}

inline ObjMeshData load_model_mesh(const std::string& path) {
    std::string ext = lower_ext(path);
    if (ext == ".obj") return load_obj_mesh(path);
    if (ext == ".glb") return load_glb_mesh(path);
    throw std::runtime_error("Unsupported model format: " + ext + " (" + path + ")");
}

inline ObjMeshData transform_mesh(const ObjMeshData& input, const Mat4& transform) {
    ObjMeshData output = input;
    bool first = true;
    AABB box;

    for (ObjTriangleData& tri : output.triangles) {
        tri.v0 = transform.transform_point(tri.v0);
        tri.v1 = transform.transform_point(tri.v1);
        tri.v2 = transform.transform_point(tri.v2);

        if (tri.has_normals) {
            tri.n0 = transform.transform_normal(tri.n0).normalized();
            tri.n1 = transform.transform_normal(tri.n1).normalized();
            tri.n2 = transform.transform_normal(tri.n2).normalized();
        }

        AABB tri_box;
        Triangle(tri.v0, tri.v1, tri.v2).bounding_box(tri_box);
        box = first ? tri_box : AABB::surrounding_box(box, tri_box);
        first = false;
    }

    output.bounds = box;
    return output;
}

inline bool get_bool(const JsonValue& obj, const std::string& key, bool fallback) {
    return obj.has(key) ? obj.at(key).boolVal : fallback;
}

inline Mat4 mesh_transform_from_object(const ObjMeshData& raw_mesh, const JsonValue& obj) {
    bool has_manual_transform = obj.has("transform") || obj.has("scale") || obj.has("translate");
    bool auto_fit = get_bool(obj, "auto_fit", !has_manual_transform);
    if (!auto_fit) return parse_transform(obj);

    double scale = obj.has("scale") ? obj.at("scale").numVal : 1.0;
    Vec3 translate = obj.has("translate") ? to_vec3(obj.at("translate")) : Vec3(0, 0, 0);
    double fit_size = obj.has("fit_size") ? obj.at("fit_size").numVal : 3.0;
    Point3 fit_center = obj.has("fit_center") ? to_vec3(obj.at("fit_center")) : Point3(0, 0, 0);
    double raw_size = raw_mesh.bounds.max_extent();
    if (raw_size <= 1e-8) {
        throw std::runtime_error("Model bounds are too small");
    }

    scale *= fit_size / raw_size;
    translate = fit_center - scale * raw_mesh.bounds.center() + translate;
    return Mat4::translate(translate.x, translate.y, translate.z) *
           Mat4::scale(scale, scale, scale);
}

inline void add_mesh_bounds(Scene& scene, const AABB& bounds) {
    scene.mesh_bounds = scene.has_mesh_bounds
        ? AABB::surrounding_box(scene.mesh_bounds, bounds)
        : bounds;
    scene.has_mesh_bounds = true;
}

inline Material* make_lambertian_material(Scene& scene, const Color& color) {
    auto mat = std::make_unique<Lambertian>(color);
    Material* ptr = mat.get();
    scene.materials.push_back(std::move(mat));
    return ptr;
}

inline std::unique_ptr<TriangleMesh> make_triangle_mesh_from_loaded_mesh(
    const ObjMeshData& mesh,
    Material* fallback_mat,
    const std::vector<Material*>& embedded_materials) {
    auto tri_mesh = std::make_unique<TriangleMesh>();
    bool any_uv = false;
    for (const ObjTriangleData& tri : mesh.triangles) {
        any_uv = any_uv || tri.has_uvs;
    }

    tri_mesh->vertices.reserve(mesh.triangles.size() * 3);
    tri_mesh->normals.reserve(mesh.triangles.size() * 3);
    if (any_uv) tri_mesh->uvs.reserve(mesh.triangles.size() * 3);
    tri_mesh->indices.reserve(mesh.triangles.size() * 3);
    tri_mesh->material_per_tri.reserve(mesh.triangles.size());

    for (const ObjTriangleData& tri : mesh.triangles) {
        int base = static_cast<int>(tri_mesh->vertices.size());
        tri_mesh->vertices.push_back(tri.v0);
        tri_mesh->vertices.push_back(tri.v1);
        tri_mesh->vertices.push_back(tri.v2);
        tri_mesh->indices.push_back(base);
        tri_mesh->indices.push_back(base + 1);
        tri_mesh->indices.push_back(base + 2);

        Vec3 face_n = cross(tri.v1 - tri.v0, tri.v2 - tri.v0);
        if (face_n.length_squared() < 1e-12) face_n = Vec3(0, 1, 0);
        face_n = face_n.normalized();
        tri_mesh->normals.push_back(tri.has_normals ? tri.n0 : face_n);
        tri_mesh->normals.push_back(tri.has_normals ? tri.n1 : face_n);
        tri_mesh->normals.push_back(tri.has_normals ? tri.n2 : face_n);

        if (any_uv) {
            tri_mesh->uvs.push_back(tri.has_uvs ? tri.uv0 : Vec2(0, 0));
            tri_mesh->uvs.push_back(tri.has_uvs ? tri.uv1 : Vec2(0, 0));
            tri_mesh->uvs.push_back(tri.has_uvs ? tri.uv2 : Vec2(0, 0));
        }

        Material* tri_mat = fallback_mat;
        if (tri.material_index >= 0 &&
            static_cast<size_t>(tri.material_index) < embedded_materials.size()) {
            tri_mat = embedded_materials[static_cast<size_t>(tri.material_index)];
        }
        tri_mesh->material_per_tri.push_back(tri_mat);
    }

    tri_mesh->acceleration_node_count();
    return tri_mesh;
}

inline void add_auto_ground(const JsonValue& root,
                            Scene& scene,
                            const std::filesystem::path& scene_dir) {
    if (!root.has("ground")) return;

    const JsonValue& ground = root.at("ground");
    if (ground.has("enabled") && !ground.at("enabled").boolVal) return;

    AABB bounds;
    if (scene.has_mesh_bounds) {
        bounds = scene.mesh_bounds;
    } else if (!scene.primitives.bounding_box(bounds)) {
        return;
    } else {
        scene.mesh_bounds = bounds;
        scene.has_mesh_bounds = true;
    }

    double max_extent = bounds.max_extent();
    if (max_extent <= 1e-8) max_extent = 1.0;

    Point3 center = bounds.center();
    double half_size = ground.has("size")
        ? 0.5 * ground.at("size").numVal
        : std::max(3.0, max_extent * 1.8);
    double y = ground.has("y")
        ? ground.at("y").numVal
        : bounds.minimum.y - std::max(1e-4, max_extent * 0.0005);

    Color default_color(0.62, 0.60, 0.55);
    Material* mat = ground.has("material")
        ? parse_material(ground.at("material"), scene, scene_dir)
        : make_lambertian_material(scene, default_color);

    Point3 p0(center.x - half_size, y, center.z - half_size);
    Point3 p1(center.x + half_size, y, center.z - half_size);
    Point3 p2(center.x + half_size, y, center.z + half_size);
    Point3 p3(center.x - half_size, y, center.z + half_size);

    auto tri_a = std::make_unique<Triangle>(p0, p2, p1, mat);
    scene.primitives.add(tri_a.get());
    scene.objects.push_back(std::move(tri_a));

    auto tri_b = std::make_unique<Triangle>(p0, p3, p2, mat);
    scene.primitives.add(tri_b.get());
    scene.objects.push_back(std::move(tri_b));
}

inline double camera_vfov_from_json(const JsonValue& c, double aspect, double fallback) {
    if (c.has("vfov")) return c.at("vfov").numVal;
    if (c.has("focal_length")) {
        double focal_length = c.at("focal_length").numVal;
        if (focal_length > 0) {
            double sensor_height = 0.0;
            if (c.has("sensor_height")) {
                sensor_height = c.at("sensor_height").numVal;
            } else if (c.has("sensor_width") && aspect > 0) {
                sensor_height = c.at("sensor_width").numVal / aspect;
            }
            if (sensor_height > 0) {
                return 2.0 * std::atan(sensor_height / (2.0 * focal_length)) * 180.0 / pi;
            }
        }
    }
    return fallback;
}

inline bool camera_has_frame(const JsonValue& c) {
    if (!c.has("frame")) return false;
    const JsonValue& frame = c.at("frame");
    return frame.has("lower_left") && frame.has("horizontal") && frame.has("vertical");
}

inline std::unique_ptr<Camera> make_frame_camera(const JsonValue& c, double aspect) {
    const JsonValue& frame = c.at("frame");
    Point3 lookfrom = to_vec3(c.at("lookfrom"));
    Point3 lower_left = to_vec3(frame.at("lower_left"));
    Vec3 horizontal = to_vec3(frame.at("horizontal"));
    Vec3 vertical = to_vec3(frame.at("vertical"));
    double vfov = camera_vfov_from_json(c, aspect, 60.0);
    double aperture = c.has("aperture") ? c.at("aperture").numVal : 0.0;
    return std::make_unique<Camera>(lookfrom, lower_left, horizontal, vertical, aperture, vfov);
}

inline Camera make_auto_camera(const AABB& bounds, double aspect, const JsonValue* camera_json) {
    double vfov = 35.0;
    double aperture = 0.0;
    Vec3 vup(0, 1, 0);
    double margin = 1.35;
    Vec3 view_dir(0, 0, 1);
    Vec3 target_offset(0, 0, 0);
    bool has_view = false;

    if (camera_json) {
        vfov = camera_vfov_from_json(*camera_json, aspect, vfov);
        if (camera_json->has("aperture")) aperture = camera_json->at("aperture").numVal;
        if (camera_json->has("margin")) margin = camera_json->at("margin").numVal;
        if (camera_json->has("target_offset")) target_offset = to_vec3(camera_json->at("target_offset"));
        if (camera_json->has("view")) {
            has_view = true;
            std::string view = lower_ascii(camera_json->at("view").strVal);
            if (view == "front") view_dir = Vec3(0, 0, 1);
            else if (view == "back") view_dir = Vec3(0, 0, -1);
            else if (view == "right" || view == "side") view_dir = Vec3(1, 0, 0);
            else if (view == "left") view_dir = Vec3(-1, 0, 0);
            else if (view == "top") {
                view_dir = Vec3(0, 1, 0);
                vup = Vec3(0, 0, -1);
            } else if (view == "bottom") {
                view_dir = Vec3(0, -1, 0);
                vup = Vec3(0, 0, 1);
            } else if (view == "three_quarter" || view == "3/4" || view == "three-quarter") {
                view_dir = Vec3(0.75, 0.35, 1.0);
            }
        }
        if (camera_json->has("vup")) vup = to_vec3(camera_json->at("vup"));
    }

    Point3 center = bounds.center() + target_offset;
    Vec3 extent = bounds.extent();
    double fit_height = std::max(extent.y, extent.x / aspect);
    if (fit_height <= 1e-8) fit_height = 1.0;

    double theta = degrees_to_radians(vfov);
    double distance = (0.5 * fit_height) / std::tan(theta / 2);
    distance += 0.5 * extent.z;
    distance *= margin;
    if (distance < 1.0) distance = 1.0;

    Point3 lookat = center;
    Point3 lookfrom = has_view
        ? center + safe_normalized(view_dir, Vec3(0, 0, 1)) * distance
        : center + Vec3(0, 0.12 * bounds.max_extent(), distance);

    if (camera_json) {
        if (camera_json->has("lookat")) lookat = to_vec3(camera_json->at("lookat"));
        if (camera_json->has("lookfrom")) lookfrom = to_vec3(camera_json->at("lookfrom"));
    }
    double focus_dist = camera_json && camera_json->has("focus_dist")
        ? camera_json->at("focus_dist").numVal
        : (lookfrom - lookat).length();

    return Camera(lookfrom, lookat, vup, vfov, aspect, aperture, focus_dist);
}

inline std::unique_ptr<Camera> build_camera(const JsonValue& root,
                                            const Scene& scene,
                                            double aspect) {
    const JsonValue* camera_json = root.has("camera") ? &root.at("camera") : nullptr;
    bool auto_camera = !camera_json || get_bool(*camera_json, "auto", false);

    if (camera_json && camera_json->has("orbit")) {
        const JsonValue& c = *camera_json;
        const JsonValue& orbit = c.at("orbit");
        Point3 target = orbit.has("target") ? to_vec3(orbit.at("target")) : Point3(0, 0, 0);
        double yaw = degrees_to_radians(orbit.has("yaw") ? orbit.at("yaw").numVal : 0.0);
        double pitch = degrees_to_radians(orbit.has("pitch") ? orbit.at("pitch").numVal : 0.0);
        double distance = orbit.has("distance") ? orbit.at("distance").numVal : 4.0;
        double cp = std::cos(pitch);
        Vec3 offset(std::sin(yaw) * cp, std::sin(pitch), std::cos(yaw) * cp);
        Point3 lookfrom = target + distance * offset;
        Vec3 vup = c.has("vup") ? to_vec3(c.at("vup")) : Vec3(0, 1, 0);
        double vfov = camera_vfov_from_json(c, aspect, 60.0);
        double aperture = c.has("aperture") ? c.at("aperture").numVal : 0.0;
        double focus = c.has("focus_dist") ? c.at("focus_dist").numVal : distance;
        return std::make_unique<Camera>(lookfrom, target, vup, vfov, aspect, aperture, focus);
    }

    if (auto_camera) {
        AABB bounds;
        if (scene.has_mesh_bounds) {
            bounds = scene.mesh_bounds;
        } else if (!scene.primitives.bounding_box(bounds)) {
            bounds = AABB(Point3(-1, -1, -1), Point3(1, 1, 1));
        }
        return std::make_unique<Camera>(make_auto_camera(bounds, aspect, camera_json));
    }

    const JsonValue& c = *camera_json;
    if (camera_has_frame(c)) {
        return make_frame_camera(c, aspect);
    }

    Point3 lookfrom = to_vec3(c.at("lookfrom"));
    Point3 lookat   = to_vec3(c.at("lookat"));
    Vec3   vup      = c.has("vup") ? to_vec3(c.at("vup")) : Vec3(0, 1, 0);
    double vfov     = camera_vfov_from_json(c, aspect, 60.0);
    double aperture = c.has("aperture") ? c.at("aperture").numVal : 0.0;
    double focus    = c.has("focus_dist") ? c.at("focus_dist").numVal : (lookfrom - lookat).length();
    return std::make_unique<Camera>(lookfrom, lookat, vup, vfov, aspect, aperture, focus);
}

class TriangleMeshEmissiveSubset : public Hittable {
public:
    TriangleMeshEmissiveSubset(const TriangleMesh* mesh, std::vector<int> tri_indices)
        : mesh_(mesh), tri_indices_(std::move(tri_indices)) {
        build_area_cdf();
    }

    bool hit(const Ray& r, double t_min, double t_max,
             HitRecord& rec) const override {
        bool hit_any = false;
        double closest = t_max;
        for (int tri_idx : tri_indices_) {
            HitRecord tmp;
            if (mesh_->hit_triangle(tri_idx, r, t_min, closest, tmp)) {
                rec = tmp;
                closest = tmp.t;
                hit_any = true;
            }
        }
        return hit_any;
    }

    bool bounding_box(AABB& output_box) const override {
        bool first = true;
        for (int tri_idx : tri_indices_) {
            Point3 p0, p1, p2;
            if (!triangle_points(tri_idx, p0, p1, p2)) continue;
            AABB box;
            Triangle(p0, p1, p2).bounding_box(box);
            output_box = first ? box : AABB::surrounding_box(output_box, box);
            first = false;
        }
        return !first;
    }

    Point3 sample_point(double r1, double r2, Vec3* normal_out = nullptr) const override {
        if (area_cdf_.empty() || area_tri_indices_.empty() || total_area_ <= 0) return Point3();

        double target = r1 * total_area_;
        auto it = std::lower_bound(area_cdf_.begin(), area_cdf_.end(), target);
        int local_idx = static_cast<int>(std::distance(area_cdf_.begin(), it));
        if (local_idx >= static_cast<int>(area_tri_indices_.size())) {
            local_idx = static_cast<int>(area_tri_indices_.size()) - 1;
        }

        double previous = local_idx > 0 ? area_cdf_[local_idx - 1] : 0.0;
        double span = area_cdf_[local_idx] - previous;
        double local_r1 = span > 0 ? (target - previous) / span : 0.0;

        Point3 p0, p1, p2;
        if (!triangle_points(area_tri_indices_[local_idx], p0, p1, p2)) return Point3();

        double sq = std::sqrt(std::clamp(local_r1, 0.0, 1.0));
        double b1 = 1.0 - sq;
        double b2 = sq * (1.0 - r2);
        double b0 = sq * r2;

        if (normal_out) {
            Vec3 n = cross(p1 - p0, p2 - p0);
            double len = n.length();
            *normal_out = len > 1e-12 ? n / len : Vec3(0, 1, 0);
        }
        return b0 * p0 + b1 * p1 + b2 * p2;
    }

    double area() const override {
        return total_area_;
    }

private:
    const TriangleMesh* mesh_;
    std::vector<int> tri_indices_;
    std::vector<int> area_tri_indices_;
    std::vector<double> area_cdf_;
    double total_area_ = 0;

    bool triangle_points(int tri_idx, Point3& p0, Point3& p1, Point3& p2) const {
        if (!mesh_ || tri_idx < 0) return false;
        size_t base = static_cast<size_t>(tri_idx) * 3;
        if (base + 2 >= mesh_->indices.size()) return false;
        int i0 = mesh_->indices[base + 0];
        int i1 = mesh_->indices[base + 1];
        int i2 = mesh_->indices[base + 2];
        if (i0 < 0 || i1 < 0 || i2 < 0) return false;
        if (static_cast<size_t>(i0) >= mesh_->vertices.size() ||
            static_cast<size_t>(i1) >= mesh_->vertices.size() ||
            static_cast<size_t>(i2) >= mesh_->vertices.size()) {
            return false;
        }
        p0 = mesh_->vertices[static_cast<size_t>(i0)];
        p1 = mesh_->vertices[static_cast<size_t>(i1)];
        p2 = mesh_->vertices[static_cast<size_t>(i2)];
        return true;
    }

    double triangle_area(int tri_idx) const {
        Point3 p0, p1, p2;
        if (!triangle_points(tri_idx, p0, p1, p2)) return 0;
        return 0.5 * cross(p1 - p0, p2 - p0).length();
    }

    void build_area_cdf() {
        area_tri_indices_.clear();
        area_cdf_.clear();
        total_area_ = 0;
        for (int tri_idx : tri_indices_) {
            double a = triangle_area(tri_idx);
            if (a <= 1e-20) continue;
            total_area_ += a;
            area_tri_indices_.push_back(tri_idx);
            area_cdf_.push_back(total_area_);
        }
    }
};

inline void collect_emissive_objects(Scene& scene) {
    scene.emissive_objects.clear();
    scene.emissive_samplers.clear();
    scene.emissive_total_area = 0.0;
    for (Hittable* obj : scene.primitives.objects) {
        Sphere* sph = dynamic_cast<Sphere*>(obj);
        if (sph && sph->material && sph->material->is_emissive()) {
            Emissive* em = static_cast<Emissive*>(sph->material);
            scene.emissive_objects.push_back({sph, em->emission});
            continue;
        }
        Triangle* tri = dynamic_cast<Triangle*>(obj);
        if (tri && tri->material && tri->material->is_emissive()) {
            Emissive* em = static_cast<Emissive*>(tri->material);
            scene.emissive_objects.push_back({tri, em->emission});
            continue;
        }
        TriangleMesh* mesh = dynamic_cast<TriangleMesh*>(obj);
        if (mesh) {
            std::vector<Material*> emissive_materials;
            std::vector<std::vector<int>> tris_by_material;
            for (size_t tri_idx = 0; tri_idx < mesh->material_per_tri.size(); tri_idx++) {
                Material* mat = mesh->material_per_tri[tri_idx];
                if (!mat || !mat->is_emissive()) continue;

                size_t group = 0;
                for (; group < emissive_materials.size(); group++) {
                    if (emissive_materials[group] == mat) break;
                }
                if (group == emissive_materials.size()) {
                    emissive_materials.push_back(mat);
                    tris_by_material.push_back(std::vector<int>());
                }
                tris_by_material[group].push_back(static_cast<int>(tri_idx));
            }

            for (size_t group = 0; group < emissive_materials.size(); group++) {
                Emissive* em = dynamic_cast<Emissive*>(emissive_materials[group]);
                if (!em) continue;
                auto sampler = std::make_unique<TriangleMeshEmissiveSubset>(
                    mesh, tris_by_material[group]);
                if (sampler->area() <= 0) continue;
                Hittable* sampler_ptr = sampler.get();
                scene.emissive_samplers.push_back(std::move(sampler));
                scene.emissive_objects.push_back({sampler_ptr, em->emission});
            }
        }
    }
    for (const EmissiveObject& eo : scene.emissive_objects) {
        scene.emissive_total_area += eo.geometry->area();
    }
}

inline void load_scene(const std::string& path,
                       Scene& scene,
                       const SceneLoadOptions& options = SceneLoadOptions()) {
    scene.primitives.clear();
    scene.world.reset();
    scene.objects.clear();
    scene.materials.clear();
    scene.primitive_count = 0;
    scene.has_mesh_bounds = false;
    scene.ambient_light = Color(0.04, 0.04, 0.04);
    scene.environment = Environment();
    scene.firefly_clamp = infinity;
    scene.lights.clear();
    scene.emissive_objects.clear();
    scene.emissive_samplers.clear();
    scene.emissive_total_area = 0.0;
    scene.output_options = ImageOutputOptions();
    scene.seed = 0;
    scene.has_seed = false;

    JsonValue root = parse_json_file(path);
    std::filesystem::path scene_dir = std::filesystem::absolute(path).parent_path();

    if (root.has("image")) parse_render_settings(root.at("image"), scene);
    if (root.has("render")) parse_render_settings(root.at("render"), scene);

    double aspect = double(scene.width) / scene.height;

    if (root.has("environment")) {
        scene.environment = parse_environment(root.at("environment"), scene_dir);
    } else if (root.has("background")) {
        scene.environment = parse_environment(root.at("background"), scene_dir);
    }

    if (root.has("lighting")) {
        const JsonValue& lighting = root.at("lighting");
        if (lighting.has("ambient")) scene.ambient_light = to_vec3(lighting.at("ambient"));
    }

    std::string preset = scene_preset_name(root);
    if (root.has("lights")) {
        for (const JsonValue& light_json : root.at("lights").arrVal) {
            scene.lights.push_back(parse_light(light_json));
        }
    } else if (!preset.empty()) {
        scene.lights = lights_for_preset(preset);
    } else {
        scene.lights = default_lights();
    }

    if (root.has("objects")) {
        for (const JsonValue& obj : root.at("objects").arrVal) {
            const std::string& type = obj.at("type").strVal;
            if (type == "sphere") {
                Point3 center = to_vec3(obj.at("center"));
                double radius = obj.at("radius").numVal;
                Material* mat = ensure_material(obj, scene, scene_dir);
                auto sph = std::make_unique<Sphere>(center, radius, mat);
                scene.primitives.add(sph.get());
                scene.objects.push_back(std::move(sph));
            } else if (type == "mesh") {
                std::string obj_source;
                if (!options.model_override.empty()) {
                    obj_source = options.model_override;
                } else if (obj.has("file")) {
                    obj_source = obj.at("file").strVal;
                } else if (obj.has("obj")) {
                    obj_source = obj.at("obj").strVal;
                } else if (obj.has("path")) {
                    obj_source = obj.at("path").strVal;
                } else {
                    throw std::runtime_error("mesh requires file/obj/path or --model override");
                }

                std::string obj_path = resolve_asset_path(scene_dir, obj_source);
                ObjMeshData raw_mesh = load_model_mesh(obj_path);
                ObjMeshData mesh = transform_mesh(raw_mesh, mesh_transform_from_object(raw_mesh, obj));
                add_mesh_bounds(scene, mesh.bounds);

                Material* fallback_mat = ensure_material(obj, scene, scene_dir);
                std::vector<Material*> embedded_materials;
                bool override_material = get_bool(obj, "override_material", false);
                if (!override_material) {
                    for (const LoadedMaterialData& material : mesh.materials) {
                        embedded_materials.push_back(add_loaded_material(mesh, material, scene));
                    }
                }

                auto mesh_ptr = make_triangle_mesh_from_loaded_mesh(mesh, fallback_mat, embedded_materials);
                scene.primitives.add(mesh_ptr.get());
                scene.objects.push_back(std::move(mesh_ptr));
            } else if (type == "triangle") {
                const JsonValue& verts = obj.at("vertices");
                Point3 a = to_vec3(verts.arrVal[0]);
                Point3 b = to_vec3(verts.arrVal[1]);
                Point3 c = to_vec3(verts.arrVal[2]);
                Material* mat = ensure_material(obj, scene, scene_dir);
                std::unique_ptr<Triangle> tri;
                if (obj.has("normals") && obj.has("uvs")) {
                    const JsonValue& norms = obj.at("normals");
                    const JsonValue& uvs = obj.at("uvs");
                    tri = std::make_unique<Triangle>(
                        a, b, c,
                        to_vec3(norms.arrVal[0]), to_vec3(norms.arrVal[1]), to_vec3(norms.arrVal[2]),
                        to_vec2(uvs.arrVal[0]), to_vec2(uvs.arrVal[1]), to_vec2(uvs.arrVal[2]),
                        mat);
                } else if (obj.has("normals")) {
                    const JsonValue& norms = obj.at("normals");
                    tri = std::make_unique<Triangle>(
                        a, b, c,
                        to_vec3(norms.arrVal[0]), to_vec3(norms.arrVal[1]), to_vec3(norms.arrVal[2]),
                        mat);
                } else {
                    tri = std::make_unique<Triangle>(a, b, c, mat);
                }
                scene.primitives.add(tri.get());
                scene.objects.push_back(std::move(tri));
            } else if (type == "triangles") {
                auto mesh = std::make_unique<TriangleMesh>();
                const JsonValue& verts = obj.at("vertices");
                for (const JsonValue& v : verts.arrVal) {
                    mesh->vertices.push_back(to_vec3(v));
                }
                const JsonValue& idxs = obj.at("indices");
                for (const JsonValue& i : idxs.arrVal) {
                    mesh->indices.push_back(static_cast<int>(i.numVal));
                }
                if (obj.has("normals")) {
                    for (const JsonValue& n : obj.at("normals").arrVal) {
                        mesh->normals.push_back(to_vec3(n));
                    }
                }
                if (obj.has("uvs")) {
                    for (const JsonValue& uv : obj.at("uvs").arrVal) {
                        mesh->uvs.push_back(to_vec2(uv));
                    }
                }
                Material* mat = ensure_material(obj, scene, scene_dir);
                size_t tri_count = mesh->indices.size() / 3;
                mesh->material_per_tri.assign(tri_count, mat);
                mesh->acceleration_node_count();
                scene.primitives.add(mesh.get());
                scene.objects.push_back(std::move(mesh));
            } else {
                throw std::runtime_error("Unknown object type: " + type);
            }
        }
    }

    add_auto_ground(root, scene, scene_dir);

    scene.camera = build_camera(root, scene, aspect);

    scene.primitive_count = scene.primitives.objects.size();
    if (scene.primitives.objects.empty()) {
        scene.world = std::make_unique<HittableList>(scene.primitives);
    } else {
        scene.world = std::make_unique<LinearBVH>(scene.primitives.objects);
    }

    collect_emissive_objects(scene);
}

#endif
