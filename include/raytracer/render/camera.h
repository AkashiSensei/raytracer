// Module D: camera -- primary ray generation with FOV + lookat
#ifndef RT_CAMERA_H
#define RT_CAMERA_H

#include "raytracer/math/ray.h"
#include "raytracer/math/util.h"
#include "raytracer/math/vec3.h"

class Camera {
public:
    Camera(Point3 lookfrom, Point3 lookat, Vec3 vup,
           double vfov_deg, double aspect,
           double aperture, double focus_dist)
        : lookfrom_(lookfrom), lookat_(lookat), vfov_degrees_(vfov_deg) {
        double theta = degrees_to_radians(vfov_deg);
        double h = std::tan(theta / 2);
        double viewport_h = 2 * h;
        double viewport_w = aspect * viewport_h;

        Vec3 w = (lookfrom - lookat).normalized();
        Vec3 u = cross(vup, w).normalized();
        Vec3 v = cross(w, u);

        origin_ = lookfrom;
        horizontal_ = focus_dist * viewport_w * u;
        vertical_   = focus_dist * viewport_h * v;
        lower_left_ = origin_ - horizontal_/2 - vertical_/2 - focus_dist * w;

        lens_radius_ = aperture / 2;
        u_ = u; v_ = v;
    }

    Camera(Point3 lookfrom, Point3 lower_left, Vec3 horizontal, Vec3 vertical,
           double aperture, double vfov_deg)
        : lookfrom_(lookfrom),
          lookat_(lower_left + 0.5 * horizontal + 0.5 * vertical),
          origin_(lookfrom),
          lower_left_(lower_left),
          horizontal_(horizontal),
          vertical_(vertical),
          lens_radius_(aperture / 2),
          vfov_degrees_(vfov_deg) {
        u_ = horizontal.length_squared() > 1e-12 ? horizontal.normalized() : Vec3(1, 0, 0);
        v_ = vertical.length_squared() > 1e-12 ? vertical.normalized() : Vec3(0, 1, 0);
    }

    Ray get_ray(double s, double t) const {
        Vec3 rd = lens_radius_ * random_in_unit_disk();
        Vec3 offset = u_ * rd.x + v_ * rd.y;
        Point3 origin = origin_ + offset;
        Vec3 dir = lower_left_ + s*horizontal_ + t*vertical_ - origin;
        return Ray(origin, dir);
    }

    Point3 lookfrom() const { return lookfrom_; }
    Point3 lookat() const { return lookat_; }
    double vfov_degrees() const { return vfov_degrees_; }

private:
    Point3 lookfrom_, lookat_, origin_, lower_left_;
    Vec3 horizontal_, vertical_, u_, v_;
    double lens_radius_;
    double vfov_degrees_;
};

#endif
