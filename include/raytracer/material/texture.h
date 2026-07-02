#ifndef RT_TEXTURE_H
#define RT_TEXTURE_H

#include "raytracer/math/util.h"
#include "raytracer/math/vec2.h"
#include "raytracer/math/vec3.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <memory>
#include <utility>
#include <vector>

typedef unsigned char stbi_uc;
extern stbi_uc* stbi_load(const char* filename, int* x, int* y, int* channels_in_file, int desired_channels);
extern stbi_uc* stbi_load_from_memory(const stbi_uc* buffer, int len, int* x, int* y, int* channels_in_file, int desired_channels);
extern void stbi_image_free(void* retval_from_stbi_load);

class Texture {
public:
    virtual ~Texture() = default;
    virtual Color value(double u, double v, const Point3& p) const = 0;
};

class SolidColorTexture : public Texture {
public:
    Color color;

    SolidColorTexture() : color(0, 0, 0) {}
    explicit SolidColorTexture(const Color& color) : color(color) {}

    Color value(double u, double v, const Point3& p) const override {
        (void)u;
        (void)v;
        (void)p;
        return color;
    }
};

class TintedTexture : public Texture {
public:
    std::shared_ptr<Texture> texture;
    Color tint;

    TintedTexture(std::shared_ptr<Texture> texture, const Color& tint)
        : texture(std::move(texture)), tint(tint) {}

    Color value(double u, double v, const Point3& p) const override {
        return tint * texture->value(u, v, p);
    }
};

class TextureChannel : public Texture {
public:
    std::shared_ptr<Texture> texture;
    int channel = 0;

    TextureChannel(std::shared_ptr<Texture> texture, int channel)
        : texture(std::move(texture)), channel(std::clamp(channel, 0, 2)) {}

    Color value(double u, double v, const Point3& p) const override {
        Color c = texture->value(u, v, p);
        double scalar = channel == 0 ? c.x : (channel == 1 ? c.y : c.z);
        return Color(scalar, 0, 0);
    }
};

// Applies a 2D UV transform to a wrapped texture. With no rotation this matches
// glTF KHR_texture_transform: uv = (uv * scale) + offset.
class TransformedTexture : public Texture {
public:
    std::shared_ptr<Texture> texture;
    Vec2 scale = Vec2(1.0, 1.0);
    Vec2 offset = Vec2(0.0, 0.0);
    double rotation = 0.0;

    TransformedTexture(std::shared_ptr<Texture> texture, const Vec2& scale, const Vec2& offset)
        : texture(std::move(texture)), scale(scale), offset(offset) {}

    TransformedTexture(std::shared_ptr<Texture> texture, const Vec2& scale, const Vec2& offset, double rotation)
        : texture(std::move(texture)), scale(scale), offset(offset), rotation(rotation) {}

    Color value(double u, double v, const Point3& p) const override {
        double tu = u * scale.x;
        double tv = v * scale.y;
        if (std::fabs(rotation) > 1e-12) {
            double radians = degrees_to_radians(rotation);
            double c = std::cos(radians);
            double s = std::sin(radians);
            double ru = c * tu - s * tv;
            double rv = s * tu + c * tv;
            tu = ru;
            tv = rv;
        }
        tu += offset.x;
        tv += offset.y;
        return texture->value(tu, tv, p);
    }
};

class ColorRampTexture : public Texture {
public:
    enum class Interpolation {
        Linear,
        Constant,
        Ease
    };

    std::shared_ptr<Texture> source;
    std::vector<std::pair<double, Color>> stops;
    Interpolation interpolation = Interpolation::Linear;

    ColorRampTexture(std::shared_ptr<Texture> source, std::vector<std::pair<double, Color>> stops)
        : source(std::move(source)), stops(std::move(stops)) {
        if (this->stops.empty()) {
            this->stops.push_back({0.0, Color(0, 0, 0)});
            this->stops.push_back({1.0, Color(1, 1, 1)});
        }
        std::sort(this->stops.begin(), this->stops.end(),
                  [](const auto& a, const auto& b) { return a.first < b.first; });
    }

