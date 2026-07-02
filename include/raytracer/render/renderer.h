// Module D: render -- reusable render core shared by CLI and integrations
#ifndef RT_RENDERER_H
#define RT_RENDERER_H

#include "raytracer/geometry/hittable.h"
#include "raytracer/math/ray.h"
#include "raytracer/math/util.h"
#include "raytracer/math/vec3.h"
#include "raytracer/scene/scene.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

struct RenderOptions {
    bool direct_only = false;
    bool preview = false;
    bool stats = false;
    std::string stats_format = "text";
    int threads = 0;
    int partial_update_rows = 0;
};

struct RenderOutput {
    int width = 0;
    int height = 0;
    int samples = 1;
    bool cancelled = false;
    std::vector<Color> pixels;
};

struct RenderCallbacks {
    std::function<void(double)> progress;
    std::function<void(const RenderOutput&, double)> partial;
    std::function<bool()> should_cancel;
};

enum class RayPathType {
    Camera,
    Specular,
    Diffuse
};

struct VisibleLightHit {
    double t = infinity;
    Color emission = Color(0, 0, 0);
};

inline bool light_visible_to_path(const Light& light, RayPathType path_type) {
    if (path_type == RayPathType::Camera) return light.visible_camera;
    if (path_type == RayPathType::Specular) return light.visible_specular;
    return light.visible_diffuse;
}

inline bool hit_rect_light(const Light& light, const Ray& ray, double t_min, double t_max, double& t_out) {
    Vec3 plane_normal = cross(light.u, light.v);
    if (plane_normal.length_squared() <= 1e-12) return false;
    plane_normal = plane_normal.normalized();

    double denom = dot(plane_normal, ray.direction);
    if (std::fabs(denom) <= 1e-10) return false;

    double t = dot(light.position - ray.origin, plane_normal) / denom;
    if (t <= t_min || t >= t_max) return false;

    Vec3 view_from_light = -ray.direction.normalized();
    if (dot(safe_normalized(light.direction, plane_normal), view_from_light) <= 0.0) return false;

    Point3 p = ray.at(t);
    Vec3 rel = p - light.position;
    double u_len2 = light.u.length_squared();
    double v_len2 = light.v.length_squared();
    if (u_len2 <= 1e-12 || v_len2 <= 1e-12) return false;
    double u_coord = dot(rel, light.u) / u_len2;
    double v_coord = dot(rel, light.v) / v_len2;
    if (std::fabs(u_coord) > 0.5 || std::fabs(v_coord) > 0.5) return false;

    t_out = t;
    return true;
}

inline bool hit_disk_light(const Light& light, const Ray& ray, double t_min, double t_max, double& t_out) {
    Vec3 plane_normal = safe_normalized(light.direction, Vec3(0, -1, 0));
    double denom = dot(plane_normal, ray.direction);
    if (std::fabs(denom) <= 1e-10) return false;

    double t = dot(light.position - ray.origin, plane_normal) / denom;
    if (t <= t_min || t >= t_max) return false;

    Vec3 view_from_light = -ray.direction.normalized();
    if (dot(plane_normal, view_from_light) <= 0.0) return false;

    Vec3 rel = ray.at(t) - light.position;
    double along_normal = dot(rel, plane_normal);
    Vec3 radial = rel - along_normal * plane_normal;
    if (radial.length_squared() > light.radius * light.radius) return false;

    t_out = t;
    return true;
}

inline bool hit_sphere_light(const Light& light, const Ray& ray, double t_min, double t_max, double& t_out) {
    Vec3 oc = ray.origin - light.position;
    double a = ray.direction.length_squared();
    double half_b = dot(oc, ray.direction);
    double c = oc.length_squared() - light.radius * light.radius;
    double discriminant = half_b * half_b - a * c;
    if (discriminant < 0) return false;
    double sqrtd = std::sqrt(discriminant);

    double root = (-half_b - sqrtd) / a;
    if (root <= t_min || root >= t_max) {
        root = (-half_b + sqrtd) / a;
        if (root <= t_min || root >= t_max) return false;
    }

    t_out = root;
    return true;
}

inline bool hit_visible_analytic_light(const Scene& scene,
                                       const Ray& ray,
                                       RayPathType path_type,
                                       double t_max,
                                       VisibleLightHit& hit) {
    bool found = false;
    double closest = t_max;
    for (const Light& light : scene.lights) {
        if (!light_visible_to_path(light, path_type)) continue;

        double t = infinity;
        bool light_hit = false;
        if (light.type == LightType::Rect) {
            light_hit = hit_rect_light(light, ray, 0.001, closest, t);
        } else if (light.type == LightType::Disk) {
            light_hit = hit_disk_light(light, ray, 0.001, closest, t);
        } else if (light.type == LightType::Sphere) {
            light_hit = hit_sphere_light(light, ray, 0.001, closest, t);
        }

        if (!light_hit) continue;
        closest = t;
        hit.t = t;
        hit.emission = light.color * light.intensity;
        found = true;
    }
    return found;
}

