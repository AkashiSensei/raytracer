// Module C: material -- abstract + concrete (Lambertian/Metal/Dielectric/PBR/Emissive)
#ifndef RT_MATERIAL_H
#define RT_MATERIAL_H

#include "raytracer/geometry/hittable.h"
#include "raytracer/material/texture.h"
#include "raytracer/math/ray.h"
#include "raytracer/math/util.h"
#include "raytracer/math/vec3.h"
#include <algorithm>
#include <cmath>
#include <memory>
#include <utility>

// Beer-Lambert volume absorption for a homogeneous participating medium.
//   T(distance) = attenuation_color^(distance / attenuation_distance)
// When attenuation_color = (1,1,1) or distance is zero, no absorption.  When
// attenuation_distance is infinite (vacuum/air), T = (1,1,1).  This matches the
// glTF KHR_materials_volume spec: sigma_t = -log(attenuation_color) / attenuation_distance.
inline Color beer_lambert(const Color& attenuation_color,
                         double attenuation_distance,
                         double distance) {
    if (attenuation_distance <= 0.0 || !std::isfinite(attenuation_distance)) {
        return Color(1, 1, 1);
    }
    double exponent = std::max(0.0, distance) / attenuation_distance;
    return Color(std::pow(std::clamp(attenuation_color.x, 0.0, 1.0), exponent),
                std::pow(std::clamp(attenuation_color.y, 0.0, 1.0), exponent),
                std::pow(std::clamp(attenuation_color.z, 0.0, 1.0), exponent));
}

class Material {
public:
    virtual ~Material() = default;
    virtual bool scatter(const Ray& r_in, const HitRecord& rec,
                         Color& attenuation, Ray& scattered,
                         Color& emission) const = 0;
    virtual Color base_color(const HitRecord& rec) const {
        (void)rec;
        return Color(0.8, 0.8, 0.8);
    }
    virtual bool is_transparent() const { return false; }
    virtual Color f(const Ray& r_in, const Ray& scattered, const HitRecord& rec) const {
        (void)r_in; (void)scattered; (void)rec;
        return Color(0, 0, 0);
    }
    virtual double pdf(const Ray& r_in, const Ray& scattered, const HitRecord& rec) const {
        (void)r_in; (void)scattered; (void)rec;
        return 0;
    }
    virtual bool is_specular() const { return false; }
    virtual bool is_emissive() const { return false; }
    virtual Color emitted(const HitRecord& rec) const {
        (void)rec;
        return Color(0, 0, 0);
    }
    // Alpha mask: when alpha_mask is true, hits with alpha < alpha_cutoff
    // should be discarded (treated as a miss). The renderer checks this
    // after scatter() and re-casts the ray if discarded.
    virtual bool is_alpha_masked() const { return false; }
    virtual double alpha_cutoff() const { return 0.5; }
    // Double-sided: when true, the material shades both faces. For
    // transparent materials this disables backface culling; for opaque
    // it flips the normal so the front face always faces the ray.
    virtual bool is_double_sided() const { return false; }
};

class Lambertian : public Material {
public:
    std::shared_ptr<Texture> texture;
    bool alpha_masked = false;
    double cutoff = 0.5;

    explicit Lambertian(const Color& albedo)
        : texture(std::make_shared<SolidColorTexture>(albedo)) {}
    Lambertian(std::shared_ptr<Texture> texture)
        : texture(std::move(texture)) {}

    Color base_color(const HitRecord& rec) const override {
        return texture->value(rec.u, rec.v, rec.p);
    }

    bool scatter(const Ray& r_in, const HitRecord& rec,
                 Color& attenuation, Ray& scattered,
                 Color& emission) const override {
        (void)r_in;
        emission = Color(0, 0, 0);
        scattered = Ray(rec.p, random_cosine_direction(rec.normal));
        attenuation = base_color(rec);
        return true;
    }

    Color f(const Ray& r_in, const Ray& scattered, const HitRecord& rec) const override {
        (void)r_in; (void)scattered;
        return base_color(rec) / pi;
    }

    double pdf(const Ray& r_in, const Ray& scattered, const HitRecord& rec) const override {
        (void)r_in;
        double dir_len2 = scattered.direction.length_squared();
        double normal_len2 = rec.normal.length_squared();
        if (dir_len2 <= 1e-12 || normal_len2 <= 1e-12) return 0;
        Vec3 unit_dir = scattered.direction / std::sqrt(dir_len2);
        Vec3 unit_normal = rec.normal / std::sqrt(normal_len2);
        double cos_theta = dot(unit_normal, unit_dir);
        return cos_theta > 0 ? cos_theta / pi : 0;
    }

