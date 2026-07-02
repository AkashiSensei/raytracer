#define TINYOBJLOADER_IMPLEMENTATION
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

#include "raytracer/render/renderer.h"
#include "raytracer/scene/scene.h"

#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <type_traits>

namespace {

int failures = 0;

void check(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << "\n";
        ++failures;
    }
}

bool near(double a, double b, double eps = 1e-9) {
    return std::fabs(a - b) <= eps;
}

bool near_vec(const Vec3& a, const Vec3& b, double eps = 1e-9) {
    return near(a.x, b.x, eps) && near(a.y, b.y, eps) && near(a.z, b.z, eps);
}

bool finite_vec(const Vec3& v) {
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

bool positive_color(const Color& c) {
    return c.x > 0 && c.y > 0 && c.z > 0;
}

JsonValue number(double value) {
    JsonValue v;
    v.type = JsonValue::Number;
    v.numVal = value;
    return v;
}

JsonValue string_value(const std::string& value) {
    JsonValue v;
    v.type = JsonValue::String;
    v.strVal = value;
    return v;
}

JsonValue array3(double x, double y, double z) {
    JsonValue v;
    v.type = JsonValue::Array;
    v.arrVal = {number(x), number(y), number(z)};
    return v;
}

template <typename T, typename = void>
struct has_acceleration_node_count : std::false_type {};

template <typename T>
struct has_acceleration_node_count<T, std::void_t<decltype(std::declval<const T&>().acceleration_node_count())>>
    : std::true_type {};

template <typename Mesh>
void check_acceleration_node_count(const Mesh& mesh) {
    if constexpr (has_acceleration_node_count<Mesh>::value) {
        check(mesh.acceleration_node_count() > 1, "TriangleMesh should build internal triangle-level acceleration");
    } else {
        check(false, "TriangleMesh should expose internal acceleration node count for regression coverage");
    }
}

void test_parse_transform_uses_srt_order() {
    JsonValue obj;
    obj.type = JsonValue::Object;
    JsonValue transform;
    transform.type = JsonValue::Object;
    transform.objVal["scale"] = number(2.0);
    transform.objVal["translate"] = array3(10.0, 0.0, 0.0);
    obj.objVal["transform"] = transform;

    Vec3 p = parse_transform(obj).transform_point(Vec3(1, 0, 0));
    check(near_vec(p, Vec3(12, 0, 0)), "transform block must apply scale before translate without scaling translation");
}

void test_transform_normal_matches_rotation_direction() {
    Mat4 r = Mat4::rotate_z(90);
    Vec3 normal = r.transform_normal(Vec3(1, 0, 0)).normalized();
    check(near_vec(normal, Vec3(0, 1, 0), 1e-9), "rotate_z(90) must rotate normals in the same direction as directions");
}

void test_sphere_pole_tangent_is_finite() {
    Sphere sphere(Point3(0, 0, 0), 1.0, nullptr);
    Ray ray(Point3(0, 2, 0), Vec3(0, -1, 0));
    HitRecord rec;
    check(sphere.hit(ray, 0.001, infinity, rec), "ray should hit sphere pole");
    check(rec.has_tangent, "sphere hit should provide tangent");
    check(finite_vec(rec.tangent) && rec.tangent.length_squared() > 0, "sphere pole tangent must be finite and non-zero");
}

void test_degenerate_triangle_uv_tangent_is_finite() {
    Triangle tri(Point3(0, 0, 0), Point3(1, 0, 0), Point3(0, 0, -1),
                 Vec3(0, 1, 0), Vec3(0, 1, 0), Vec3(0, 1, 0),
                 Vec2(0, 0), Vec2(0, 0), Vec2(0, 0), nullptr);
    Ray ray(Point3(0.25, 1, -0.25), Vec3(0, -1, 0));
    HitRecord rec;
    check(tri.hit(ray, 0.001, infinity, rec), "ray should hit degenerate-UV triangle");
    check(rec.has_tangent, "triangle with UVs should provide tangent");
    check(finite_vec(rec.tangent) && rec.tangent.length_squared() > 0, "degenerate triangle UV tangent must be finite and non-zero");
}

void test_degenerate_mesh_uv_tangent_is_finite() {
    TriangleMesh mesh;
    mesh.vertices = {Point3(0, 0, 0), Point3(1, 0, 0), Point3(0, 0, -1)};
    mesh.indices = {0, 1, 2};
    mesh.normals = {Vec3(0, 1, 0), Vec3(0, 1, 0), Vec3(0, 1, 0)};
    mesh.uvs = {Vec2(0, 0), Vec2(0, 0), Vec2(0, 0)};
    mesh.material_per_tri = {nullptr};

    Ray ray(Point3(0.25, 1, -0.25), Vec3(0, -1, 0));
    HitRecord rec;
    check(mesh.hit(ray, 0.001, infinity, rec), "ray should hit degenerate-UV mesh");
    check(rec.has_tangent, "mesh with UVs should provide tangent");
    check(finite_vec(rec.tangent) && rec.tangent.length_squared() > 0, "degenerate mesh UV tangent must be finite and non-zero");
}

void test_obj_loader_handles_mixed_missing_attributes() {
    std::filesystem::path path = std::filesystem::temp_directory_path() / "rt_mixed_attributes.obj";
    {
        std::ofstream out(path);
        out << "v 0 0 0\n"
            << "v 1 0 0\n"
            << "v 0 1 0\n"
            << "v 1 1 0\n"
            << "vt 0 0\n"
            << "vn 0 0 1\n"
            << "f 1/1/1 2/1/1 3/1/1\n"
            << "f 2 4 3\n";
    }

    TriangleMesh mesh = load_obj_mesh(path.string(), Mat4::identity());
    std::filesystem::remove(path);

    check(mesh.indices.size() == 6, "mixed-attribute OBJ should load two triangles");
    check(mesh.vertices.size() == 6, "OBJ loader should expand mixed-attribute vertices per triangle");
    check(mesh.normals.size() == mesh.vertices.size(), "OBJ loader should provide safe normal data for every expanded vertex");
    check(mesh.uvs.empty() || mesh.uvs.size() == mesh.vertices.size(), "OBJ loader UV data must be empty or complete");
}

void test_obj_loader_reads_mtl_diffuse_and_texture() {
    ObjMeshData mesh = load_model_mesh("models/obj/mtl_test.obj");
    check(mesh.materials.size() == 1, "OBJ MTL fixture should load one material");
    if (!mesh.materials.empty()) {
        check(near_vec(mesh.materials[0].albedo, Color(0.25, 0.5, 0.75)),
              "Kd should map to loaded material albedo");
        check(mesh.materials[0].base_color_texture == 0,
              "map_Kd should map to base color texture index");
        check(!mesh.textures.empty(), "map_Kd should create a loaded texture entry");
    }
}

const LoadedMaterialData* find_loaded_material(const ObjMeshData& mesh, const std::string& name) {
    for (const LoadedMaterialData& material : mesh.materials) {
        if (material.name == name) return &material;
    }
    return nullptr;
}

void test_obj_loader_reads_extended_mtl_fields() {
    ObjMeshData mesh = load_model_mesh("models/obj/mtl_extended.obj");
    check(mesh.materials.size() == 3, "extended OBJ MTL fixture should load three materials");

    const LoadedMaterialData* glow = find_loaded_material(mesh, "glow");
    check(glow != nullptr, "extended OBJ MTL should load emissive material");
    if (glow) {
        check(near_vec(glow->emissive, Color(2.0, 1.0, 0.5)),
              "OBJ MTL Ke should map to emissive color");
        check(glow->emissive_texture >= 0,
              "OBJ MTL map_Ke should map to emissive texture, including map options");
    }

    const LoadedMaterialData* glassy = find_loaded_material(mesh, "glassy");
    check(glassy != nullptr, "extended OBJ MTL should load transparent material");
    if (glassy) {
        check(glassy->alpha_blend, "OBJ MTL d less than one should enable alpha blend");
        check(near(glassy->alpha, 0.35), "OBJ MTL d should map to alpha");
        check(near(glassy->ior, 1.45), "OBJ MTL Ni should map to IOR");
    }

    const LoadedMaterialData* shiny = find_loaded_material(mesh, "shiny");
    check(shiny != nullptr, "extended OBJ MTL should load specular material");
    if (shiny) {
        check(shiny->metallic > 0.85, "OBJ MTL Ks should map strong specular materials toward metal");
        check(shiny->roughness < 0.1, "OBJ MTL Ns should map high shininess to low roughness");
    }
}

void test_obj_loader_reads_additional_texture_maps() {
    ObjMeshData mesh = load_model_mesh("models/obj/mtl_extended.obj");
    const LoadedMaterialData* glassy = find_loaded_material(mesh, "glassy");
    check(glassy != nullptr, "extended OBJ MTL should load glassy material");
    if (glassy) {
        check(glassy->alpha_texture >= 0, "OBJ MTL map_d should map to alpha texture");
    }

    const LoadedMaterialData* shiny = find_loaded_material(mesh, "shiny");
    check(shiny != nullptr, "extended OBJ MTL should load shiny material");
    if (shiny) {
        check(shiny->specular_texture >= 0, "OBJ MTL map_Ks should map to specular texture");
        check(shiny->shininess_texture >= 0, "OBJ MTL map_Ns should map to shininess texture");
    }
}

void test_obj_loader_ignores_missing_mtl_file() {
    try {
        ObjMeshData mesh = load_model_mesh("models/obj/mark.obj");
        check(!mesh.triangles.empty(), "OBJ with missing MTL should still load geometry");
    } catch (const std::exception& e) {
        check(false, std::string("OBJ with missing MTL should not fail: ") + e.what());
    }
}

void test_triangle_mesh_exposes_internal_acceleration() {
    TriangleMesh mesh;
    for (int i = 0; i < 8; ++i) {
        double x = static_cast<double>(i) * 2.0;
        mesh.vertices.push_back(Point3(x, 0, 0));
        mesh.vertices.push_back(Point3(x + 1, 0, 0));
        mesh.vertices.push_back(Point3(x, 1, 0));
        mesh.indices.push_back(i * 3 + 0);
        mesh.indices.push_back(i * 3 + 1);
        mesh.indices.push_back(i * 3 + 2);
        mesh.material_per_tri.push_back(nullptr);
    }

    Ray ray(Point3(0.25, 0.25, 1), Vec3(0, 0, -1));
    HitRecord rec;
    check(mesh.hit(ray, 0.001, infinity, rec), "ray should hit accelerated mesh");

    check_acceleration_node_count(mesh);
}

void test_pbr_exposes_brdf_and_pdf_for_direct_lighting() {
    auto albedo = std::make_shared<SolidColorTexture>(Color(0.8, 0.7, 0.6));
    PBR pbr(albedo, 0.0, 0.45);

    HitRecord rec;
    rec.p = Point3(0, 0, 0);
    rec.normal = Vec3(0, 1, 0);
    rec.tangent = Vec3(1, 0, 0);
    rec.has_tangent = true;
    rec.u = 0.5;
    rec.v = 0.5;

    Ray incoming(Point3(0, 1, 1), Vec3(0, -1, -1).normalized());
    Ray outgoing(rec.p, Vec3(0, 1, 1).normalized());

    Color brdf = pbr.f(incoming, outgoing, rec);
    double pdf = pbr.pdf(incoming, outgoing, rec);

    check(!pbr.is_specular(), "PBR should use the non-delta BRDF path for direct light and MIS");
    check(pdf > 0 && std::isfinite(pdf), "PBR pdf should be positive for a valid outgoing direction");
    check(finite_vec(brdf) && positive_color(brdf), "PBR BRDF should be finite and positive for a lit direction");
}

void test_lambertian_scatter_matches_cosine_pdf_contract() {
    set_random_seed(9001);
    Lambertian lambert(Color(0.6, 0.7, 0.8));

    HitRecord rec;
    rec.p = Point3(0, 0, 0);
    rec.normal = Vec3(0, 1, 0);
    rec.u = 0.25;
    rec.v = 0.75;

    Ray incoming(Point3(0, 1, 0), Vec3(0, -1, 0));
    for (int i = 0; i < 32; i++) {
        Color attenuation, emission;
        Ray scattered;
        bool did_scatter = lambert.scatter(incoming, rec, attenuation, scattered, emission);
        check(did_scatter, "Lambertian scatter should produce an outgoing ray");
        check(near(scattered.direction.length(), 1.0, 1e-9),
              "Lambertian scatter should return a unit direction for cosine PDF evaluation");
        double cos_theta = dot(rec.normal, scattered.direction);
        check(cos_theta > 0.0, "Lambertian scatter should stay above the geometric surface");
        check(near(lambert.pdf(incoming, scattered, rec), cos_theta / pi, 1e-9),
              "Lambertian pdf should match the cosine-weighted scatter direction");
        check(near_vec(attenuation, Color(0.6, 0.7, 0.8), 1e-12),
              "Lambertian scatter should preserve albedo attenuation");
        check(near_vec(emission, Color(0, 0, 0), 1e-12),
              "Lambertian scatter should not emit light");
    }
}

void test_display_color_exposure_and_tone_mapping() {
    ImageOutputOptions opts;
    opts.exposure = 2.0;
    opts.tone_map = ToneMapMode::Reinhard;
    Color out = to_display_color(Color(1.0, 0.5, 0.0), 1, opts);

    check(out.x > out.y, "tone mapped red channel should remain brighter than green");
    check(out.x < 1.0 && out.y < 1.0, "tone mapped display color should stay below one");
    check(out.z == 0.0, "zero input channel should stay zero");
}

void test_output_format_detection() {
    check(output_format_for_path("image.ppm") == ImageOutputFormat::PPM,
          "ppm extension should select PPM output");
    check(output_format_for_path("image.png") == ImageOutputFormat::PNG,
          "png extension should select PNG output");
    check(output_format_for_path("image.unknown") == ImageOutputFormat::PPM,
          "unknown extension should preserve old PPM behavior");
}

void test_environment_solid_and_gradient_backgrounds() {
    {
        std::ofstream out("/tmp/rt_environment_solid.json");
        out << "{"
            << "\"image\":{\"width\":16,\"height\":8},"
            << "\"environment\":{\"type\":\"solid\",\"color\":[0.2,0.3,0.4],\"intensity\":2.0},"
            << "\"objects\":[]"
            << "}";
    }
    Scene solid_scene;
    load_scene("/tmp/rt_environment_solid.json", solid_scene);
    check(solid_scene.environment.type == EnvironmentType::Solid,
          "environment.type solid should select solid background");
    check(near_vec(scene_background(solid_scene, Ray(Point3(0, 0, 0), Vec3(0, 1, 0))),
                   Color(0.4, 0.6, 0.8)),
          "solid environment should return color multiplied by intensity");

    {
        std::ofstream out("/tmp/rt_environment_gradient.json");
        out << "{"
            << "\"image\":{\"width\":16,\"height\":8},"
            << "\"environment\":{\"type\":\"gradient\",\"top\":[0.0,0.0,1.0],\"bottom\":[1.0,1.0,1.0]},"
            << "\"objects\":[]"
            << "}";
    }
    Scene gradient_scene;
    load_scene("/tmp/rt_environment_gradient.json", gradient_scene);
    Color up = scene_background(gradient_scene, Ray(Point3(0, 0, 0), Vec3(0, 1, 0)));
    Color down = scene_background(gradient_scene, Ray(Point3(0, 0, 0), Vec3(0, -1, 0)));
    check(down.x > up.x && down.y > up.y,
          "gradient environment should vary with ray direction");
}

void test_render_scene_reports_partial_updates() {
    {
        std::ofstream out("/tmp/rt_partial_updates.json");
        out << "{"
            << "\"image\":{\"width\":8,\"height\":4,\"samples\":1,\"max_depth\":2},"
            << "\"environment\":{\"type\":\"solid\",\"color\":[0.1,0.2,0.3]},"
            << "\"objects\":[]"
            << "}";
    }

    Scene scene;
    load_scene("/tmp/rt_partial_updates.json", scene);

    RenderOptions options;
    options.threads = 1;
    options.partial_update_interval = 2;

    int partial_count = 0;
    int status_count = 0;
    long long last_pixels_done = 0;
    double last_progress = 0.0;
    RenderCallbacks callbacks;
    callbacks.status = [&](const RenderProgressInfo& info) {
        status_count += 1;
        last_pixels_done = info.pixels_done;
        check(info.schedule == RenderSchedule::Rows, "row schedule status should identify row scheduling");
        check(info.pixels_total == static_cast<long long>(scene.width) * static_cast<long long>(scene.height),
              "row schedule status should report total pixels");
        check(info.elapsed_seconds >= 0.0, "row schedule status should report elapsed seconds");
    };
    callbacks.partial = [&](const RenderOutput& partial, double progress) {
        partial_count += 1;
        last_progress = progress;
        check(partial.width == scene.width && partial.height == scene.height,
              "partial render output should preserve image dimensions");
        check(partial.pixels.size() == static_cast<size_t>(scene.width) * static_cast<size_t>(scene.height),
              "partial render output should preserve pixel buffer size");
        check(progress > 0.0 && progress <= 1.0,
              "partial render progress should be normalized");
    };

    RenderOutput output = render_scene(scene, options, callbacks);
    check(!output.cancelled, "partial update test render should complete");
    check(status_count > 0, "render_scene should emit row schedule status updates");
    check(last_pixels_done == static_cast<long long>(scene.width) * static_cast<long long>(scene.height),
          "final row schedule status should report all pixels complete");
    check(partial_count >= 2, "render_scene should emit partial updates at configured row intervals");
    check(status_count >= partial_count, "row schedule should emit status for each partial update");
    check(near(last_progress, 1.0), "final partial update should report full progress");
}

void test_render_scene_sample_pass_schedule_reports_accumulated_samples() {
    {
        std::ofstream out("/tmp/rt_sample_pass_updates.json");
        out << "{"
            << "\"image\":{\"width\":4,\"height\":3,\"samples\":4,\"max_depth\":2},"
            << "\"environment\":{\"type\":\"solid\",\"color\":[0.2,0.3,0.4]},"
            << "\"objects\":[]"
            << "}";
    }

    Scene scene;
    load_scene("/tmp/rt_sample_pass_updates.json", scene);

    RenderOptions options;
    options.threads = 1;
    options.partial_update_interval = 2;
    options.sample_pass_batch = 2;
    options.schedule = RenderSchedule::SamplePasses;

    int partial_count = 0;
    int status_count = 0;
    int last_status_samples = 0;
    int last_partial_samples = 0;
    RenderCallbacks callbacks;
    callbacks.status = [&](const RenderProgressInfo& info) {
        status_count += 1;
        last_status_samples = info.samples_done;
        check(info.schedule == RenderSchedule::SamplePasses,
              "sample-pass status should identify sample-pass scheduling");
        check(info.samples_total == scene.samples,
              "sample-pass status should report total samples");
        check(info.elapsed_seconds >= 0.0,
              "sample-pass status should report elapsed seconds");
    };
    callbacks.partial = [&](const RenderOutput& partial, double progress) {
        partial_count += 1;
        last_partial_samples = partial.samples;
        check(partial.width == scene.width && partial.height == scene.height,
              "sample-pass partial output should preserve image dimensions");
        check(partial.samples == 2 || partial.samples == 4,
              "sample-pass partial output should report accumulated sample count");
        check(progress > 0.0 && progress <= 1.0,
              "sample-pass partial progress should be normalized");
    };

    RenderOutput output = render_scene(scene, options, callbacks);
    check(!output.cancelled, "sample-pass schedule render should complete");
    check(output.samples == scene.samples, "final sample-pass output should keep requested sample count");
    check(status_count > 0, "sample-pass schedule should emit status updates");
    check(last_status_samples == scene.samples, "final sample-pass status should report all samples complete");
    check(partial_count == 2, "sample-pass schedule should emit partial updates at sample intervals");
    check(status_count >= partial_count, "sample-pass schedule should emit status for each partial update");
    check(last_partial_samples == scene.samples, "final sample-pass partial should use final sample count");
}

void test_seeded_render_is_thread_count_independent() {
    {
        std::ofstream out("/tmp/rt_seed_thread_independent.json");
        out << "{"
            << "\"image\":{\"width\":8,\"height\":6,\"samples\":4,\"max_depth\":4,\"seed\":77},"
            << "\"camera\":{\"lookfrom\":[0,1,4],\"lookat\":[0,0,-1],\"vup\":[0,1,0],\"vfov\":40},"
            << "\"lighting\":{\"ambient\":[0,0,0]},"
            << "\"objects\":["
            << "{\"type\":\"sphere\",\"center\":[0,-100.5,-1],\"radius\":100,"
            << "\"material\":{\"type\":\"lambertian\",\"albedo\":[0.8,0.8,0.8]}},"
            << "{\"type\":\"sphere\",\"center\":[0,0,-1],\"radius\":0.5,"
            << "\"material\":{\"type\":\"lambertian\",\"albedo\":[0.5,0.6,0.7]}},"
            << "{\"type\":\"sphere\",\"center\":[0,1.5,-1],\"radius\":0.25,"
            << "\"material\":{\"type\":\"emissive\",\"emission\":[8,8,8]}}"
            << "]"
            << "}";
    }

    Scene single_thread_scene;
    Scene multi_thread_scene;
    load_scene("/tmp/rt_seed_thread_independent.json", single_thread_scene);
    load_scene("/tmp/rt_seed_thread_independent.json", multi_thread_scene);

    RenderOptions single_thread_options;
    single_thread_options.threads = 1;
    RenderOutput single_thread = render_scene(single_thread_scene, single_thread_options);

    RenderOptions multi_thread_options;
    multi_thread_options.threads = 4;
    RenderOutput multi_thread = render_scene(multi_thread_scene, multi_thread_options);

    check(single_thread.pixels.size() == multi_thread.pixels.size(),
          "seeded render should produce comparable pixel buffers");
    size_t count = std::min(single_thread.pixels.size(), multi_thread.pixels.size());
    for (size_t i = 0; i < count; i++) {
        check(near_vec(single_thread.pixels[i], multi_thread.pixels[i], 1e-12),
              "seeded render should not depend on render thread count");
    }
}

void test_extended_light_types_parse() {
    JsonValue rect;
    rect.type = JsonValue::Object;
    rect.objVal["type"] = string_value("rect");
    rect.objVal["position"] = array3(0, 3, 0);
    rect.objVal["u"] = array3(2, 0, 0);
    rect.objVal["v"] = array3(0, 0, 1);
    Light rect_light = parse_light(rect);
    check(rect_light.type == LightType::Rect, "rect light should parse as rectangular area light");
    check(near(rect_light.area(), 2.0), "rect light area should come from u cross v");
    check(!rect_light.visible_camera && rect_light.visible_specular,
          "area lights should default to camera-hidden and specular-visible");

    JsonValue sphere;
    sphere.type = JsonValue::Object;
    sphere.objVal["type"] = string_value("sphere");
    sphere.objVal["position"] = array3(1, 2, 3);
    sphere.objVal["radius"] = number(0.5);
    Light sphere_light = parse_light(sphere);
    check(sphere_light.type == LightType::Sphere, "sphere light should parse as spherical area light");
    check(near(sphere_light.radius, 0.5), "sphere light should store radius");

    JsonValue spot;
    spot.type = JsonValue::Object;
    spot.objVal["type"] = string_value("spot");
    spot.objVal["position"] = array3(0, 3, 0);
    spot.objVal["direction"] = array3(0, -1, 0);
    spot.objVal["angle"] = number(20.0);
    Light spot_light = parse_light(spot);
    check(spot_light.type == LightType::Spot, "spot light should parse as spot light");
    check(near(spot_light.angle, 20.0), "spot light should store cone angle");
}

void test_extended_light_sampling_outputs_radiance() {
    JsonValue rect;
    rect.type = JsonValue::Object;
    rect.objVal["type"] = string_value("rect");
    rect.objVal["position"] = array3(0, 2, 0);
    rect.objVal["direction"] = array3(0, -1, 0);
    rect.objVal["u"] = array3(2, 0, 0);
    rect.objVal["v"] = array3(0, 0, 2);
    rect.objVal["intensity"] = number(4.0);
    Light rect_light = parse_light(rect);
    LightSample rect_sample = sample_scene_light(rect_light, Point3(0, 0, 0), 0.5, 0.5);
    check(rect_sample.radiance.x > 0.0 && near(rect_sample.distance, 2.0),
          "rect light sampling should produce finite positive direct radiance");
    check(near(rect_sample.pdf, 1.0),
          "rect light sampling should report solid-angle PDF for MIS");

    JsonValue spot;
    spot.type = JsonValue::Object;
    spot.objVal["type"] = string_value("spot");
    spot.objVal["position"] = array3(0, 2, 0);
    spot.objVal["direction"] = array3(0, -1, 0);
    spot.objVal["angle"] = number(10.0);
    Light spot_light = parse_light(spot);
    check(sample_scene_light(spot_light, Point3(0, 0, 0)).radiance.x > 0.0,
          "spot light should illuminate points inside its cone");
    check(sample_scene_light(spot_light, Point3(2, 0, 0)).radiance.length_squared() == 0.0,
          "spot light should reject points outside its cone");
}

void test_analytic_area_light_visibility_for_camera_and_specular_rays() {
    {
        std::ofstream out("/tmp/rt_area_light_visibility.json");
        out << "{"
            << "\"image\":{\"width\":16,\"height\":16,\"samples\":1},"
            << "\"background\":{\"type\":\"solid\",\"color\":[0,0,0]},"
            << "\"lighting\":{\"ambient\":[0,0,0]},"
            << "\"camera\":{\"lookfrom\":[0,0,0],\"lookat\":[0,0,-1],\"vfov\":60},"
            << "\"lights\":[{\"type\":\"rect\",\"position\":[0,0,-2],\"direction\":[0,0,1],"
            << "\"u\":[2,0,0],\"v\":[0,2,0],\"color\":[1,0.5,0.25],\"intensity\":3}],"
            << "\"objects\":[]"
            << "}";
    }
    Scene hidden_scene;
    load_scene("/tmp/rt_area_light_visibility.json", hidden_scene);
    RenderOptions options;
    Ray ray(Point3(0, 0, 0), Vec3(0, 0, -1));
    Color camera_hit = ray_color(ray, hidden_scene, 4, options, infinity, false, RayPathType::Camera);
    check(near_vec(camera_hit, Color(0, 0, 0), 1e-9),
          "area light should be hidden from camera rays by default");
    Color specular_hit = ray_color(ray, hidden_scene, 4, options, infinity, false, RayPathType::Specular);
    check(near_vec(specular_hit, Color(3, 1.5, 0.75), 1e-9),
          "area light should be visible to specular rays by default");

    {
        std::ofstream out("/tmp/rt_area_light_camera_visible.json");
        out << "{"
            << "\"image\":{\"width\":16,\"height\":16,\"samples\":1},"
            << "\"background\":{\"type\":\"solid\",\"color\":[0,0,0]},"
            << "\"camera\":{\"lookfrom\":[0,0,0],\"lookat\":[0,0,-1],\"vfov\":60},"
            << "\"lights\":[{\"type\":\"rect\",\"position\":[0,0,-2],\"direction\":[0,0,1],"
            << "\"u\":[2,0,0],\"v\":[0,2,0],\"intensity\":2,\"visible_camera\":true}],"
            << "\"objects\":[]"
            << "}";
    }
    Scene visible_scene;
    load_scene("/tmp/rt_area_light_camera_visible.json", visible_scene);
    Color visible_camera_hit = ray_color(ray, visible_scene, 4, options, infinity, false, RayPathType::Camera);
    check(near_vec(visible_camera_hit, Color(2, 2, 2), 1e-9),
          "visible_camera=true should make analytic area lights visible to camera rays");
}

void test_camera_focal_length_orbit_and_framing_fields() {
    {
        std::ofstream out("/tmp/rt_camera_focal.json");
        out << "{"
            << "\"image\":{\"width\":100,\"height\":100},"
            << "\"camera\":{\"lookfrom\":[0,0,5],\"lookat\":[0,0,0],\"focal_length\":50,\"sensor_height\":50},"
            << "\"objects\":[]"
            << "}";
    }
    Scene focal_scene;
    load_scene("/tmp/rt_camera_focal.json", focal_scene);
    check(near(focal_scene.camera->vfov_degrees(), 53.13010235415598, 1e-6),
          "camera focal_length/sensor_height should derive vertical FOV");

    {
        std::ofstream out("/tmp/rt_camera_frame.json");
        out << "{"
            << "\"image\":{\"width\":100,\"height\":100},"
            << "\"camera\":{"
            << "\"lookfrom\":[0,0,0],\"lookat\":[0,0,-1],\"vfov\":90,"
            << "\"frame\":{\"lower_left\":[-2,-1,-1],\"horizontal\":[4,0,0],\"vertical\":[0,2,0]}"
            << "},"
            << "\"objects\":[]"
            << "}";
    }
    Scene frame_scene;
    load_scene("/tmp/rt_camera_frame.json", frame_scene);
    Ray right_edge = frame_scene.camera->get_ray(1.0, 0.5);
    check(near_vec(right_edge.direction, Vec3(2, 0, -1), 1e-9),
          "camera frame should override centered vfov projection for Blender framing");

    {
        std::ofstream out("/tmp/rt_camera_orbit.json");
        out << "{"
            << "\"image\":{\"width\":100,\"height\":100},"
            << "\"camera\":{\"orbit\":{\"target\":[0,1,0],\"yaw\":0,\"pitch\":0,\"distance\":4}},"
            << "\"objects\":[]"
            << "}";
    }
    Scene orbit_scene;
    load_scene("/tmp/rt_camera_orbit.json", orbit_scene);
    check(near_vec(orbit_scene.camera->lookfrom(), Point3(0, 1, 4), 1e-6),
          "camera orbit should derive lookfrom from target/yaw/pitch/distance");

    {
        std::ofstream out("/tmp/rt_camera_auto_view.json");
        out << "{"
            << "\"image\":{\"width\":100,\"height\":100},"
            << "\"camera\":{\"auto\":true,\"view\":\"front\",\"margin\":2.0,\"target_offset\":[0,1,0]},"
            << "\"objects\":[{\"type\":\"sphere\",\"center\":[0,0,0],\"radius\":1,\"material\":{\"type\":\"lambertian\",\"albedo\":[1,1,1]}}]"
            << "}";
    }
    Scene auto_scene;
    load_scene("/tmp/rt_camera_auto_view.json", auto_scene);
    check(auto_scene.camera->lookfrom().z > 2.0 && auto_scene.camera->lookat().y > 0.5,
          "auto camera should honor view, margin, and target_offset");
}

void test_scene_preset_and_render_block_are_accepted() {
    std::ofstream out("/tmp/rt_scene_preset.json");
    out << "{"
        << "\"render\":{\"width\":48,\"height\":24,\"samples\":3,\"max_depth\":7,\"firefly_clamp\":4.0},"
        << "\"scene\":{\"preset\":\"studio_softbox\"},"
        << "\"objects\":[{\"type\":\"sphere\",\"center\":[0,0,0],\"radius\":1,\"material\":{\"type\":\"lambertian\",\"albedo\":[1,1,1]}}]"
        << "}";
    out.close();

    Scene scene;
    load_scene("/tmp/rt_scene_preset.json", scene);
    check(scene.width == 48 && scene.height == 24 && scene.samples == 3 && scene.max_depth == 7,
          "render block should be accepted as an image alias");
    check(near(scene.firefly_clamp, 4.0), "render.firefly_clamp should be parsed");
    bool has_rect = false;
    for (const Light& light : scene.lights) has_rect = has_rect || light.type == LightType::Rect;
    check(has_rect, "studio_softbox preset should add a rectangular softbox light");
}

void test_firefly_clamp_preserves_hue_by_scaling() {
    check(near_vec(clamp_radiance(Color(10, 4, 2), 5.0), Color(5, 2, 1)),
          "firefly clamp should scale radiance instead of clipping channels independently");
    check(near_vec(clamp_radiance(Color(1, 2, 3), infinity), Color(1, 2, 3)),
          "infinite firefly clamp should leave radiance unchanged");
}

void test_loaded_material_emissive_routes_to_emissive() {
    Scene scene;
    ObjMeshData mesh;
    LoadedMaterialData data;
    data.emissive = Color(2, 3, 4);

    Material* mat = add_loaded_material(mesh, data, scene);
    check(mat->is_emissive(), "GLB emissiveFactor should route to Emissive material");
}

void test_loaded_glb_pbr_uses_metallic_roughness_and_normal_textures() {
    Scene scene;
    ObjMeshData mesh;
    LoadedTextureData mr_texture;
    mr_texture.path = "textures/mtl_test.ppm";
    mesh.textures.push_back(mr_texture);
    LoadedTextureData normal_texture;
    normal_texture.path = "textures/mtl_test.ppm";
    mesh.textures.push_back(normal_texture);

    LoadedMaterialData data;
    data.use_pbr = true;
    data.albedo = Color(0.8, 0.7, 0.6);
    data.metallic = 0.1;
    data.roughness = 0.2;
    data.metallic_roughness_texture = 0;
    data.normal_texture = 1;

    Material* mat = add_loaded_material(mesh, data, scene);
    auto* pbr = dynamic_cast<PBR*>(mat);
    check(pbr != nullptr, "GLB PBR material should route to PBR material");
    if (pbr) {
        HitRecord rec;
        rec.u = 0.0;
        rec.v = 0.0;
        rec.p = Point3(0, 0, 0);
        check(near(pbr->roughness->value(rec.u, rec.v, rec.p).x, 128.0 / 255.0, 1e-6),
              "GLB metallicRoughnessTexture green channel should drive roughness");
        check(near(pbr->metallic->value(rec.u, rec.v, rec.p).x, 192.0 / 255.0, 1e-6),
              "GLB metallicRoughnessTexture blue channel should drive metallic");
        check(pbr->has_normal_map, "GLB normalTexture should enable PBR normal map");
    }
}

void test_loaded_glb_pbr_emissive_texture_preserves_surface_shading() {
    Scene scene;
    ObjMeshData mesh;
    LoadedTextureData texture;
    texture.path = "textures/mtl_test.ppm";
    mesh.textures.push_back(texture);

    LoadedMaterialData data;
    data.use_pbr = true;
    data.albedo = Color(0.8, 0.7, 0.6);
    data.metallic = 0.0;
    data.roughness = 0.5;
    data.emissive = Color(2, 3, 4);
    data.emissive_texture = 0;

    Material* mat = add_loaded_material(mesh, data, scene);
    auto* pbr = dynamic_cast<PBR*>(mat);
    check(pbr != nullptr, "GLB PBR material with emissiveTexture should preserve PBR shading");
    check(!mat->is_emissive(), "GLB PBR material with emissiveTexture should not become a pure area light");
    if (pbr) {
        HitRecord rec;
        rec.u = 0.0;
        rec.v = 0.0;
        rec.p = Point3(0, 0, 0);
        check(near_vec(pbr->base_color(rec), Color(0.8, 0.7, 0.6), 1e-6),
              "GLB PBR material should keep its base color while adding emission");
        check(near_vec(pbr->emitted(rec),
                       Color(2 * 64.0 / 255.0, 3 * 128.0 / 255.0, 4 * 192.0 / 255.0), 1e-6),
              "GLB emissiveTexture should modulate emissiveFactor");
    }
}

void test_loaded_glb_volume_attenuation_reaches_dielectric() {
    Scene scene;
    ObjMeshData mesh;
    LoadedMaterialData data;
    data.transmission = 1.0;
    data.albedo = Color(1, 1, 1);
    data.ior = 1.4;
    data.roughness = 0.42;
    data.attenuation_color = Color(0.25, 0.5, 1.0);
    data.attenuation_distance = 2.0;

    Material* mat = add_loaded_material(mesh, data, scene);
    auto* dielectric = dynamic_cast<Dielectric*>(mat);
    check(dielectric != nullptr, "GLB transmission material should route to Dielectric");
    if (dielectric) {
        HitRecord rec;
        rec.p = Point3(0, 0, 0);
        rec.normal = Vec3(0, 1, 0);
        rec.front_face = false;
        rec.t = 2.0;
        Color attenuation, emission;
        Ray scattered;
        // With medium tracking, Beer-Lambert is driven by the ray's current
        // medium, not by rec.front_face.  Simulate a ray already inside the
        // dielectric volume by tagging the incoming ray with the medium.
        Ray inside(Point3(0, 0, 0), Vec3(0, 1, 0));
        inside.medium_color = Color(0.25, 0.5, 1.0);
        inside.medium_attenuation_distance = 2.0;
        dielectric->scatter(inside, rec, attenuation, scattered, emission);
        check(near_vec(attenuation, Color(0.25, 0.5, 1.0), 1e-6),
              "GLB KHR_materials_volume attenuation should affect dielectric attenuation inside the medium");
        check(near(dielectric->roughness, 0.42),
              "GLB transmission material should preserve roughness for rough glass");
    }
}

void test_glb_transparency_texture_and_volume_thickness_parse() {
    JsonValue material;
    material.type = JsonValue::Object;
    JsonValue extensions;
    extensions.type = JsonValue::Object;

    JsonValue transmission;
    transmission.type = JsonValue::Object;
    transmission.objVal["transmissionFactor"] = number(0.25);
    JsonValue transmission_texture;
    transmission_texture.type = JsonValue::Object;
    transmission_texture.objVal["index"] = number(2);
    transmission.objVal["transmissionTexture"] = transmission_texture;
    extensions.objVal["KHR_materials_transmission"] = transmission;

    JsonValue volume;
    volume.type = JsonValue::Object;
    volume.objVal["thicknessFactor"] = number(3.0);
    JsonValue thickness_texture;
    thickness_texture.type = JsonValue::Object;
    thickness_texture.objVal["index"] = number(4);
    volume.objVal["thicknessTexture"] = thickness_texture;
    extensions.objVal["KHR_materials_volume"] = volume;

    material.objVal["extensions"] = extensions;
    material.objVal["alphaMode"] = string_value("MASK");
    material.objVal["alphaCutoff"] = number(0.4);

    LoadedMaterialData data = glb_material_data(material);
    check(near(data.transmission, 0.25), "GLB transmissionFactor should parse");
    check(data.transmission_texture == 2, "GLB transmissionTexture should parse");
    check(near(data.thickness_factor, 3.0), "GLB KHR_materials_volume thicknessFactor should parse");
    check(data.thickness_texture == 4, "GLB KHR_materials_volume thicknessTexture should parse");
    check(data.alpha_mask && near(data.alpha_cutoff, 0.4),
          "GLB alphaMode MASK and alphaCutoff should parse");
}

void test_json_dielectric_accepts_volume_attenuation() {
    Scene scene;
    JsonValue material;
    material.type = JsonValue::Object;
    material.objVal["type"] = string_value("dielectric");
    material.objVal["ior"] = number(1.333);
    material.objVal["albedo"] = array3(1.0, 1.0, 1.0);
    material.objVal["roughness"] = number(0.35);
    material.objVal["attenuation_color"] = array3(0.4, 0.7, 1.0);
    material.objVal["attenuation_distance"] = number(3.0);

    Material* mat = parse_material(material, scene);
    auto* dielectric = dynamic_cast<Dielectric*>(mat);
    check(dielectric != nullptr, "JSON dielectric material should parse attenuation fields");
    if (dielectric) {
        HitRecord rec;
        rec.p = Point3(0, 0, 0);
        rec.normal = Vec3(0, 1, 0);
        rec.front_face = false;
        rec.t = 3.0;
        Color attenuation, emission;
        Ray scattered;
        // Medium tracking: the incoming ray carries the dielectric's volume
        // parameters; Beer-Lambert is evaluated against rec.t.
        Ray inside(Point3(0, 0, 0), Vec3(0, 1, 0));
        inside.medium_color = Color(0.4, 0.7, 1.0);
        inside.medium_attenuation_distance = 3.0;
        dielectric->scatter(inside, rec, attenuation, scattered, emission);
        check(near_vec(attenuation, Color(0.4, 0.7, 1.0), 1e-6),
              "JSON dielectric attenuation should tint rays exiting the medium");
        check(near(dielectric->roughness, 0.35),
              "JSON dielectric roughness should be parsed for rough transmission");
    }
}

void test_glb_texture_transform_parses_scale_and_offset() {
    JsonValue material;
    material.type = JsonValue::Object;
    JsonValue pbr;
    pbr.type = JsonValue::Object;
    JsonValue base_color_texture;
    base_color_texture.type = JsonValue::Object;
    base_color_texture.objVal["index"] = number(0);
    JsonValue tex_exts;
    tex_exts.type = JsonValue::Object;
    JsonValue transform;
    transform.type = JsonValue::Object;
    transform.objVal["scale"] = array3(2.0, 0.5, 0.0);
    transform.objVal["offset"] = array3(0.1, 0.2, 0.0);
    tex_exts.objVal["KHR_texture_transform"] = transform;
    base_color_texture.objVal["extensions"] = tex_exts;
    pbr.objVal["baseColorTexture"] = base_color_texture;
    material.objVal["pbrMetallicRoughness"] = pbr;

    LoadedMaterialData data = glb_material_data(material);
    check(data.base_color_transform.active, "KHR_texture_transform should activate");
    check(near(data.base_color_transform.scale.x, 2.0) && near(data.base_color_transform.scale.y, 0.5),
          "KHR_texture_transform scale should parse");
    check(near(data.base_color_transform.offset.x, 0.1) && near(data.base_color_transform.offset.y, 0.2),
          "KHR_texture_transform offset should parse");
}

void test_dielectric_partial_transmission_factor_stores() {
    Dielectric d(1.5, Color(1.0, 1.0, 1.0));
    d.transmission = 0.6;
    check(near(d.transmission, 0.6), "Dielectric partial transmission factor should store");
    check(d.is_transparent() && d.is_specular(), "Dielectric should report transparent and specular");
}

void test_material_alpha_mask_interface() {
    Lambertian lamb(Color(0.5, 0.5, 0.5));
    check(!lamb.is_alpha_masked(), "Lambertian default should not be alpha masked");
    lamb.alpha_masked = true;
    lamb.cutoff = 0.33;
    check(lamb.is_alpha_masked() && near(lamb.alpha_cutoff(), 0.33),
          "Lambertian alpha mask fields should expose via interface");

    PBR pbr(std::make_shared<SolidColorTexture>(Color(0.5, 0.5, 0.5)), 0.5, 0.5);
    check(!pbr.is_alpha_masked(), "PBR default should not be alpha masked");
    pbr.alpha_masked = true;
    pbr.cutoff = 0.5;
    check(pbr.is_alpha_masked(), "PBR alpha mask should expose via interface");

    Dielectric d(1.5);
    d.double_sided = true;
    check(d.is_double_sided(), "Dielectric double-sided should expose via interface");
}

void test_transformed_texture_applies_uv_scale_offset() {
    auto solid = std::make_shared<SolidColorTexture>(Color(0.4, 0.6, 0.8));
    TransformedTexture tex(solid, Vec2(2.0, 1.0), Vec2(0.5, 0.0));
    Color c = tex.value(0.25, 0.5, Point3(0, 0, 0));
    // u' = 0.25 * 2 + 0.5 = 1.0; v' = 0.5 * 1 + 0 = 0.5; solid ignores UV so color unchanged
    check(near_vec(c, Color(0.4, 0.6, 0.8)), "TransformedTexture should pass through to wrapped texture");

    auto checker = std::make_shared<CheckerTexture>(Color(1, 1, 1), Color(0, 0, 0), 1.0);
    TransformedTexture rotated(checker, Vec2(1.0, 1.0), Vec2(0.5, 0.0), 90.0);
    check(near_vec(rotated.value(0.25, 0.25, Point3()), Color(1, 1, 1), 1e-6),
          "TransformedTexture should apply offset after rotation");
}

void test_checker_texture_json_scale_and_offset() {
    JsonValue checker = parse_json(
        "{\"type\":\"checker\",\"color1\":[1,1,1],\"color2\":[0,0,0],"
        "\"scale\":2,\"uv_scale\":[1,1],\"uv_offset\":[0.25,0]}");
    auto tex = parse_texture_json(checker, std::filesystem::current_path());
    check(near_vec(tex->value(0.0, 0.0, Point3()), Color(1, 1, 1)),
          "checker texture should sample color1 in the first transformed cell");
    check(near_vec(tex->value(0.25, 0.0, Point3()), Color(0, 0, 0)),
          "checker texture uv_offset and scale should move sampling into the adjacent cell");

    JsonValue rotated_checker = parse_json(
        "{\"type\":\"checker\",\"color1\":[1,1,1],\"color2\":[0,0,0],"
        "\"scale\":1,\"uv_scale\":[1,1],\"uv_offset\":[0.5,0],\"uv_rotation\":90}");
    auto rotated = parse_texture_json(rotated_checker, std::filesystem::current_path());
    check(near_vec(rotated->value(0.25, 0.25, Point3()), Color(1, 1, 1), 1e-6),
          "checker texture should apply uv_offset after uv_rotation");

    std::ofstream out("/tmp/rt_checker_material.json");
    out << "{"
        << "\"image\":{\"width\":8,\"height\":8,\"samples\":1},"
        << "\"camera\":{\"lookfrom\":[0,0,2],\"lookat\":[0,0,0],\"vfov\":60},"
        << "\"objects\":[{\"type\":\"sphere\",\"center\":[0,0,0],\"radius\":1,"
        << "\"material\":{\"type\":\"pbr\",\"albedo\":{\"type\":\"checker\","
        << "\"color1\":[0.9,0.9,0.9],\"color2\":[0.1,0.1,0.1],\"scale\":4},"
        << "\"metallic\":0,\"roughness\":0.5}}]"
        << "}";
    out.close();
    Scene scene;
    load_scene("/tmp/rt_checker_material.json", scene);
    HitRecord rec;
    rec.u = 0.1;
    rec.v = 0.1;
    rec.p = Point3();
    check(near_vec(scene.materials[0]->base_color(rec), Color(0.9, 0.9, 0.9), 1e-6),
          "PBR albedo should accept a checker texture object");
}

void test_image_texture_json_accepts_inline_base64_payload() {
    JsonValue image = parse_json(
        "{\"type\":\"image\",\"mime_type\":\"image/png\","
        "\"data_base64\":\"iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mP8z8BQDwAEhQGAhKmMIQAAAABJRU5ErkJggg==\"}");
    auto tex = parse_texture_json(image, std::filesystem::current_path());
    Color c = tex->value(0.0, 0.0, Point3());
    check(finite_vec(c) && c.x >= 0.0 && c.x <= 1.0 && c.y >= 0.0 && c.y <= 1.0 && c.z >= 0.0 && c.z <= 1.0,
          "image texture JSON with data_base64 should decode and sample a finite color");
}

void test_image_texture_json_accepts_uv_mapping_fields() {
    std::string checker_path = (std::filesystem::current_path() / "textures/checkerboard.png").string();
    JsonValue image = parse_json(
        "{\"type\":\"image\",\"path\":\"textures/checkerboard.png\","
        "\"uv_scale\":[5,5],\"uv_offset\":[0,0],\"uv_rotation\":0}");
    auto tex = parse_texture_json(image, std::filesystem::current_path());
    Color c = tex->value(0.1, 0.1, Point3());
    check(finite_vec(c) && c.x >= 0.0 && c.x <= 1.0 && c.y >= 0.0 && c.y <= 1.0 && c.z >= 0.0 && c.z <= 1.0,
          "image texture JSON with UV mapping fields should load and sample a finite color");

    std::ofstream out("/tmp/rt_image_texture_mapping.json");
    out << "{"
        << "\"image\":{\"width\":8,\"height\":8,\"samples\":1},"
        << "\"camera\":{\"lookfrom\":[0,0,2],\"lookat\":[0,0,0],\"vfov\":60},"
        << "\"objects\":[{\"type\":\"sphere\",\"center\":[0,0,0],\"radius\":1,"
        << "\"material\":{\"type\":\"pbr\",\"albedo\":{\"type\":\"image\","
        << "\"path\":\"" << checker_path << "\",\"uv_scale\":[5,5]},"
        << "\"metallic\":0,\"roughness\":0.5}}]"
        << "}";
    out.close();
    Scene scene;
    load_scene("/tmp/rt_image_texture_mapping.json", scene);
    HitRecord rec;
    rec.u = 0.12;
    rec.v = 0.34;
    rec.p = Point3();
    Color base = scene.materials[0]->base_color(rec);
    check(finite_vec(base), "PBR albedo should accept an image texture object with UV mapping");
}

void test_image_texture_linear_interpolation() {
    ImageTexture tex;
    tex.width = 2;
    tex.height = 1;
    tex.pixels = {Color(0, 0, 0), Color(1, 1, 1)};
    tex.interpolation = ImageTexture::Interpolation::Linear;
    Color middle = tex.value(0.5, 0.5, Point3());
    check(near_vec(middle, Color(0.5, 0.5, 0.5), 1e-6),
          "ImageTexture linear interpolation should blend neighboring texels");
}

void test_image_texture_srgb_decode() {
    ImageTexture tex;
    tex.width = 1;
    tex.height = 1;
    tex.pixels = {Color(0.5, 0.5, 0.5)};
    tex.decode_srgb = true;
    Color linear = tex.value(0.5, 0.5, Point3());
    check(near(linear.x, 0.21404114048223255, 1e-6) &&
          near(linear.y, 0.21404114048223255, 1e-6) &&
          near(linear.z, 0.21404114048223255, 1e-6),
          "ImageTexture should decode sRGB texels to linear when requested");
}

void test_image_texture_extension_modes() {
    ImageTexture tex;
    tex.width = 2;
    tex.height = 1;
    tex.pixels = {Color(0, 0, 0), Color(1, 1, 1)};

    tex.extension = ImageTexture::Extension::Extend;
    check(near_vec(tex.value(1.25, 0.5, Point3()), Color(1, 1, 1), 1e-6),
          "ImageTexture extend mode should clamp UVs to the texture edge");

    tex.extension = ImageTexture::Extension::Clip;
    check(near_vec(tex.value(1.25, 0.5, Point3()), Color(0, 0, 0), 1e-6),
          "ImageTexture clip mode should return black outside 0..1 UVs");

    tex.extension = ImageTexture::Extension::Mirror;
    check(near_vec(tex.value(1.25, 0.5, Point3()), Color(1, 1, 1), 1e-6) &&
          near_vec(tex.value(-0.25, 0.5, Point3()), Color(0, 0, 0), 1e-6),
          "ImageTexture mirror mode should mirror-repeat UVs");

    tex.interpolation = ImageTexture::Interpolation::Linear;
    tex.extension = ImageTexture::Extension::Extend;
    check(near_vec(tex.value(0.0, 0.5, Point3()), Color(0, 0, 0), 1e-6) &&
          near_vec(tex.value(1.0, 0.5, Point3()), Color(1, 1, 1), 1e-6),
          "ImageTexture linear extend mode should clamp edge samples instead of wrapping");

    tex.extension = ImageTexture::Extension::Clip;
    check(near_vec(tex.value(0.0, 0.5, Point3()), Color(0, 0, 0), 1e-6) &&
          near_vec(tex.value(1.0, 0.5, Point3()), Color(1, 1, 1), 1e-6),
          "ImageTexture linear clip mode should clamp in-range edge samples instead of wrapping");
}

void test_color_ramp_texture_json_wraps_source_texture() {
    JsonValue ramp = parse_json(
        "{\"type\":\"color_ramp\",\"source\":{\"type\":\"checker\",\"color1\":[0,0,0],"
        "\"color2\":[1,1,1],\"scale\":2},\"stops\":["
        "{\"position\":0,\"color\":[1,0,0]},"
        "{\"position\":1,\"color\":[0,0,1]}]}");
    auto tex = parse_texture_json(ramp, std::filesystem::current_path());
    check(near_vec(tex->value(0.1, 0.1, Point3()), Color(1, 0, 0), 1e-6),
          "ColorRampTexture JSON should map dark source values to the first ramp color");
    check(near_vec(tex->value(0.6, 0.1, Point3()), Color(0, 0, 1), 1e-6),
          "ColorRampTexture JSON should map bright source values to the last ramp color");
}

void test_color_ramp_texture_interpolation_modes() {
    JsonValue constant = parse_json(
        "{\"type\":\"color_ramp\",\"interpolation\":\"constant\","
        "\"source\":{\"type\":\"solid\",\"color\":[0.5,0.5,0.5]},\"stops\":["
        "{\"position\":0,\"color\":[1,0,0]},"
        "{\"position\":1,\"color\":[0,0,1]}]}");
    auto constant_tex = parse_texture_json(constant, std::filesystem::current_path());
    check(near_vec(constant_tex->value(0.0, 0.0, Point3()), Color(1, 0, 0), 1e-6),
          "ColorRampTexture constant interpolation should hold the previous stop color");

    JsonValue ease = parse_json(
        "{\"type\":\"color_ramp\",\"interpolation\":\"ease\","
        "\"source\":{\"type\":\"solid\",\"color\":[0.25,0.25,0.25]},\"stops\":["
        "{\"position\":0,\"color\":[0,0,0]},"
        "{\"position\":1,\"color\":[1,1,1]}]}");
    auto ease_tex = parse_texture_json(ease, std::filesystem::current_path());
    check(near_vec(ease_tex->value(0.0, 0.0, Point3()), Color(0.15625, 0.15625, 0.15625), 1e-6),
          "ColorRampTexture ease interpolation should smooth the ramp factor");
}

void test_math_and_mix_texture_json_nodes() {
    JsonValue math = parse_json(
        "{\"type\":\"math\",\"operation\":\"multiply\","
        "\"a\":{\"type\":\"checker\",\"color1\":[0,0,0],\"color2\":[1,1,1],\"scale\":2},"
        "\"b\":{\"type\":\"solid\",\"color\":[0.5,0.5,0.5]}}");
    auto math_tex = parse_texture_json(math, std::filesystem::current_path());
    check(near_vec(math_tex->value(0.1, 0.1, Point3()), Color(0, 0, 0), 1e-6) &&
          near_vec(math_tex->value(0.6, 0.1, Point3()), Color(0.5, 0.5, 0.5), 1e-6),
          "MathTexture JSON should preserve dynamic scalar texture inputs");

    JsonValue mix = parse_json(
        "{\"type\":\"mix\",\"factor\":{\"type\":\"solid\",\"color\":[0.25,0.25,0.25]},"
        "\"color1\":{\"type\":\"solid\",\"color\":[1,0,0]},"
        "\"color2\":{\"type\":\"solid\",\"color\":[0,0,1]}}");
    auto mix_tex = parse_texture_json(mix, std::filesystem::current_path());
    check(near_vec(mix_tex->value(0.0, 0.0, Point3()), Color(0.75, 0.0, 0.25), 1e-6),
          "MixTexture JSON should linearly blend two color textures");

    JsonValue clamped = parse_json(
        "{\"type\":\"math\",\"operation\":\"add\",\"clamp\":true,"
        "\"a\":{\"type\":\"solid\",\"color\":[0.75,0.75,0.75]},"
        "\"b\":{\"type\":\"solid\",\"color\":[0.75,0.75,0.75]}}");
    auto clamped_tex = parse_texture_json(clamped, std::filesystem::current_path());
    check(near_vec(clamped_tex->value(0.0, 0.0, Point3()), Color(1, 1, 1), 1e-6),
          "MathTexture JSON should clamp scalar results when requested");

    JsonValue material = parse_json(
        "{\"type\":\"pbr\",\"albedo\":[0.8,0.8,0.8],\"metallic\":0,"
        "\"roughness\":{\"type\":\"mix\","
        "\"factor\":{\"type\":\"solid\",\"color\":[0.25,0.25,0.25]},"
        "\"color1\":{\"type\":\"solid\",\"color\":[0.2,0.2,0.2]},"
        "\"color2\":{\"type\":\"solid\",\"color\":[0.6,0.6,0.6]}}}");
    Scene scene;
    Material* mat = parse_material(material, scene, std::filesystem::current_path());
    auto* pbr = dynamic_cast<PBR*>(mat);
    check(pbr != nullptr &&
          near(pbr->roughness->value(0.0, 0.0, Point3()).x, 0.3, 1e-6),
          "PBR scalar fields should accept MixTexture JSON nodes");
}

void test_noise_texture_json_is_deterministic_and_transformable() {
    JsonValue noise = parse_json(
        "{\"type\":\"noise\",\"scale\":6,\"detail\":4,\"roughness\":0.55,\"distortion\":0.25}");
    auto tex = parse_texture_json(noise, std::filesystem::current_path());
    Color a = tex->value(0.17, 0.29, Point3());
    Color b = tex->value(0.17, 0.29, Point3());
    check(near_vec(a, b, 1e-12) &&
          a.x >= 0.0 && a.x <= 1.0 &&
          a.y >= 0.0 && a.y <= 1.0 &&
          a.z >= 0.0 && a.z <= 1.0,
          "NoiseTexture JSON should be deterministic and normalized");

    JsonValue transformed = parse_json(
        "{\"type\":\"noise\",\"scale\":6,\"detail\":4,\"uv_offset\":[0.25,0.0]}");
    auto moved = parse_texture_json(transformed, std::filesystem::current_path());
    Color c = moved->value(0.17, 0.29, Point3());
    check(!near_vec(a, c, 1e-6),
          "NoiseTexture JSON should honor UV transform wrappers");
}

void test_invert_and_map_range_texture_json_nodes() {
    JsonValue invert = parse_json(
        "{\"type\":\"invert\",\"factor\":{\"type\":\"solid\",\"color\":[1,1,1]},"
        "\"color\":{\"type\":\"checker\",\"color1\":[0.2,0.2,0.2],"
        "\"color2\":[0.8,0.8,0.8],\"scale\":2}}");
    auto invert_tex = parse_texture_json(invert, std::filesystem::current_path());
    check(near_vec(invert_tex->value(0.1, 0.1, Point3()), Color(0.8, 0.8, 0.8), 1e-6) &&
          near_vec(invert_tex->value(0.6, 0.1, Point3()), Color(0.2, 0.2, 0.2), 1e-6),
          "InvertTexture JSON should dynamically invert wrapped texture colors");

    JsonValue mapped = parse_json(
        "{\"type\":\"map_range\",\"from_min\":0,\"from_max\":1,\"to_min\":0.2,\"to_max\":0.6,"
        "\"value\":{\"type\":\"checker\",\"color1\":[0,0,0],\"color2\":[1,1,1],\"scale\":2}}");
    auto mapped_tex = parse_texture_json(mapped, std::filesystem::current_path());
    check(near_vec(mapped_tex->value(0.1, 0.1, Point3()), Color(0.2, 0.2, 0.2), 1e-6) &&
          near_vec(mapped_tex->value(0.6, 0.1, Point3()), Color(0.6, 0.6, 0.6), 1e-6),
          "MapRangeTexture JSON should dynamically remap scalar texture values");
}

void test_texture_export_metadata_is_ignored_by_scene_parser() {
    JsonValue material = parse_json(
        "{\"type\":\"pbr\",\"unsupported_textures\":[\"MUSGRAVE\"],"
        "\"albedo\":{\"type\":\"checker\",\"coord\":\"generated\","
        "\"color1\":[1,1,1],\"color2\":[0,0,0],\"scale\":2},"
        "\"metallic\":0,\"roughness\":0.5}");
    Scene scene;
    Material* mat = parse_material(material, scene, std::filesystem::current_path());
    auto* pbr = dynamic_cast<PBR*>(mat);
    check(pbr != nullptr, "PBR material should parse with texture export metadata present");
    if (pbr == nullptr) return;
    HitRecord rec;
    rec.u = 0.1;
    rec.v = 0.1;
    rec.p = Point3();
    check(near_vec(pbr->base_color(rec), Color(1, 1, 1), 1e-6),
          "texture coord metadata should not alter renderer-side texture sampling");
}

void test_pbr_scalar_fields_accept_texture_objects() {
    JsonValue material = parse_json(
        "{\"type\":\"pbr\",\"albedo\":[0.8,0.8,0.8],"
        "\"metallic\":{\"type\":\"checker\",\"color1\":[0,0,0],\"color2\":[1,1,1],\"scale\":2},"
        "\"roughness\":{\"type\":\"color_ramp\",\"source\":{\"type\":\"checker\","
        "\"color1\":[0,0,0],\"color2\":[1,1,1],\"scale\":2},\"stops\":["
        "{\"position\":0,\"color\":[0.25,0,0]},"
        "{\"position\":1,\"color\":[0.75,0,0]}]}}");

    Scene scene;
    Material* mat = parse_material(material, scene, std::filesystem::current_path());
    auto* pbr = dynamic_cast<PBR*>(mat);
    check(pbr != nullptr, "PBR material should parse for scalar texture regression");
    if (pbr == nullptr) return;

    check(near(pbr->metallic->value(0.1, 0.1, Point3()).x, 0.0, 1e-6) &&
          near(pbr->metallic->value(0.6, 0.1, Point3()).x, 1.0, 1e-6),
          "PBR metallic should accept a checker texture object");
    check(near(pbr->roughness->value(0.1, 0.1, Point3()).x, 0.25, 1e-6) &&
          near(pbr->roughness->value(0.6, 0.1, Point3()).x, 0.75, 1e-6),
          "PBR roughness should accept a color-ramp texture object");
}

void test_random_seed_repeats_sequence() {
    set_random_seed(1234);
    double a = random_double();
    double b = random_double();
    set_random_seed(1234);
    check(near(a, random_double()), "first random sample should repeat after seed reset");
    check(near(b, random_double()), "second random sample should repeat after seed reset");
}

void test_random_double_stays_in_unit_interval() {
    set_random_seed(4321);
    for (int i = 0; i < 1000; i++) {
        double v = random_double();
        check(v >= 0.0 && v < 1.0, "random_double should stay in [0, 1)");
    }
}

void test_collect_emissive_objects_uses_only_emissive_mesh_triangles() {
    Scene scene;

    auto diffuse = std::make_unique<Lambertian>(Color(0.2, 0.2, 0.2));
    Material* diffuse_ptr = diffuse.get();
    scene.materials.push_back(std::move(diffuse));

    auto emissive = std::make_unique<Emissive>(Color(4, 3, 2));
    Material* emissive_ptr = emissive.get();
    scene.materials.push_back(std::move(emissive));

    auto mesh = std::make_unique<TriangleMesh>();
    mesh->vertices = {
        Point3(0, 0, 0), Point3(1, 0, 0), Point3(0, 1, 0),
        Point3(0, 0, 1), Point3(0, 2, 1), Point3(2, 0, 1)
    };
    mesh->indices = {0, 1, 2, 3, 4, 5};
    mesh->material_per_tri = {diffuse_ptr, emissive_ptr};

    scene.primitives.add(mesh.get());
    scene.objects.push_back(std::move(mesh));

    collect_emissive_objects(scene);

    check(scene.emissive_objects.size() == 1, "mixed mesh should create one emissive sampling subset");
    if (!scene.emissive_objects.empty()) {
        check(near(scene.emissive_objects[0].geometry->area(), 2.0),
              "emissive mesh sampling area should exclude non-emissive triangles");
        check(near_vec(scene.emissive_objects[0].emission, Color(4, 3, 2)),
              "emissive mesh subset should keep the emissive material color");
        check(near(scene.emissive_total_area, 2.0),
              "scene should precompute total emissive sampling area");
    }
}

void test_alpha_mask_hit_traces_past_discarded_surface() {
    Scene scene;
    scene.max_depth = 8;
    scene.ambient_light = Color(0, 0, 0);
    scene.lights.clear();
    scene.environment.type = EnvironmentType::Solid;
    scene.environment.color = Color(0, 0, 0);

    auto mask = std::make_unique<Lambertian>(Color(0, 0, 0));
    mask->alpha_masked = true;
    mask->cutoff = 0.5;
    Material* mask_ptr = mask.get();
    scene.materials.push_back(std::move(mask));

    auto glow = std::make_unique<Emissive>(Color(3, 1, 0.5));
    Material* glow_ptr = glow.get();
    scene.materials.push_back(std::move(glow));

    auto front = std::make_unique<Sphere>(Point3(0, 0, -1.0), 0.45, mask_ptr);
    scene.primitives.add(front.get());
    scene.objects.push_back(std::move(front));

    auto back = std::make_unique<Sphere>(Point3(0, 0, -2.0), 0.45, glow_ptr);
    scene.primitives.add(back.get());
    scene.objects.push_back(std::move(back));

    scene.world = std::make_unique<LinearBVH>(scene.primitives.objects);
    collect_emissive_objects(scene);

    RenderOptions options;
    Color c = ray_color(Ray(Point3(0, 0, 0), Vec3(0, 0, -1)), scene, scene.max_depth,
                        options, infinity, false);

    check(c.x > 2.5 && c.y > 0.8 && c.z > 0.4,
          "alpha masked hits should continue tracing to geometry behind the discarded surface");
}

void test_alpha_masked_surfaces_do_not_cast_solid_shadows() {
    Scene scene;
    auto mask = std::make_unique<Lambertian>(Color(0, 0, 0));
    mask->alpha_masked = true;
    mask->cutoff = 0.5;
    Material* mask_ptr = mask.get();
    scene.materials.push_back(std::move(mask));

    auto front = std::make_unique<Sphere>(Point3(0, 0, -1.0), 0.45, mask_ptr);
    scene.primitives.add(front.get());
    scene.objects.push_back(std::move(front));
    scene.world = std::make_unique<LinearBVH>(scene.primitives.objects);

    bool blocked = is_shadowed(*scene.world, Ray(Point3(0, 0, 0), Vec3(0, 0, -1)), 3.0);
    check(!blocked, "alpha masked discarded hits should not cast solid direct-light shadows");
}

void test_mirror_glass_water_acceptance_scene_loads() {
    Scene scene;
    load_scene("scenes/mirror_glass_water.json", scene);

    check(scene.width == 640 && scene.height == 360,
          "mirror/glass/water acceptance scene should use preview-sized image settings");
    check(scene.primitive_count >= 7,
          "mirror/glass/water acceptance scene should load several primitives");
    check(!scene.emissive_objects.empty(),
          "mirror/glass/water acceptance scene should include at least one emissive area light");
}

void test_disk_light_parses_and_samples() {
    JsonValue light_json;
    light_json.type = JsonValue::Object;
    light_json.objVal["type"] = string_value("disk");
    light_json.objVal["position"] = array3(0.0, 3.0, 0.0);
    light_json.objVal["direction"] = array3(0.0, -1.0, 0.0);
    light_json.objVal["radius"] = number(0.5);
    light_json.objVal["intensity"] = number(4.0);

    Light light = parse_light(light_json);
    check(light.type == LightType::Disk, "disk light should parse to LightType::Disk");
    check(near(light.radius, 0.5), "disk light radius should parse");
    check(near(light.intensity, 4.0), "disk light intensity should parse");
    double area = light.area();
    check(near(area, pi * 0.5 * 0.5), "disk light area should be pi*r^2");

    // Sampling a point on the disk should stay within the disk radius.
    Point3 p = sample_light_point(light, 0.5, 0.0);
    Vec3 offset = p - light.position;
    double horiz = std::sqrt(offset.x * offset.x + offset.z * offset.z);
    check(horiz <= light.radius + 1e-6,
          "disk light sample point should lie within the disk radius");

    // The sample from below the disk should carry radiance and be non-delta.
    LightSample s = sample_scene_light(light, Point3(0, 0, 0), 0.5, 0.5);
    check(!s.is_delta, "disk light should be a non-delta (area) light");
    check(s.radiance.length_squared() > 0, "disk light should emit positive radiance");
    check(s.pdf > 0.0 && std::isfinite(s.pdf),
          "disk light sampling should report a finite solid-angle PDF");
}

void test_dielectric_medium_tracking_applies_beer_lambert_on_exit() {
    // A ray inside a dielectric should accumulate Beer-Lambert absorption
    // based on the path length rec.t and the ray's current medium.  When the
    // ray refracts out, the scattered ray's medium should reset to air.
    Dielectric glass(1.5, Color(1, 1, 1));
    glass.attenuation_color = Color(0.5, 0.7, 1.0);
    glass.attenuation_distance = 1.0;

    HitRecord rec;
    rec.p = Point3(0, 0, -1.0);
    rec.normal = Vec3(0, 0, 1);   // back face: ray exits the dielectric
    rec.front_face = false;
    rec.t = 1.0;                  // traveled 1.0 inside the medium

    Ray inside(Point3(0, 0, 0), Vec3(0, 0, -1));
    inside.medium_color = glass.attenuation_color;
    inside.medium_attenuation_distance = glass.attenuation_distance;

    // Beer-Lambert (attenuation_color^(rec.t/attenuation_distance)) is applied
    // before the stochastic reflect/refract choice, so it is deterministic.
    Color attenuation, emission;
    Ray scattered;
    bool refracted_out = false;
    for (int attempt = 0; attempt < 64 && !refracted_out; attempt++) {
        glass.scatter(inside, rec, attenuation, scattered, emission);
        // Near-normal incidence: Fresnel ~0.04, so most attempts refract.
        refracted_out = near_vec(scattered.medium_color, Color(1, 1, 1), 1e-6);
    }

    // attenuation = base_color(1,1,1) * (0.5, 0.7, 1.0)^1 = (0.5, 0.7, 1.0).
    check(near_vec(attenuation, Color(0.5, 0.7, 1.0), 1e-4),
          "Beer-Lambert should apply attenuation_color^(rec.t/attenuation_distance)");
    check(refracted_out,
          "at near-normal incidence the dielectric should eventually refract out");
    // After refracting out (back face), the scattered ray should be in air.
    check(near_vec(scattered.medium_color, Color(1, 1, 1), 1e-6) &&
          !std::isfinite(scattered.medium_attenuation_distance),
          "refracted-out ray should reset medium to air/vacuum");
}

void test_dielectric_medium_tracking_preserves_medium_on_reflection() {
    // A ray inside the dielectric at a grazing angle triggers total internal
    // reflection, which deterministically reflects and should preserve the
    // incoming medium on the scattered ray.
    Dielectric glass(1.5, Color(1, 1, 1));
    glass.attenuation_color = Color(0.3, 0.5, 0.9);
    glass.attenuation_distance = 2.0;

    // Grazing angle (1.2 in y, -1 in z): cos_theta ~0.64, sin_theta ~0.77,
    // ratio*sin_theta = 1.5*0.77 > 1.0 -> total internal reflection.
    HitRecord rec;
    rec.p = Point3(0, 0, -1.0);
    rec.normal = Vec3(0, 0, 1);
    rec.front_face = false;
    rec.t = 1.5;

    Ray inside(Point3(0, 0, 0), Vec3(0, 1.2, -1).normalized());
    inside.medium_color = glass.attenuation_color;
    inside.medium_attenuation_distance = glass.attenuation_distance;

    Color attenuation, emission;
    Ray scattered;
    glass.scatter(inside, rec, attenuation, scattered, emission);

    // Beer-Lambert for 1.5 / 2.0 = exponent 0.75.
    double exp_val = 1.5 / 2.0;
    Color expected(std::pow(0.3, exp_val), std::pow(0.5, exp_val), std::pow(0.9, exp_val));
    check(near_vec(attenuation, expected, 1e-4),
          "Beer-Lambert should apply for TIR segments using the incoming medium");
    // TIR reflection should preserve the incoming medium on the scattered ray.
    check(near_vec(scattered.medium_color, inside.medium_color, 1e-6) &&
          near(scattered.medium_attenuation_distance, inside.medium_attenuation_distance, 1e-6),
          "reflected ray should preserve the incoming dielectric medium");
}

}  // namespace