inline bool is_shadowed(const Hittable& world, const Ray& shadow_ray, double max_t) {
    Ray ray = shadow_ray;
    double remaining_t = max_t;
    for (int skip_count = 0; skip_count < 16; skip_count++) {
        HitRecord shadow_rec;
        if (!world.hit(ray, 0.001, remaining_t, shadow_rec)) return false;

        if (shadow_rec.material && shadow_rec.material->is_alpha_masked()) {
            Color bc = shadow_rec.material->base_color(shadow_rec);
            double alpha = std::max({bc.x, bc.y, bc.z});
            if (alpha < shadow_rec.material->alpha_cutoff()) {
                Vec3 dir = ray.direction.normalized();
                remaining_t -= shadow_rec.t;
                if (remaining_t <= 0.001) return false;
                ray = Ray(shadow_rec.p + 0.001 * dir, ray.direction);
                continue;
            }
        }

        return true;
    }
    return false;
}

inline const EmissiveObject* sample_emissive_by_area(const Scene& scene,
                                                     double r,
                                                     double total_area) {
    if (total_area <= 0) return nullptr;
    double target = r * total_area;
    double accum = 0;
    for (const EmissiveObject& eo : scene.emissive_objects) {
        accum += eo.geometry->area();
        if (target <= accum) return &eo;
    }
    return scene.emissive_objects.empty() ? nullptr : &scene.emissive_objects.back();
}

inline Color direct_delta_lights(const Ray& r_in, const HitRecord& rec, const Scene& scene) {
    Color base = rec.material ? rec.material->base_color(rec) : Color(0.8, 0.8, 0.8);
    Color result = base * scene.ambient_light;

    for (const Light& light : scene.lights) {
        double r1 = 0.5;
        double r2 = 0.5;
        if (light.type == LightType::Sphere || light.type == LightType::Rect ||
            light.type == LightType::Disk) {
            r1 = random_double();
            r2 = random_double();
        }

        LightSample sample = sample_scene_light(light, rec.p, r1, r2);
        if (sample.radiance.length_squared() <= 0) continue;

        Vec3 light_dir = sample.direction;
        double max_t = std::isfinite(sample.distance) ? sample.distance - 0.001 : infinity;
        double n_dot_l = dot(rec.normal, light_dir);
        if (n_dot_l <= 0) continue;

        Ray shadow_ray(rec.p + 0.001 * rec.normal, light_dir);
        if (is_shadowed(*scene.world, shadow_ray, max_t)) continue;

        Ray light_ray(rec.p, light_dir);
        Color brdf = rec.material ? rec.material->f(r_in, light_ray, rec) : base / pi;
        result += brdf * sample.radiance * n_dot_l;
    }

    return result;
}