    Color value(double u, double v, const Point3& p) const override {
        Color src = source ? source->value(u, v, p) : Color(0, 0, 0);
        double t = std::clamp(0.2126 * src.x + 0.7152 * src.y + 0.0722 * src.z, 0.0, 1.0);
        if (t <= stops.front().first) return stops.front().second;
        if (t >= stops.back().first) return stops.back().second;

        for (size_t i = 1; i < stops.size(); i++) {
            if (t <= stops[i].first) {
                double lo = stops[i - 1].first;
                double hi = stops[i].first;
                double f = hi > lo ? (t - lo) / (hi - lo) : 0.0;
                if (interpolation == Interpolation::Constant) return stops[i - 1].second;
                if (interpolation == Interpolation::Ease) f = f * f * (3.0 - 2.0 * f);
                return (1.0 - f) * stops[i - 1].second + f * stops[i].second;
            }
        }
        return stops.back().second;
    }
};

class MathTexture : public Texture {
public:
    enum class Operation {
        Add,
        Subtract,
        Multiply,
        Divide,
        Minimum,
        Maximum,
        Power,
        LessThan,
        GreaterThan
    };

    std::shared_ptr<Texture> a;
    std::shared_ptr<Texture> b;
    Operation operation = Operation::Add;
    bool clamp_result = false;

    MathTexture(std::shared_ptr<Texture> a, std::shared_ptr<Texture> b, Operation operation)
        : a(std::move(a)), b(std::move(b)), operation(operation) {}

    Color value(double u, double v, const Point3& p) const override {
        double av = a ? a->value(u, v, p).x : 0.0;
        double bv = b ? b->value(u, v, p).x : 0.0;
        double result = 0.0;
        switch (operation) {
            case Operation::Add: result = av + bv; break;
            case Operation::Subtract: result = av - bv; break;
            case Operation::Multiply: result = av * bv; break;
            case Operation::Divide: result = std::fabs(bv) > 1e-12 ? av / bv : 0.0; break;
            case Operation::Minimum: result = std::min(av, bv); break;
            case Operation::Maximum: result = std::max(av, bv); break;
            case Operation::Power: result = av >= 0.0 ? std::pow(av, bv) : 0.0; break;
            case Operation::LessThan: result = av < bv ? 1.0 : 0.0; break;
            case Operation::GreaterThan: result = av > bv ? 1.0 : 0.0; break;
        }
        if (clamp_result) result = std::clamp(result, 0.0, 1.0);
        return Color(result, result, result);
    }
};

class MixTexture : public Texture {
public:
    std::shared_ptr<Texture> factor;
    std::shared_ptr<Texture> a;
    std::shared_ptr<Texture> b;

    MixTexture(std::shared_ptr<Texture> factor, std::shared_ptr<Texture> a, std::shared_ptr<Texture> b)
        : factor(std::move(factor)), a(std::move(a)), b(std::move(b)) {}

    Color value(double u, double v, const Point3& p) const override {
        double f = std::clamp(factor ? factor->value(u, v, p).x : 0.5, 0.0, 1.0);
        Color av = a ? a->value(u, v, p) : Color(0, 0, 0);
        Color bv = b ? b->value(u, v, p) : Color(1, 1, 1);
        return (1.0 - f) * av + f * bv;
    }
};

class InvertTexture : public Texture {
public:
    std::shared_ptr<Texture> factor;
    std::shared_ptr<Texture> color;

    InvertTexture(std::shared_ptr<Texture> factor, std::shared_ptr<Texture> color)
        : factor(std::move(factor)), color(std::move(color)) {}

    Color value(double u, double v, const Point3& p) const override {
        double f = std::clamp(factor ? factor->value(u, v, p).x : 1.0, 0.0, 1.0);
        Color c = color ? color->value(u, v, p) : Color(1, 1, 1);
        Color inverted = Color(1, 1, 1) - c;
        return (1.0 - f) * c + f * inverted;
    }
};

class MapRangeTexture : public Texture {
public:
    std::shared_ptr<Texture> value_tex;
    double from_min = 0.0;
    double from_max = 1.0;
    double to_min = 0.0;
    double to_max = 1.0;
    bool clamp_result = true;

    explicit MapRangeTexture(std::shared_ptr<Texture> value_tex)
        : value_tex(std::move(value_tex)) {}

