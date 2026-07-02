// Module D: main -- CLI + JSON scene loading + render loop
#define TINYOBJLOADER_IMPLEMENTATION
#include "tiny_obj_loader.h"
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION

#include "raytracer/render/image.h"
#include "raytracer/render/renderer.h"
#include "raytracer/scene/scene.h"
#include <chrono>
#include <filesystem>
#include <iostream>
#include <string>

void print_usage(const char* prog) {
    std::cerr << "Usage: " << prog << " [options]\n"
              << "  --scene <path>     scene JSON file (default: scenes/default.json)\n"
              << "  --model <path>     OBJ/GLB model path (uses scenes/obj.json if --scene is omitted)\n"
              << "  --obj <path>       alias of --model\n"
              << "  --out <path>       output image path, .ppm or .png (overrides scene)\n"
              << "  --samples <n>      samples per pixel (overrides scene)\n"
              << "  --threads <n>      render worker threads (default: hardware concurrency)\n"
              << "  --exposure <n>     display exposure multiplier (default: scene/default 1.0)\n"
              << "  --tone-map <mode>  tone mapping: aces, reinhard, none\n"
              << "  --seed <n>         deterministic random seed (default: random_device)\n"
              << "  --firefly-clamp <n> clamp per-sample radiance peak before accumulation\n"
              << "  --direct-light-samples <n> samples per analytic area light hit point (default: 1)\n"
              << "  --emissive-light-samples <n> samples per emissive object direct-light estimate (default: 1)\n"
              << "  --disable-specular-roulette disable Russian roulette on deep specular paths\n"
              << "  --stats            print load/render timing and scene statistics\n"
              << "  --stats-format <m> stats output format: text or json (default: text)\n"
              << "  --direct-only      disable recursive random bounces, use direct light + shadows only\n"
              << "  --preview          fast preview mode: direct-only and samples=1 unless overridden\n";
}