    bool is_alpha_masked() const override { return alpha_masked; }
    double alpha_cutoff() const override { return cutoff; }
};

class Metal : public Material {
public:
    std::shared_ptr<Texture> texture;
    double fuzz;

    Metal(const Color& albedo, double fuzz = 0)
        : texture(std::make_shared<SolidColorTexture>(albedo)),
          fuzz(std::min(fuzz, 1.0)) {}
    Metal(std::shared_ptr<Texture> texture, double fuzz = 0)
        : texture(std::move(texture)), fuzz(std::min(fuzz, 1.0)) {}

    Color base_color(const HitRecord& rec) const override {
        return texture->value(rec.u, rec.v, rec.p);
    }

    bool scatter(const Ray& r_in, const HitRecord& rec,
                 Color& attenuation, Ray& scattered,
                 Color& emission) const override {
        emission = Color(0, 0, 0);
        Vec3 reflected = reflect(r_in.direction.normalized(), rec.normal);
        scattered = Ray(rec.p, reflected + fuzz * random_in_unit_sphere());
        attenuation = base_color(rec);
        return dot(scattered.direction, rec.normal) > 0;
    }

    bool is_specular() const override { return true; }
};

class Dielectric : public Material {
public:
    double ior;
    Color albedo;
    std::shared_ptr<Texture> albedo_texture;
    Color attenuation_color = Color(1, 1, 1);
    double attenuation_distance = infinity;
    double roughness = 0.0;
    double transmission = 1.0;  // KHR_materials_transmission factor (0..1)
    std::shared_ptr<Texture> transmission_texture;
    bool double_sided = false;

    explicit Dielectric(double index_of_refraction, Color albedo = Color(1.0, 1.0, 1.0))
        : ior(index_of_refraction), albedo(albedo) {}

    Dielectric(double index_of_refraction, std::shared_ptr<Texture> texture)
        : ior(index_of_refraction), albedo(1.0, 1.0, 1.0), albedo_texture(std::move(texture)) {}

    Color base_color(const HitRecord& rec) const override {
        return albedo_texture ? albedo_texture->value(rec.u, rec.v, rec.p) : albedo;
    }

    bool is_transparent() const override { return true; }
    bool is_specular() const override { return true; }
    bool is_double_sided() const override { return double_sided; }

    double transmission_value(const HitRecord& rec) const {
        double t = transmission;
        if (transmission_texture) {
            t *= std::clamp(transmission_texture->value(rec.u, rec.v, rec.p).x, 0.0, 1.0);
        }
        return std::clamp(t, 0.0, 1.0);
    }

    bool scatter(const Ray& r_in, const HitRecord& rec,
                 Color& attenuation, Ray& scattered,
                 Color& emission) const override {
        emission = Color(0, 0, 0);
        // Beer-Lambert for the segment the incoming ray just traveled in its
        // current medium.  For a ray in air this is (1,1,1); for a ray that
        // already refracted into this dielectric (or a nested one) it applies
        // volume absorption proportional to the path length rec.t.
        attenuation = base_color(rec) *
                      beer_lambert(r_in.medium_color, r_in.medium_attenuation_distance, rec.t);
        double ratio = rec.front_face ? (1.0 / ior) : ior;
        Vec3 unit_dir = r_in.direction.normalized();
        double cos_theta = std::fmin(dot(-unit_dir, rec.normal), 1.0);
        double sin_theta = std::sqrt(1.0 - cos_theta * cos_theta);

        double trans = transmission_value(rec);
        // With partial transmission, stochastically choose reflect vs refract
        // weighted by Fresnel and transmission factor. When trans < 1, a
        // fraction of rays reflect instead of refracting, modeling thin
        // transparent surfaces.
        bool cannot_refract = ratio * sin_theta > 1.0;
        double fresnel = schlick(cos_theta, ratio);
        bool did_refract;
        Vec3 dir;
        if (cannot_refract || fresnel + (1.0 - fresnel) * (1.0 - trans) > random_double()) {
            dir = reflect(unit_dir, rec.normal);
            did_refract = false;
        } else {
            dir = refract(unit_dir, rec.normal, ratio);
            did_refract = true;
        }

        double rough = std::clamp(roughness, 0.0, 1.0);
        if (rough > 0.0) {
            // Rough transmission: blur the outgoing direction. Scale the
            // perturbation by rough^2 so low roughness stays sharp.
            Vec3 rough_dir = dir + (rough * rough) * random_in_unit_sphere();
            if (rough_dir.length_squared() > 1e-12 &&
                dot(rough_dir, rec.normal) * dot(dir, rec.normal) >= 0.0) {
                dir = rough_dir.normalized();
            }
        }

        scattered = Ray(rec.p, dir);
        // Track the medium for Beer-Lambert along the next segment:
        // - Refracting into the dielectric (front face hit): the scattered ray
        //   now travels inside this medium, so tag it with our attenuation params.
        // - Refracting out of the dielectric (back face hit): the scattered ray
        //   exits to air/vacuum, so reset the medium to no-absorption defaults.
        // - Reflection (TIR or Fresnel bounce): the ray stays in whatever medium
        //   the incoming ray was in, preserving correct cumulative absorption.
        if (did_refract) {
            if (rec.front_face) {
                scattered.medium_color = attenuation_color;
                scattered.medium_attenuation_distance = attenuation_distance;
            } else {
                scattered.medium_color = Color(1, 1, 1);
                scattered.medium_attenuation_distance = infinity;
            }
        } else {
            scattered.medium_color = r_in.medium_color;
            scattered.medium_attenuation_distance = r_in.medium_attenuation_distance;
        }
        return true;
    }

private:
    static double schlick(double cosine, double ref_idx) {
        double r0 = (1 - ref_idx) / (1 + ref_idx);
        r0 = r0 * r0;
        return r0 + (1 - r0) * std::pow(1 - cosine, 5);
    }
};

