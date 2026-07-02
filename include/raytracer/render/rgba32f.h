// RGBA32F result format shared by Blender bridge and remote server.
#ifndef RT_RGBA32F_H
#define RT_RGBA32F_H

#include "raytracer/render/renderer.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

inline void append_u32_le(std::vector<unsigned char>& bytes, uint32_t value) {
    bytes.push_back(static_cast<unsigned char>(value & 0xffu));
    bytes.push_back(static_cast<unsigned char>((value >> 8) & 0xffu));
    bytes.push_back(static_cast<unsigned char>((value >> 16) & 0xffu));
    bytes.push_back(static_cast<unsigned char>((value >> 24) & 0xffu));
}

inline float finite_channel(double v, bool clamp_nonnegative = true) {
    if (!std::isfinite(v)) return 0.0f;
    if (clamp_nonnegative) v = std::max(0.0, v);
    return static_cast<float>(v);
}

inline void append_float_le(std::vector<unsigned char>& bytes, float value) {
    const unsigned char* raw = reinterpret_cast<const unsigned char*>(&value);
    bytes.insert(bytes.end(), raw, raw + sizeof(float));
}

inline std::vector<unsigned char> encode_rgba32f_pixels(int width,
                                                        int height,
                                                        int samples,
                                                        const std::vector<Color>& pixels,
                                                        bool clamp_nonnegative = true) {
    std::vector<unsigned char> bytes;
    bytes.reserve(16 + static_cast<size_t>(width) * static_cast<size_t>(height) * 16);

    const char magic[8] = {'R', 'T', 'R', 'G', 'B', 'A', 'F', '1'};
    bytes.insert(bytes.end(), magic, magic + 8);
    append_u32_le(bytes, static_cast<uint32_t>(width));
    append_u32_le(bytes, static_cast<uint32_t>(height));

    double scale = samples > 0 ? 1.0 / samples : 1.0;
    for (const Color& raw : pixels) {
        append_float_le(bytes, finite_channel(raw.x * scale, clamp_nonnegative));
        append_float_le(bytes, finite_channel(raw.y * scale, clamp_nonnegative));
        append_float_le(bytes, finite_channel(raw.z * scale, clamp_nonnegative));
        append_float_le(bytes, 1.0f);
    }
    return bytes;
}

inline std::vector<unsigned char> encode_rgba32f(const RenderOutput& output) {
    return encode_rgba32f_pixels(output.width, output.height, output.samples, output.pixels, true);
}

inline std::vector<unsigned char> encode_albedo_rgba32f(const RenderOutput& output) {
    return encode_rgba32f_pixels(output.width, output.height, output.samples, output.albedo_pixels, true);
}

inline std::vector<unsigned char> encode_normal_rgba32f(const RenderOutput& output) {
    return encode_rgba32f_pixels(output.width, output.height, output.samples, output.normal_pixels, false);
}

inline void write_rgba32f_bytes(const std::string& path, const std::vector<unsigned char>& bytes) {
    std::filesystem::path out_path(path);
    if (out_path.has_parent_path()) {
        std::filesystem::create_directories(out_path.parent_path());
    }

    std::ofstream out(path, std::ios::binary);
    if (!out) throw std::runtime_error("Cannot open RGBA output: " + path);
    out.write(reinterpret_cast<const char*>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
}

inline void write_rgba32f(const std::string& path, const RenderOutput& output) {
    write_rgba32f_bytes(path, encode_rgba32f(output));
}

inline void write_albedo_rgba32f(const std::string& path, const RenderOutput& output) {
    write_rgba32f_bytes(path, encode_albedo_rgba32f(output));
}

inline void write_normal_rgba32f(const std::string& path, const RenderOutput& output) {
    write_rgba32f_bytes(path, encode_normal_rgba32f(output));
}

#endif
