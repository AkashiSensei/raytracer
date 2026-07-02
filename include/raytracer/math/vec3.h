// Module A: math foundation -- 3D vector (also used as Point3 / Color)
#ifndef RT_VEC3_H
#define RT_VEC3_H

#include <cmath>
#include "raytracer/math/util.h"

class Vec3 {
public:
    double x, y, z;

    Vec3() : x(0), y(0), z(0) {}
    Vec3(double x, double y, double z) : x(x), y(y), z(z) {}

    Vec3 operator-() const { return Vec3(-x, -y, -z); }

    Vec3& operator+=(const Vec3& o) { x += o.x; y += o.y; z += o.z; return *this; }
    Vec3& operator-=(const Vec3& o) { x -= o.x; y -= o.y; z -= o.z; return *this; }
    Vec3& operator*=(double t)      { x *= t;   y *= t;   z *= t;   return *this; }
    Vec3& operator/=(double t)      { return *this *= 1.0 / t; }

    double length_squared() const { return x*x + y*y + z*z; }
    double length() const { return std::sqrt(length_squared()); }
    Vec3 normalized() const { Vec3 v = *this; return v /= length(); }

    static Vec3 random() {
        return Vec3(random_double(), random_double(), random_double());
    }
    static Vec3 random(double min, double max) {
        return Vec3(random_double(min, max), random_double(min, max), random_double(min, max));
    }
};

using Point3 = Vec3;
using Color  = Vec3;

inline Vec3 operator+(const Vec3& a, const Vec3& b) { return Vec3(a.x+b.x, a.y+b.y, a.z+b.z); }
inline Vec3 operator-(const Vec3& a, const Vec3& b) { return Vec3(a.x-b.x, a.y-b.y, a.z-b.z); }
inline Vec3 operator*(const Vec3& a, const Vec3& b) { return Vec3(a.x*b.x, a.y*b.y, a.z*b.z); }
inline Vec3 operator*(double t, const Vec3& a)      { return Vec3(t*a.x,   t*a.y,   t*a.z); }
inline Vec3 operator*(const Vec3& a, double t)      { return t * a; }
inline Vec3 operator/(const Vec3& a, double t)      { return a * (1.0 / t); }

inline double dot(const Vec3& a, const Vec3& b) { return a.x*b.x + a.y*b.y + a.z*b.z; }
inline Vec3 cross(const Vec3& a, const Vec3& b) {
    return Vec3(a.y*b.z - a.z*b.y,
                a.z*b.x - a.x*b.z,
                a.x*b.y - a.y*b.x);
}

inline Vec3 random_in_unit_sphere() {
    while (true) {
        Vec3 p = Vec3::random(-1, 1);
        if (p.length_squared() < 1) return p;
    }
}

inline Vec3 random_unit_vector() {
    return random_in_unit_sphere().normalized();
}

inline Vec3 random_cosine_direction(const Vec3& normal) {
    Vec3 w = normal;
    double w_len2 = w.length_squared();
    if (w_len2 <= 1e-12) {
        w = Vec3(0, 1, 0);
    } else {
        w /= std::sqrt(w_len2);
    }

    Vec3 a = std::fabs(w.x) > 0.9 ? Vec3(0, 1, 0) : Vec3(1, 0, 0);
    Vec3 v = cross(w, a);
    double v_len2 = v.length_squared();
    if (v_len2 <= 1e-12) {
        a = std::fabs(w.y) > 0.9 ? Vec3(0, 0, 1) : Vec3(0, 1, 0);
        v = cross(w, a);
        v_len2 = v.length_squared();
    }
    if (v_len2 <= 1e-12) {
        v = Vec3(1, 0, 0);
    } else {
        v /= std::sqrt(v_len2);
    }
    Vec3 u = cross(v, w);

    double r1 = random_double();
    double r2 = random_double();
    double phi = 2.0 * pi * r1;
    double radius = std::sqrt(r2);
    double x = radius * std::cos(phi);
    double y = radius * std::sin(phi);
    double z = std::sqrt(1.0 - r2);
    return u * x + v * y + w * z;
}

inline Vec3 random_in_unit_disk() {
    while (true) {
        Vec3 p(random_double(-1,1), random_double(-1,1), 0);
        if (p.length_squared() < 1) return p;
    }
}

inline Vec3 reflect(const Vec3& v, const Vec3& n) {
    return v - 2 * dot(v, n) * n;
}

inline Vec3 refract(const Vec3& uv, const Vec3& n, double etai_over_etat) {
    double cos_theta = std::fmin(dot(-uv, n), 1.0);
    Vec3 r_out_perp = etai_over_etat * (uv + cos_theta * n);
    Vec3 r_out_para = -std::sqrt(std::fabs(1.0 - r_out_perp.length_squared())) * n;
    return r_out_perp + r_out_para;
}

inline Color gamma2(const Color& c) {
    return Color(std::sqrt(c.x), std::sqrt(c.y), std::sqrt(c.z));
}

#endif