// PBR metal-roughness with Cook-Torrance BRDF (GGX + Smith + Schlick)
class PBR : public Material {
public:
    std::shared_ptr<Texture> albedo;
    std::shared_ptr<Texture> metallic;
    std::shared_ptr<Texture> roughness;
    std::shared_ptr<Texture> normal;
    Color emission = Color(0, 0, 0);
    std::shared_ptr<Texture> emission_texture;
    bool has_normal_map = false;
    bool alpha_masked = false;
    double cutoff = 0.5;

    PBR(std::shared_ptr<Texture> albedo_tex, double metallic_val, double roughness_val)
        : albedo(std::move(albedo_tex)),
          metallic(std::make_shared<SolidColorTexture>(Color(metallic_val, 0, 0))),
          roughness(std::make_shared<SolidColorTexture>(Color(roughness_val, 0, 0))),
          normal(std::make_shared<SolidColorTexture>(Color(0.5, 0.5, 1.0))) {}

    Color base_color(const HitRecord& rec) const override {
        return albedo->value(rec.u, rec.v, rec.p);
    }

    Color emitted(const HitRecord& rec) const override {
        if (!emission_texture) return emission;
        return emission * emission_texture->value(rec.u, rec.v, rec.p);
    }

    bool scatter(const Ray& r_in, const HitRecord& rec,
                 Color& attenuation, Ray& scattered,
                 Color& emission) const override {
        emission = Color(0, 0, 0);

        double rough = roughness_value(rec);
        Vec3 n = shading_normal(rec);
        Vec3 v = (-r_in.direction).normalized();
        double n_dot_v = std::max(0.0, dot(n, v));
        if (n_dot_v <= 0) return false;

        Vec3 up = std::fabs(n.x) > 0.9 ? Vec3(0, 1, 0) : Vec3(1, 0, 0);
        Vec3 tbn_t = cross(up, n).normalized();
        Vec3 tbn_b = cross(n, tbn_t);

        double r1 = random_double();
        double r2 = random_double();
        double alpha = rough * rough;
        double phi = 2 * pi * r1;
        double cos_theta = std::sqrt((1 - r2) / (1 + (alpha * alpha - 1) * r2));
        double sin_theta = std::sqrt(1 - cos_theta * cos_theta);
        Vec3 h_local(sin_theta * std::cos(phi), sin_theta * std::sin(phi), cos_theta);
        Vec3 h = (tbn_t * h_local.x + tbn_b * h_local.y + n * h_local.z).normalized();

        Vec3 scattered_dir = (2 * dot(v, h) * h - v).normalized();
        double n_dot_l = std::max(0.0, dot(n, scattered_dir));
        if (n_dot_l <= 0 || dot(rec.normal, scattered_dir) <= 0) return false;

        scattered = Ray(rec.p, scattered_dir);
        double pdf_val = pdf(r_in, scattered, rec);
        if (pdf_val <= 0) return false;

        attenuation = f(r_in, scattered, rec) * n_dot_l / pdf_val;
        return true;
    }