inline Color ray_color(const Ray& r, const Scene& scene, int depth,
                       const RenderOptions& options,
                       double prev_pdf,
                       bool prev_brdf,
                       RayPathType path_type = RayPathType::Camera) {
    if (depth <= 0) return Color(0, 0, 0);

    HitRecord rec;
    bool hit_world = scene.world->hit(r, 0.001, infinity, rec);
    double world_t = hit_world ? rec.t : infinity;
    VisibleLightHit light_hit;
    if (hit_visible_analytic_light(scene, r, path_type, world_t, light_hit)) {
        return light_hit.emission;
    }

    if (!hit_world) {
        return scene_background(scene, r);
    }

    if (rec.material && rec.material->is_emissive()) {
        Color emitted = rec.material->emitted(rec);
        if (prev_brdf && prev_pdf > 0 && !scene.emissive_objects.empty()) {
            double total_area = scene.emissive_total_area;
            double pdf_light = 0;
            if (total_area > 0) {
                double dist2 = (rec.p - r.origin).length_squared();
                double cos_light = dot(rec.normal, -r.direction.normalized());
                if (cos_light > 0) pdf_light = dist2 / (cos_light * total_area);
            }
            double w_brdf = prev_pdf / (prev_pdf + pdf_light);
            return emitted * w_brdf;
        }
        return emitted;
    }

    if (rec.material && rec.material->is_alpha_masked()) {
        Color bc = rec.material->base_color(rec);
        double alpha = std::max({bc.x, bc.y, bc.z});
        if (alpha < rec.material->alpha_cutoff()) {
            Vec3 continue_dir = r.direction.normalized();
            Ray continue_ray(rec.p + 0.001 * continue_dir, r.direction);
            // Preserve the current medium so Beer-Lambert keeps accumulating
            // across alpha-mask cutouts (e.g. a leaf inside a glass vase).
            continue_ray.medium_color = r.medium_color;
            continue_ray.medium_attenuation_distance = r.medium_attenuation_distance;
            return ray_color(continue_ray, scene, depth, options, prev_pdf, prev_brdf, path_type);
        }
    }

    Ray scattered;
    Color attenuation, scatter_emission;
    bool did_scatter = rec.material && rec.material->scatter(r, rec, attenuation, scattered, scatter_emission);
    // Non-transparent materials don't change the medium, so the scattered ray
    // keeps traveling in whatever medium the incoming ray was in.  Dielectric
    // already set the outgoing medium in scatter(); leave it untouched here.
    if (did_scatter && rec.material && !rec.material->is_transparent()) {
        scattered.medium_color = r.medium_color;
        scattered.medium_attenuation_distance = r.medium_attenuation_distance;
    }
    Color emission = scatter_emission + (rec.material ? rec.material->emitted(rec) : Color(0, 0, 0));

    if (rec.material && rec.material->is_specular()) {
        if (options.direct_only) return emission;
        if (!did_scatter) return emission;

        int bounces_done = scene.max_depth - depth;
        bool rr_active = bounces_done >= 5;
        double p = 1.0;
        if (rr_active) {
            double lum = 0.2126 * attenuation.x + 0.7152 * attenuation.y + 0.0722 * attenuation.z;
            p = std::min(0.95, std::max(0.1, lum));
            if (random_double() > p) return emission;
        }

        Color child = ray_color(scattered, scene, depth - 1, options, 1.0, true, RayPathType::Specular);
        if (rr_active) child = child / p;
        return emission + attenuation * child;
    }

    Color direct = direct_delta_lights(r, rec, scene);

    if (!scene.emissive_objects.empty()) {
        double total_area = scene.emissive_total_area;
        const EmissiveObject* eo = sample_emissive_by_area(scene, random_double(), total_area);
        Vec3 light_normal;
        Point3 light_point = eo
            ? eo->geometry->sample_point(random_double(), random_double(), &light_normal)
            : Point3();

        Vec3 to_light = light_point - rec.p;
        double dist2 = to_light.length_squared();
        if (eo && total_area > 0 && dist2 > 1e-8) {
            double dist = std::sqrt(dist2);
            Vec3 light_dir = to_light / dist;
            double n_dot_l = dot(rec.normal, light_dir);
            if (n_dot_l > 0) {
                double cos_light = dot(light_normal, -light_dir);
                if (cos_light > 0) {
                    Ray shadow_ray(rec.p + 0.001 * rec.normal, light_dir);
                    if (!is_shadowed(*scene.world, shadow_ray, dist - 0.001)) {
                        double pdf_light = (dist2 / cos_light) / total_area;
                        Ray light_ray(rec.p, light_dir);
                        Color f_val = rec.material ? rec.material->f(r, light_ray, rec) : Color(0, 0, 0);
                        double brdf_pdf = rec.material ? rec.material->pdf(r, light_ray, rec) : 0;
                        double w_light = pdf_light / (pdf_light + brdf_pdf);
                        direct += eo->emission * f_val * n_dot_l * w_light / pdf_light;
                    }
                }
            }
        }
    }

    if (options.direct_only) return emission + direct;
    if (!did_scatter) return emission + direct;

    double brdf_pdf = rec.material->pdf(r, scattered, rec);
    Color f_val = rec.material->f(r, scattered, rec);
    if (brdf_pdf <= 0) {
        return emission + direct +
               attenuation * ray_color(scattered, scene, depth - 1, options, 1.0, true, RayPathType::Diffuse);
    }

    Color indirect = f_val * dot(rec.normal, scattered.direction) / brdf_pdf
                   * ray_color(scattered, scene, depth - 1, options, brdf_pdf, true, RayPathType::Diffuse);

    return emission + direct + indirect;
}

inline int resolve_thread_count(const Scene& scene, const RenderOptions& options) {
    unsigned int hardware_threads = std::thread::hardware_concurrency();
    int thread_count = options.threads > 0
        ? options.threads
        : static_cast<int>(hardware_threads == 0 ? 1 : hardware_threads);
    if (thread_count > scene.height) thread_count = scene.height;
    if (thread_count < 1) thread_count = 1;
    return thread_count;
}