int main() {
    test_parse_transform_uses_srt_order();
    test_transform_normal_matches_rotation_direction();
    test_sphere_pole_tangent_is_finite();
    test_degenerate_triangle_uv_tangent_is_finite();
    test_degenerate_mesh_uv_tangent_is_finite();
    test_obj_loader_handles_mixed_missing_attributes();
    test_obj_loader_reads_mtl_diffuse_and_texture();
    test_obj_loader_reads_extended_mtl_fields();
    test_obj_loader_reads_additional_texture_maps();
    test_obj_loader_ignores_missing_mtl_file();
    test_triangle_mesh_exposes_internal_acceleration();
    test_pbr_exposes_brdf_and_pdf_for_direct_lighting();
    test_lambertian_scatter_matches_cosine_pdf_contract();
    test_display_color_exposure_and_tone_mapping();
    test_output_format_detection();
    test_environment_solid_and_gradient_backgrounds();
    test_render_scene_reports_partial_updates();
    test_render_scene_sample_pass_schedule_reports_accumulated_samples();
    test_seeded_render_is_thread_count_independent();
    test_extended_light_types_parse();
    test_extended_light_sampling_outputs_radiance();
    test_analytic_area_light_visibility_for_camera_and_specular_rays();
    test_camera_focal_length_orbit_and_framing_fields();
    test_scene_preset_and_render_block_are_accepted();
    test_firefly_clamp_preserves_hue_by_scaling();
    test_loaded_material_emissive_routes_to_emissive();
    test_loaded_glb_pbr_uses_metallic_roughness_and_normal_textures();
    test_loaded_glb_pbr_emissive_texture_preserves_surface_shading();
    test_loaded_glb_volume_attenuation_reaches_dielectric();
    test_glb_transparency_texture_and_volume_thickness_parse();
    test_glb_texture_transform_parses_scale_and_offset();
    test_dielectric_partial_transmission_factor_stores();
    test_material_alpha_mask_interface();
    test_transformed_texture_applies_uv_scale_offset();
    test_checker_texture_json_scale_and_offset();
    test_image_texture_json_accepts_inline_base64_payload();
    test_image_texture_json_accepts_uv_mapping_fields();
    test_image_texture_linear_interpolation();
    test_image_texture_srgb_decode();
    test_image_texture_extension_modes();
    test_color_ramp_texture_json_wraps_source_texture();
    test_color_ramp_texture_interpolation_modes();
    test_math_and_mix_texture_json_nodes();
    test_noise_texture_json_is_deterministic_and_transformable();
    test_invert_and_map_range_texture_json_nodes();
    test_texture_export_metadata_is_ignored_by_scene_parser();
    test_pbr_scalar_fields_accept_texture_objects();
    test_json_dielectric_accepts_volume_attenuation();
    test_random_seed_repeats_sequence();
    test_random_double_stays_in_unit_interval();
    test_collect_emissive_objects_uses_only_emissive_mesh_triangles();
    test_alpha_mask_hit_traces_past_discarded_surface();
    test_alpha_masked_surfaces_do_not_cast_solid_shadows();
    test_mirror_glass_water_acceptance_scene_loads();
    test_disk_light_parses_and_samples();
    test_dielectric_medium_tracking_applies_beer_lambert_on_exit();
    test_dielectric_medium_tracking_preserves_medium_on_reflection();

    if (failures != 0) {
        std::cerr << failures << " regression test(s) failed\n";
        return 1;
    }
    std::cout << "All regression tests passed\n";
    return 0;
}