    Color f(const Ray& r_in, const Ray& scattered, const HitRecord& rec) const override {
        Vec3 n = shading_normal(rec);
        Vec3 v = (-r_in.direction).normalized();
        Vec3 l = scattered.direction.normalized();
        double n_dot_l = std::max(0.0, dot(n, l));
        double n_dot_v = std::max(0.0, dot(n, v));
        if (n_dot_l <= 0 || n_dot_v <= 0) return Color(0, 0, 0);

        Vec3 h = (v + l).normalized();
        double n_dot_h = std::max(0.0, dot(n, h));
        double v_dot_h = std::max(0.0, dot(v, h));
        if (n_dot_h <= 0 || v_dot_h <= 0) return Color(0, 0, 0);

        Color base = base_color(rec);
        double met = metallic_value(rec);
        double rough = roughness_value(rec);
        double alpha = rough * rough;
        double a2 = alpha * alpha;
        double denom_d = n_dot_h * n_dot_h * (a2 - 1) + 1;
        double D = a2 / (pi * denom_d * denom_d);

        auto g1 = [&](double ndx) {
            double sq = std::sqrt(a2 + (1 - a2) * ndx * ndx);
            return 2 * ndx / (ndx + sq);
        };
        double G = g1(n_dot_l) * g1(n_dot_v);

        Color F0 = (1 - met) * Color(0.04, 0.04, 0.04) + met * base;
        Color F = F0 + (Color(1, 1, 1) - F0) * std::pow(1 - v_dot_h, 5);

        Color kD = (Color(1, 1, 1) - F) * (1 - met);
        Color diffuse = kD * base / pi;
        Color specular = F * (D * G) / (4 * n_dot_l * n_dot_v);
        return diffuse + specular;
    }

    double pdf(const Ray& r_in, const Ray& scattered, const HitRecord& rec) const override {
        Vec3 n = shading_normal(rec);
        Vec3 v = (-r_in.direction).normalized();
        Vec3 l = scattered.direction.normalized();
        if (dot(n, l) <= 0 || dot(n, v) <= 0) return 0;

        Vec3 h = (v + l).normalized();
        double n_dot_h = std::max(0.0, dot(n, h));
        double v_dot_h = std::max(0.0, dot(v, h));
        if (n_dot_h <= 0 || v_dot_h <= 0) return 0;

        double rough = roughness_value(rec);
        double alpha = rough * rough;
        double a2 = alpha * alpha;
        double denom_d = n_dot_h * n_dot_h * (a2 - 1) + 1;
        double D = a2 / (pi * denom_d * denom_d);
        double pdf_val = (D * n_dot_h) / (4 * v_dot_h);
        return pdf_val > 0 && std::isfinite(pdf_val) ? pdf_val : 0;
    }

    bool is_specular() const override { return false; }
    bool is_alpha_masked() const override { return alpha_masked; }
    double alpha_cutoff() const override { return cutoff; }

private:
    double metallic_value(const HitRecord& rec) const {
        return std::clamp(metallic->value(rec.u, rec.v, rec.p).x, 0.0, 1.0);
    }

    double roughness_value(const HitRecord& rec) const {
        double rough = roughness->value(rec.u, rec.v, rec.p).x;
        return std::clamp(rough, 0.001, 1.0);
    }

    Vec3 shading_normal(const HitRecord& rec) const {
        Vec3 n = rec.normal;
        if (has_normal_map && rec.has_tangent) {
            Color ns = normal->value(rec.u, rec.v, rec.p);
            Vec3 tn = Vec3(2 * ns.x - 1, 2 * ns.y - 1, 2 * ns.z - 1).normalized();
            Vec3 T = rec.tangent.normalized();
            Vec3 B = cross(n, T).normalized();
            n = (T * tn.x + B * tn.y + n * tn.z).normalized();
            if (dot(n, rec.normal) < 0) n = -n;
        }
        return n;
    }
};

// Emissive material (area light)
class Emissive : public Material {
public:
    Color emission;
    std::shared_ptr<Texture> emission_texture;

    explicit Emissive(const Color& e) : emission(e) {}
    Emissive(const Color& e, std::shared_ptr<Texture> texture)
        : emission(e), emission_texture(std::move(texture)) {}

    Color emitted(const HitRecord& rec) const override {
        if (!emission_texture) return emission;
        return emission * emission_texture->value(rec.u, rec.v, rec.p);
    }

    Color base_color(const HitRecord& rec) const override {
        return emitted(rec);
    }

    bool scatter(const Ray& r_in, const HitRecord& rec,
                 Color& attenuation, Ray& scattered,
                 Color& emission_out) const override {
        (void)r_in;
        (void)rec;
        (void)scattered;
        (void)attenuation;
        emission_out = emitted(rec);
        return false;
    }

    bool is_emissive() const override { return true; }
};

#endif