inline RenderOutput render_scene(const Scene& scene,
                                 const RenderOptions& options,
                                 const RenderCallbacks& callbacks = RenderCallbacks()) {
    RenderOutput output;
    output.width = scene.width;
    output.height = scene.height;
    output.samples = scene.samples;
    output.pixels.assign(static_cast<size_t>(scene.width) * static_cast<size_t>(scene.height),
                         Color(0, 0, 0));

    int thread_count = resolve_thread_count(scene, options);
    std::atomic<int> next_row{0};
    std::atomic<int> rows_done{0};
    std::atomic<int> last_pct{-1};
    std::atomic<bool> cancelled{false};
    std::mutex output_mutex;
    std::mutex partial_mutex;
    int partial_interval = options.partial_update_rows > 0
        ? std::max(1, options.partial_update_rows)
        : 0;
    bool partial_enabled = callbacks.partial && partial_interval > 0;

    auto cancel_requested = [&]() {
        if (cancelled.load()) return true;
        if (callbacks.should_cancel && callbacks.should_cancel()) {
            cancelled.store(true);
            return true;
        }
        return false;
    };

    auto report_progress = [&](int done_rows) {
        if (!callbacks.progress || scene.height <= 0) return;
        int pct = 100 * done_rows / scene.height;
        int previous = last_pct.load();
        if ((pct % 5 == 0 || done_rows == scene.height) && pct != previous &&
            last_pct.compare_exchange_strong(previous, pct)) {
            callbacks.progress(double(done_rows) / double(scene.height));
        }
    };

    auto report_partial = [&](int done_rows) {
        if (!partial_enabled || scene.height <= 0) return;
        if (done_rows != scene.height && (done_rows % partial_interval) != 0) return;

        std::lock_guard<std::mutex> partial_lock(partial_mutex);
        RenderOutput snapshot;
        {
            std::lock_guard<std::mutex> output_lock(output_mutex);
            snapshot = output;
        }
        callbacks.partial(snapshot, double(done_rows) / double(scene.height));
    };

    auto render_worker = [&]() {
        while (!cancel_requested()) {
            int j = next_row.fetch_add(1);
            if (j >= scene.height) break;

            int sample_row = scene.height - 1 - j;
            std::vector<Color> row_pixels;
            if (partial_enabled) {
                row_pixels.assign(static_cast<size_t>(scene.width), Color(0, 0, 0));
            }
            bool row_cancelled = false;
            for (int i = 0; i < scene.width; i++) {
                if (cancel_requested()) {
                    row_cancelled = true;
                    break;
                }
                Color col(0, 0, 0);
                for (int s = 0; s < scene.samples; s++) {
                    if ((s & 15) == 0 && cancel_requested()) {
                        row_cancelled = true;
                        break;
                    }
                    double offset_x = (options.direct_only && scene.samples == 1) ? 0.5 : random_double();
                    double offset_y = (options.direct_only && scene.samples == 1) ? 0.5 : random_double();
                    double u = (i + offset_x) / std::max(1, scene.width - 1);
                    double v = (sample_row + offset_y) / std::max(1, scene.height - 1);
                    Color sample = ray_color(
                        scene.camera->get_ray(u, v), scene, scene.max_depth, options, infinity, false);
                    col += clamp_radiance(sample, scene.firefly_clamp);
                }
                if (row_cancelled) break;
                if (partial_enabled) {
                    row_pixels[static_cast<size_t>(i)] = col;
                } else {
                    output.pixels[static_cast<size_t>(j) * static_cast<size_t>(scene.width) +
                                  static_cast<size_t>(i)] = col;
                }
            }

            if (cancelled.load() || row_cancelled) break;
            if (partial_enabled) {
                std::lock_guard<std::mutex> lock(output_mutex);
                std::copy(row_pixels.begin(),
                          row_pixels.end(),
                          output.pixels.begin() +
                              static_cast<size_t>(j) * static_cast<size_t>(scene.width));
            }
            int done = rows_done.fetch_add(1) + 1;
            report_progress(done);
            report_partial(done);
        }
    };

    std::vector<std::thread> workers;
    workers.reserve(static_cast<size_t>(thread_count));
    for (int t = 0; t < thread_count; t++) {
        workers.emplace_back(render_worker);
    }
    for (std::thread& worker : workers) {
        worker.join();
    }

    output.cancelled = cancelled.load();
    if (!output.cancelled && callbacks.progress) callbacks.progress(1.0);
    return output;
}

#endif