    Color value(double u, double v, const Point3& p) const override {
        double x = value_tex ? value_tex->value(u, v, p).x : 0.0;
        double denom = from_max - from_min;
        double t = std::fabs(denom) > 1e-12 ? (x - from_min) / denom : 0.0;
        double result = to_min + t * (to_max - to_min);
        if (clamp_result) {
            double lo = std::min(to_min, to_max);
            double hi = std::max(to_min, to_max);
            result = std::clamp(result, lo, hi);
        }
        return Color(result, result, result);
    }
};

class NoiseTexture : public Texture {
public:
    double scale = 5.0;
    int detail = 2;
    double roughness = 0.5;
    double distortion = 0.0;

    Color value(double u, double v, const Point3& p) const override {
        (void)p;
        double x = u * std::max(scale, 1e-8);
        double y = v * std::max(scale, 1e-8);
        if (std::fabs(distortion) > 1e-12) {
            double dx = value_noise(x + 17.31, y - 9.17) - 0.5;
            double dy = value_noise(x - 5.83, y + 23.47) - 0.5;
            x += dx * distortion;
            y += dy * distortion;
        }

        int octaves = std::clamp(detail, 1, 16);
        double amplitude = 1.0;
        double sum = 0.0;
        double norm = 0.0;
        for (int i = 0; i < octaves; i++) {
            sum += amplitude * value_noise(x, y);
            norm += amplitude;
            x *= 2.0;
            y *= 2.0;
            amplitude *= std::clamp(roughness, 0.0, 1.0);
        }
        double n = norm > 0.0 ? sum / norm : 0.0;
        return Color(n, n, n);
    }

private:
    static double fade(double t) {
        return t * t * (3.0 - 2.0 * t);
    }

    static double hash_lattice(int x, int y) {
        uint64_t h = splitmix64(static_cast<uint64_t>(static_cast<uint32_t>(x)) |
                                (static_cast<uint64_t>(static_cast<uint32_t>(y)) << 32));
        return static_cast<double>(h >> 11) * 0x1.0p-53;
    }

    static double value_noise(double x, double y) {
        int x0 = static_cast<int>(std::floor(x));
        int y0 = static_cast<int>(std::floor(y));
        int x1 = x0 + 1;
        int y1 = y0 + 1;
        double tx = fade(x - x0);
        double ty = fade(y - y0);

        double n00 = hash_lattice(x0, y0);
        double n10 = hash_lattice(x1, y0);
        double n01 = hash_lattice(x0, y1);
        double n11 = hash_lattice(x1, y1);
        double nx0 = (1.0 - tx) * n00 + tx * n10;
        double nx1 = (1.0 - tx) * n01 + tx * n11;
        return (1.0 - ty) * nx0 + ty * nx1;
    }
};

class CheckerTexture : public Texture {
public:
    Color color1 = Color(0.8, 0.8, 0.8);
    Color color2 = Color(0.2, 0.2, 0.2);
    double scale = 5.0;
    Vec2 uv_scale = Vec2(1.0, 1.0);
    Vec2 uv_offset = Vec2(0.0, 0.0);
    double uv_rotation = 0.0;

    CheckerTexture() = default;
    CheckerTexture(const Color& c1, const Color& c2, double scale)
        : color1(c1), color2(c2), scale(scale) {}

    Color value(double u, double v, const Point3& p) const override {
        (void)p;
        double tu = u * uv_scale.x;
        double tv = v * uv_scale.y;

        if (std::fabs(uv_rotation) > 1e-12) {
            double radians = degrees_to_radians(uv_rotation);
            double c = std::cos(radians);
            double s = std::sin(radians);
            double ru = c * tu - s * tv;
            double rv = s * tu + c * tv;
            tu = ru;
            tv = rv;
        }
        tu += uv_offset.x;
        tv += uv_offset.y;

        double checker_scale = std::max(1e-8, scale);
        int iu = static_cast<int>(std::floor(tu * checker_scale));
        int iv = static_cast<int>(std::floor(tv * checker_scale));
        return ((iu + iv) & 1) == 0 ? color1 : color2;
    }
};

class ImageTexture : public Texture {
public:
    enum class Interpolation {
        Nearest,
        Linear
    };
    enum class Extension {
        Repeat,
        Extend,
        Clip,
        Mirror
    };

    int width = 0;
    int height = 0;
    std::vector<Color> pixels;
    Interpolation interpolation = Interpolation::Nearest;
    Extension extension = Extension::Repeat;
    bool decode_srgb = false;