int main(int argc, char* argv[]) {
    std::string scene_path = "scenes/default.json";
    bool scene_specified = false;
    std::string out_override;
    std::string model_override;
    int samples_override = -1;
    double exposure_override = -1.0;
    double firefly_clamp_override = -1.0;
    std::string tone_map_override;
    long long seed_override = -1;
    RenderOptions render_options;

    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "--scene" && i + 1 < argc) {
            scene_path = argv[++i];
            scene_specified = true;
        } else if ((arg == "--model" || arg == "--obj") && i + 1 < argc) {
            model_override = argv[++i];
        } else if (arg == "--out" && i + 1 < argc) {
            out_override = argv[++i];
        } else if (arg == "--samples" && i + 1 < argc) {
            samples_override = std::stoi(argv[++i]);
        } else if (arg == "--threads" && i + 1 < argc) {
            render_options.threads = std::stoi(argv[++i]);
            if (render_options.threads <= 0) {
                std::cerr << "--threads must be greater than 0\n";
                return 1;
            }
        } else if (arg == "--exposure" && i + 1 < argc) {
            exposure_override = std::stod(argv[++i]);
        } else if (arg == "--tone-map" && i + 1 < argc) {
            tone_map_override = argv[++i];
        } else if (arg == "--seed" && i + 1 < argc) {
            seed_override = std::stoll(argv[++i]);
        } else if (arg == "--firefly-clamp" && i + 1 < argc) {
            firefly_clamp_override = std::stod(argv[++i]);
        } else if (arg == "--direct-light-samples" && i + 1 < argc) {
            render_options.direct_light_samples = std::stoi(argv[++i]);
            if (render_options.direct_light_samples <= 0) {
                std::cerr << "--direct-light-samples must be greater than 0\n";
                return 1;
            }
        } else if (arg == "--emissive-light-samples" && i + 1 < argc) {
            render_options.emissive_light_samples = std::stoi(argv[++i]);
            if (render_options.emissive_light_samples <= 0) {
                std::cerr << "--emissive-light-samples must be greater than 0\n";
                return 1;
            }
        } else if (arg == "--stats") {
            render_options.stats = true;
        } else if (arg == "--stats-format" && i + 1 < argc) {
            render_options.stats = true;
            render_options.stats_format = argv[++i];
        } else if (arg == "--direct-only") {
            render_options.direct_only = true;
        } else if (arg == "--disable-specular-roulette") {
            render_options.specular_russian_roulette = false;
        } else if (arg == "--preview") {
            render_options.preview = true;
            render_options.direct_only = true;
        } else if (arg == "--help" || arg == "-h") {
            print_usage(argv[0]);
            return 0;
        } else {
            std::cerr << "Unknown argument: " << arg << "\n\n";
            print_usage(argv[0]);
            return 1;
        }
    }

    if (!model_override.empty() && !scene_specified) {
        scene_path = "scenes/obj.json";
    }

    SceneLoadOptions load_options;
    if (!model_override.empty()) {
        std::filesystem::path model_path(model_override);
        if (model_path.is_relative()) {
            model_path = std::filesystem::absolute(model_path);
        }
        load_options.model_override = model_path.lexically_normal().string();
    }

    Scene scene;
    auto total_start = std::chrono::steady_clock::now();
    auto load_start = std::chrono::steady_clock::now();
    try {
        load_scene(scene_path, scene, load_options);
    } catch (const std::exception& e) {
        std::cerr << "Error loading scene: " << e.what() << "\n";
        return 1;
    }
    auto load_end = std::chrono::steady_clock::now();

    if (!out_override.empty()) scene.output = out_override;
    if (exposure_override > 0) scene.output_options.exposure = exposure_override;
    if (firefly_clamp_override > 0) scene.firefly_clamp = firefly_clamp_override;
    if (!tone_map_override.empty()) scene.output_options.tone_map = parse_tone_map_mode(tone_map_override);
    if (seed_override >= 0) {
        set_random_seed(static_cast<unsigned int>(seed_override));
    } else if (scene.has_seed) {
        set_random_seed(scene.seed);
    }
    if (samples_override > 0)  {
        scene.samples = samples_override;
    } else if (render_options.preview) {
        scene.samples = 1;
    }

    int thread_count = resolve_thread_count(scene, render_options);

    std::cout << "Scene: " << scene_path << "\n"
              << "Model: " << (model_override.empty() ? "(scene)" : load_options.model_override) << "\n"
              << "Image: " << scene.width << "x" << scene.height
              << ", samples=" << scene.samples
              << ", depth=" << scene.max_depth
              << ", mode=" << (render_options.direct_only ? "direct-only" : "path-tracing")
              << ", direct_light_samples=" << render_options.direct_light_samples
              << ", emissive_light_samples=" << render_options.emissive_light_samples
              << ", threads=" << thread_count << "\n"
              << "Primitives: " << scene.primitive_count << "\n";

    RenderCallbacks callbacks;
    callbacks.progress = [](double progress) {
        int pct = static_cast<int>(progress * 100.0);
        std::cerr << "\rProgress: " << pct << "%" << std::flush;
    };

    auto render_start = std::chrono::steady_clock::now();
    RenderOutput output = render_scene(scene, render_options, callbacks);
    auto render_end = std::chrono::steady_clock::now();
    std::cerr << "\rProgress: 100%\n";
    if (output.cancelled) {
        std::cerr << "Render cancelled\n";
        return 2;
    }

    write_image(scene.output, output.width, output.height, output.pixels, output.samples, scene.output_options);
    auto total_end = std::chrono::steady_clock::now();
    std::cout << "Wrote " << scene.output << "\n";
    if (render_options.stats) {
        auto millis = [](std::chrono::steady_clock::duration d) {
            return std::chrono::duration_cast<std::chrono::milliseconds>(d).count();
        };
        double load_ms = millis(load_end - load_start);
        double render_ms = millis(render_end - render_start);
        double total_ms = millis(total_end - total_start);
        if (render_options.stats_format == "json") {
            std::cout << "{"
                      << "\"scene\":\"" << scene_path << "\","
                      << "\"output\":\"" << scene.output << "\","
                      << "\"width\":" << scene.width << ","
                      << "\"height\":" << scene.height << ","
                      << "\"samples\":" << scene.samples << ","
                      << "\"max_depth\":" << scene.max_depth << ","
                      << "\"threads\":" << thread_count << ","
                      << "\"direct_light_samples\":" << render_options.direct_light_samples << ","
                      << "\"emissive_light_samples\":" << render_options.emissive_light_samples << ","
                      << "\"primitives\":" << scene.primitive_count << ","
                      << "\"load_ms\":" << load_ms << ","
                      << "\"render_ms\":" << render_ms << ","
                      << "\"total_ms\":" << total_ms << ","
                      << "\"emissive_area\":" << scene.emissive_total_area
                      << "}\n";
        } else {
            std::cout << "Stats:\n"
                      << "  load_ms=" << load_ms << "\n"
                      << "  render_ms=" << render_ms << "\n"
                      << "  total_ms=" << total_ms << "\n"
                      << "  direct_light_samples=" << render_options.direct_light_samples << "\n"
                      << "  emissive_light_samples=" << render_options.emissive_light_samples << "\n"
                      << "  emissive_area=" << scene.emissive_total_area << "\n";
        }
    }
    return 0;
}