    ImageTexture() = default;
    explicit ImageTexture(const std::string& path) { load(path); }
    ImageTexture(const std::vector<unsigned char>& encoded, const std::string& mime_type) {
        load(encoded, mime_type);
    }

    Color value(double u, double v, const Point3& p) const override {
        (void)p;
        if (pixels.empty() || width <= 0 || height <= 0) return Color(1, 0, 1);

        if (!normalize_uv(u, v)) return Color(0, 0, 0);

        if (interpolation == Interpolation::Linear) {
            return sample_linear(u, v);
        }
        return sample_nearest(u, v);
    }

private:
    static int positive_mod(int value, int modulus) {
        int result = value % modulus;
        return result < 0 ? result + modulus : result;
    }

    bool normalize_uv(double& u, double& v) const {
        if (extension == Extension::Clip) {
            if (u < 0.0 || u > 1.0 || v < 0.0 || v > 1.0) return false;
            return true;
        }
        if (extension == Extension::Extend) {
            u = std::clamp(u, 0.0, 1.0);
            v = std::clamp(v, 0.0, 1.0);
            return true;
        }
        if (extension == Extension::Mirror) {
            u = mirror_coord(u);
            v = mirror_coord(v);
            return true;
        }

        u = repeat_coord(u);
        v = repeat_coord(v);
        return true;
    }

    static double repeat_coord(double value) {
        value = value - std::floor(value);
        return value < 0.0 ? value + 1.0 : value;
    }

    static double mirror_coord(double value) {
        double period = std::floor(value);
        double local = value - period;
        if (local < 0.0) local += 1.0;
        long long p = static_cast<long long>(period);
        return (p & 1LL) ? 1.0 - local : local;
    }

    Color sample_nearest(double u, double v) const {
        int i = static_cast<int>(u * width);
        int j = static_cast<int>((1.0 - v) * height);
        if (i < 0) i = 0;
        if (j < 0) j = 0;
        if (i >= width) i = width - 1;
        if (j >= height) j = height - 1;
        return pixel_at(i, j);
    }

    Color sample_linear(double u, double v) const {
        double x = u * width - 0.5;
        double y = (1.0 - v) * height - 0.5;
        int x0 = static_cast<int>(std::floor(x));
        int y0 = static_cast<int>(std::floor(y));
        int x1 = x0 + 1;
        int y1 = y0 + 1;
        double tx = x - x0;
        double ty = y - y0;

        Color c00 = pixel_at(sample_index(x0, width), sample_index(y0, height));
        Color c10 = pixel_at(sample_index(x1, width), sample_index(y0, height));
        Color c01 = pixel_at(sample_index(x0, width), sample_index(y1, height));
        Color c11 = pixel_at(sample_index(x1, width), sample_index(y1, height));
        Color cx0 = (1.0 - tx) * c00 + tx * c10;
        Color cx1 = (1.0 - tx) * c01 + tx * c11;
        return (1.0 - ty) * cx0 + ty * cx1;
    }

    Color pixel_at(int x, int y) const {
        Color c = pixels[y * width + x];
        return decode_srgb ? srgb_to_linear(c) : c;
    }

    int sample_index(int value, int size) const {
        if (extension == Extension::Repeat) return positive_mod(value, size);
        return std::clamp(value, 0, size - 1);
    }

    static double srgb_channel_to_linear(double value) {
        value = std::clamp(value, 0.0, 1.0);
        if (value <= 0.04045) return value / 12.92;
        return std::pow((value + 0.055) / 1.055, 2.4);
    }

    static Color srgb_to_linear(const Color& c) {
        return Color(srgb_channel_to_linear(c.x),
                     srgb_channel_to_linear(c.y),
                     srgb_channel_to_linear(c.z));
    }

    static std::string lower_ext(const std::string& path) {
        std::string ext = std::filesystem::path(path).extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return ext;
    }

    static std::string shell_quote(const std::string& s) {
        std::string out = "'";
        for (char c : s) {
            if (c == '\'') out += "'\\''";
            else out += c;
        }
        out += "'";
        return out;
    }

    static std::string temp_path(const std::string& suffix) {
        static int counter = 0;
        std::filesystem::path base = std::filesystem::temp_directory_path();
        return (base / ("raytracer_texture_" + std::to_string(std::time(nullptr)) +
                        "_" + std::to_string(counter++) + suffix)).string();
    }

    static std::string extension_for_mime(const std::string& mime_type) {
        if (mime_type == "image/png") return ".png";
        if (mime_type == "image/jpeg" || mime_type == "image/jpg") return ".jpg";
        if (mime_type == "image/x-portable-pixmap") return ".ppm";
        return ".img";
    }

    static void skip_comments(std::istream& in) {
        while (true) {
            in >> std::ws;
            if (in.peek() != '#') return;
            std::string line;
            std::getline(in, line);
        }
    }

    void load_ppm(const std::string& path) {
        std::ifstream in(path, std::ios::binary);
        if (!in) throw std::runtime_error("Cannot open texture image: " + path);

        std::string magic;
        in >> magic;
        if (magic != "P3" && magic != "P6") {
            throw std::runtime_error("Unsupported texture image format: " + path);
        }

        skip_comments(in);
        in >> width;
        skip_comments(in);
        in >> height;
        skip_comments(in);
        int max_value = 255;
        in >> max_value;
        in.get();

        if (width <= 0 || height <= 0 || max_value <= 0) {
            throw std::runtime_error("Invalid PPM texture: " + path);
        }

        pixels.clear();
        pixels.reserve(static_cast<size_t>(width) * static_cast<size_t>(height));

        if (magic == "P6") {
            std::vector<unsigned char> raw(static_cast<size_t>(width) * static_cast<size_t>(height) * 3);
            in.read(reinterpret_cast<char*>(raw.data()), static_cast<std::streamsize>(raw.size()));
            if (in.gcount() != static_cast<std::streamsize>(raw.size())) {
                throw std::runtime_error("Truncated PPM texture: " + path);
            }
            for (size_t i = 0; i < raw.size(); i += 3) {
                pixels.push_back(Color(raw[i] / double(max_value),
                                       raw[i + 1] / double(max_value),
                                       raw[i + 2] / double(max_value)));
            }
        } else {
            for (int i = 0; i < width * height; i++) {
                int r, g, b;
                in >> r >> g >> b;
                pixels.push_back(Color(r / double(max_value),
                                       g / double(max_value),
                                       b / double(max_value)));
            }
        }
    }

    void load_stb_pixels(stbi_uc* data, int w, int h, const std::string& source) {
        if (!data) throw std::runtime_error("Failed to decode texture image: " + source);
        width = w;
        height = h;
        pixels.clear();
        pixels.reserve(static_cast<size_t>(width) * static_cast<size_t>(height));
        for (size_t i = 0; i < static_cast<size_t>(width) * static_cast<size_t>(height) * 3; i += 3) {
            pixels.push_back(Color(data[i] / 255.0, data[i + 1] / 255.0, data[i + 2] / 255.0));
        }
        stbi_image_free(data);
    }

    void load_with_stb(const std::string& path) {
        int w = 0, h = 0, channels = 0;
        stbi_uc* data = stbi_load(path.c_str(), &w, &h, &channels, 3);
        load_stb_pixels(data, w, h, path);
    }

    void load_with_stb(const std::vector<unsigned char>& encoded, const std::string& mime_type) {
        int w = 0, h = 0, channels = 0;
        stbi_uc* data = stbi_load_from_memory(encoded.data(), static_cast<int>(encoded.size()), &w, &h, &channels, 3);
        load_stb_pixels(data, w, h, mime_type);
    }

    void load(const std::string& path) {
        std::string ext = lower_ext(path);
        if (ext == ".ppm" || ext == ".pnm") load_ppm(path);
        else load_with_stb(path);
    }

    void load(const std::vector<unsigned char>& encoded, const std::string& mime_type) {
        if (mime_type != "image/x-portable-pixmap") {
            load_with_stb(encoded, mime_type);
            return;
        }

        std::string source = temp_path(extension_for_mime(mime_type));
        {
            std::ofstream out(source, std::ios::binary);
            out.write(reinterpret_cast<const char*>(encoded.data()),
                      static_cast<std::streamsize>(encoded.size()));
        }

        try {
            load_ppm(source);
            std::filesystem::remove(source);
        } catch (...) {
            std::filesystem::remove(source);
            throw;
        }
    }
};

#endif
