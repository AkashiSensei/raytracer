// Remote HTTP renderer server for Blender integrations.
#define TINYOBJLOADER_IMPLEMENTATION
#include "tiny_obj_loader.h"
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

#include "raytracer/render/renderer.h"
#include "raytracer/render/rgba32f.h"
#include "raytracer/scene/scene.h"
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cctype>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <netdb.h>
#include <sstream>
#include <stdexcept>
#include <string>
#include <sys/socket.h>
#include <sys/types.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {

struct ServerArgs {
    std::string host = "127.0.0.1";
    std::string port = "8080";
    int default_threads = 0;
    int max_request_threads = 0;
    bool once = false;
    size_t max_body_bytes = 512ull * 1024ull * 1024ull;
};

constexpr int kRemoteSamplePassBatch = 64;

struct HttpRequest {
    std::string method;
    std::string target;
    std::string path;
    std::string query;
    std::map<std::string, std::string> headers;
    std::string body;
};

struct RenderJob {
    std::string id;
    std::atomic<bool> cancel_requested{false};
    mutable std::mutex mutex;
    double progress = 0.0;
    bool started = false;
    bool done = false;
    bool cancelled = false;
    bool failed = false;
    std::string error;
    std::vector<unsigned char> result;
    std::vector<unsigned char> partial_result;
    unsigned long long partial_seq = 0;
    RenderProgressInfo render_status;
    bool has_render_status = false;
};

std::mutex g_jobs_mutex;
std::map<std::string, std::shared_ptr<RenderJob>> g_jobs;
std::atomic<unsigned long long> g_next_job_id{1};

void print_usage(const char* prog) {
    std::cerr << "Usage: " << prog << " [options]\n"
              << "  --host <host>        listen host, default 127.0.0.1\n"
              << "  --port <port>        listen port, default 8080\n"
              << "  --default-threads <n> default render worker threads when request omits threads\n"
              << "  --max-request-threads <n> cap per-request render threads, default unlimited\n"
              << "  --threads <n>         compatibility alias for --default-threads\n"
              << "  --max-body-mb <n>    maximum request body size, default 512\n"
              << "  --once               handle one request then exit\n"
              << "  --version            print server protocol version\n";
}

bool parse_args(int argc, char* argv[], ServerArgs& args) {
    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "--host" && i + 1 < argc) {
            args.host = argv[++i];
        } else if (arg == "--port" && i + 1 < argc) {
            args.port = argv[++i];
        } else if ((arg == "--default-threads" || arg == "--threads") && i + 1 < argc) {
            args.default_threads = std::stoi(argv[++i]);
            if (args.default_threads <= 0) {
                std::cerr << arg << " must be greater than 0\n";
                return false;
            }
        } else if (arg == "--max-request-threads" && i + 1 < argc) {
            args.max_request_threads = std::stoi(argv[++i]);
            if (args.max_request_threads <= 0) {
                std::cerr << "--max-request-threads must be greater than 0\n";
                return false;
            }
        } else if (arg == "--max-body-mb" && i + 1 < argc) {
            int mb = std::stoi(argv[++i]);
            if (mb <= 0) {
                std::cerr << "--max-body-mb must be greater than 0\n";
                return false;
            }
            args.max_body_bytes = static_cast<size_t>(mb) * 1024ull * 1024ull;
        } else if (arg == "--once") {
            args.once = true;
        } else if (arg == "--version") {
            std::cout << "raytracer-server protocol=1\n";
            std::exit(0);
        } else if (arg == "--help" || arg == "-h") {
            print_usage(argv[0]);
            std::exit(0);
        } else {
            std::cerr << "Unknown argument: " << arg << "\n";
            return false;
        }
    }
    return true;
}

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return s;
}

std::string trim(const std::string& s) {
    size_t first = s.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return "";
    size_t last = s.find_last_not_of(" \t\r\n");
    return s.substr(first, last - first + 1);
}

size_t content_length(const HttpRequest& request) {
    auto it = request.headers.find("content-length");
    if (it == request.headers.end()) return 0;
    return static_cast<size_t>(std::stoull(it->second));
}

HttpRequest parse_request_head(const std::string& head) {
    std::istringstream input(head);
    std::string line;
    HttpRequest request;

    if (!std::getline(input, line)) throw std::runtime_error("empty request");
    if (!line.empty() && line.back() == '\r') line.pop_back();

    std::istringstream request_line(line);
    std::string version;
    request_line >> request.method >> request.target >> version;
    if (request.method.empty() || request.target.empty()) {
        throw std::runtime_error("invalid request line");
    }

    size_t query_pos = request.target.find('?');
    request.path = query_pos == std::string::npos
        ? request.target
        : request.target.substr(0, query_pos);
    request.query = query_pos == std::string::npos
        ? ""
        : request.target.substr(query_pos + 1);

    while (std::getline(input, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) break;
        size_t colon = line.find(':');
        if (colon == std::string::npos) continue;
        request.headers[lower(trim(line.substr(0, colon)))] = trim(line.substr(colon + 1));
    }
    return request;
}

HttpRequest read_request(int client_fd, size_t max_body_bytes) {
    std::string data;
    char buffer[8192];
    size_t header_end = std::string::npos;

    while ((header_end = data.find("\r\n\r\n")) == std::string::npos) {
        ssize_t n = recv(client_fd, buffer, sizeof(buffer), 0);
        if (n <= 0) throw std::runtime_error("connection closed before headers");
        data.append(buffer, buffer + n);
        if (data.size() > 1024 * 1024) throw std::runtime_error("request headers too large");
    }

    HttpRequest request = parse_request_head(data.substr(0, header_end + 4));
    size_t length = content_length(request);
    if (length > max_body_bytes) throw std::runtime_error("request body too large");

    size_t body_start = header_end + 4;
    while (data.size() < body_start + length) {
        ssize_t n = recv(client_fd, buffer, sizeof(buffer), 0);
        if (n <= 0) throw std::runtime_error("connection closed before body");
        data.append(buffer, buffer + n);
    }

    request.body = data.substr(body_start, length);
    return request;
}

void send_all(int fd, const char* data, size_t size) {
    size_t sent = 0;
    while (sent < size) {
        ssize_t n = send(fd, data + sent, size - sent, 0);
        if (n <= 0) throw std::runtime_error("send failed");
        sent += static_cast<size_t>(n);
    }
}

void send_response(int client_fd,
                   int status,
                   const std::string& reason,
                   const std::string& content_type,
                   const std::vector<unsigned char>& body) {
    std::ostringstream head;
    head << "HTTP/1.1 " << status << " " << reason << "\r\n"
         << "Content-Type: " << content_type << "\r\n"
         << "Content-Length: " << body.size() << "\r\n"
         << "Connection: close\r\n"
         << "X-Raytracer-Protocol: 1\r\n"
         << "\r\n";
    std::string head_text = head.str();
    send_all(client_fd, head_text.data(), head_text.size());
    if (!body.empty()) {
        send_all(client_fd, reinterpret_cast<const char*>(body.data()), body.size());
    }
}

std::vector<unsigned char> text_body(const std::string& text) {
    return std::vector<unsigned char>(text.begin(), text.end());
}

std::string json_escape(const std::string& value) {
    std::string out;
    out.reserve(value.size() + 8);
    for (char c : value) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default: out += c; break;
        }
    }
    return out;
}

std::map<std::string, std::string> parse_query(const std::string& query) {
    std::map<std::string, std::string> params;
    size_t start = 0;
    while (start <= query.size()) {
        size_t amp = query.find('&', start);
        std::string part = query.substr(start, amp == std::string::npos ? std::string::npos : amp - start);
        if (!part.empty()) {
            size_t eq = part.find('=');
            if (eq == std::string::npos) {
                params[part] = "";
            } else {
                params[part.substr(0, eq)] = part.substr(eq + 1);
            }
        }
        if (amp == std::string::npos) break;
        start = amp + 1;
    }
    return params;
}

bool query_bool(const std::map<std::string, std::string>& params, const std::string& key, bool fallback) {
    auto it = params.find(key);
    if (it == params.end()) return fallback;
    std::string value = lower(it->second);
    return value.empty() || value == "1" || value == "true" || value == "yes" || value == "on";
}

int query_int(const std::map<std::string, std::string>& params, const std::string& key, int fallback) {
    auto it = params.find(key);
    if (it == params.end() || it->second.empty()) return fallback;
    return std::stoi(it->second);
}

RenderSchedule query_render_schedule(const std::map<std::string, std::string>& params) {
    auto it = params.find("render_schedule");
    if (it == params.end() || it->second.empty()) return RenderSchedule::Rows;
    std::string value = lower(it->second);
    if (value == "sample-passes" || value == "sample_passes" ||
        value == "sample-pass" || value == "sample_pass") {
        return RenderSchedule::SamplePasses;
    }
    return RenderSchedule::Rows;
}

int apply_thread_limit(int requested_threads, const ServerArgs& args) {
    if (requested_threads > 0 && args.max_request_threads > 0) {
        return std::min(requested_threads, args.max_request_threads);
    }
    return requested_threads;
}

std::filesystem::path temp_scene_path() {
    auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    std::string name = "raytracer_remote_" + std::to_string(getpid()) + "_" + std::to_string(now) + ".json";
    return std::filesystem::temp_directory_path() / name;
}

Scene load_scene_from_body(const std::string& body) {
    if (body.empty()) {
        throw std::runtime_error("empty render request body");
    }

    std::filesystem::path scene_path = temp_scene_path();
    {
        std::ofstream scene_file(scene_path);
        if (!scene_file) throw std::runtime_error("cannot create temporary scene file");
        scene_file << body;
    }

    Scene scene;
    try {
        load_scene(scene_path.string(), scene);
    } catch (...) {
        std::filesystem::remove(scene_path);
        throw;
    }
    std::filesystem::remove(scene_path);
    return scene;
}

RenderOptions make_render_options(const std::map<std::string, std::string>& params,
                                  const ServerArgs& args,
                                  int& requested_threads) {
    RenderOptions options;
    requested_threads = query_int(params, "threads", args.default_threads);
    options.threads = apply_thread_limit(requested_threads, args);
    options.direct_only = query_bool(params, "direct_only", false);
    options.schedule = query_render_schedule(params);
    options.partial_update_interval = query_int(params, "partial_update_interval",
                                                query_int(params, "partial_update_rows", 0));
    if (options.partial_update_interval < 0) options.partial_update_interval = 0;
    options.sample_pass_batch = kRemoteSamplePassBatch;
    return options;
}

void apply_scene_overrides(Scene& scene, const std::map<std::string, std::string>& params) {
    int samples_override = query_int(params, "samples", -1);
    int max_depth_override = query_int(params, "max_depth", -1);
    if (samples_override > 0) scene.samples = samples_override;
    if (max_depth_override > 0) scene.max_depth = max_depth_override;
}

std::vector<unsigned char> render_scene_body(const std::string& body,
                                             const std::map<std::string, std::string>& params,
                                             const ServerArgs& args,
                                             const std::function<void(double)>& on_progress,
                                             const std::function<void(const RenderProgressInfo&)>& on_status,
                                             const std::function<void(const RenderOutput&, double)>& on_partial,
                                             const std::function<bool()>& should_cancel) {
    Scene scene = load_scene_from_body(body);
    apply_scene_overrides(scene, params);

    int requested_threads = 0;
    RenderOptions options = make_render_options(params, args, requested_threads);
    std::cerr << "INFO remote render image=" << scene.width << "x" << scene.height
              << " samples=" << scene.samples
              << " depth=" << scene.max_depth
              << " threads=" << resolve_thread_count(scene, options)
              << " requested_threads=" << requested_threads
              << " max_request_threads=" << args.max_request_threads
              << " schedule=" << render_schedule_name(options.schedule)
              << " sample_pass_batch=" << options.sample_pass_batch
              << " direct_only=" << (options.direct_only ? "true" : "false")
              << " primitives=" << scene.primitive_count << "\n";

    RenderCallbacks callbacks;
    callbacks.progress = [&](double progress) {
        double clamped = std::clamp(progress, 0.0, 1.0);
        int pct = static_cast<int>(clamped * 100.0);
        std::cerr << "PROGRESS " << pct << "%\n" << std::flush;
        if (on_progress) on_progress(clamped);
    };
    callbacks.status = on_status;
    callbacks.partial = on_partial;
    callbacks.should_cancel = should_cancel;

    RenderOutput output = render_scene(scene, options, callbacks);
    if (output.cancelled) throw std::runtime_error("render cancelled");
    return encode_rgba32f(output);
}

std::vector<unsigned char> render_request(const HttpRequest& request, const ServerArgs& args) {
    if (request.method != "POST" || request.path != "/render") {
        throw std::runtime_error("expected POST /render");
    }
    return render_scene_body(request.body,
                             parse_query(request.query),
                             args,
                             std::function<void(double)>(),
                             std::function<void(const RenderProgressInfo&)>(),
                             std::function<void(const RenderOutput&, double)>(),
                             std::function<bool()>());
}

std::string make_job_id() {
    auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    unsigned long long seq = g_next_job_id.fetch_add(1);
    return std::to_string(getpid()) + "-" + std::to_string(now) + "-" + std::to_string(seq);
}

std::shared_ptr<RenderJob> find_job(const std::string& id) {
    std::lock_guard<std::mutex> lock(g_jobs_mutex);
    auto it = g_jobs.find(id);
    return it == g_jobs.end() ? nullptr : it->second;
}

std::string job_status_json(const std::shared_ptr<RenderJob>& job) {
    std::lock_guard<std::mutex> lock(job->mutex);
    std::string status = "queued";
    if (job->failed) status = "error";
    else if (job->cancelled) status = "cancelled";
    else if (job->done) status = "done";
    else if (job->started) status = "rendering";

    std::ostringstream out;
    out << "{\"job_id\":\"" << json_escape(job->id) << "\","
        << "\"status\":\"" << status << "\","
        << "\"progress\":" << std::clamp(job->progress, 0.0, 1.0) << ","
        << "\"partial_seq\":" << job->partial_seq;
    if (job->has_render_status) {
        const RenderProgressInfo& info = job->render_status;
        out << ",\"render_schedule\":\"" << render_schedule_name(info.schedule) << "\""
            << ",\"samples_done\":" << info.samples_done
            << ",\"samples_total\":" << info.samples_total
            << ",\"rows_done\":" << info.rows_done
            << ",\"rows_total\":" << info.rows_total
            << ",\"pixels_done\":" << info.pixels_done
            << ",\"pixels_total\":" << info.pixels_total
            << ",\"elapsed_seconds\":" << info.elapsed_seconds
            << ",\"remaining_seconds\":" << info.remaining_seconds;
    }
    if (job->failed) {
        out << ",\"error\":\"" << json_escape(job->error) << "\"";
    }
    out << "}\n";
    return out.str();
}

void run_job(std::shared_ptr<RenderJob> job,
             std::string body,
             std::map<std::string, std::string> params,
             ServerArgs args) {
    {
        std::lock_guard<std::mutex> lock(job->mutex);
        job->started = true;
    }

    try {
        std::vector<unsigned char> result = render_scene_body(
            body,
            params,
            args,
            [job](double progress) {
                std::lock_guard<std::mutex> lock(job->mutex);
                job->progress = progress;
            },
            [job](const RenderProgressInfo& info) {
                std::lock_guard<std::mutex> lock(job->mutex);
                job->render_status = info;
                job->has_render_status = true;
                job->progress = std::max(job->progress, std::clamp(info.progress, 0.0, 1.0));
            },
            [job](const RenderOutput& partial, double progress) {
                try {
                    std::vector<unsigned char> bytes = encode_rgba32f(partial);
                    std::lock_guard<std::mutex> lock(job->mutex);
                    job->partial_result = std::move(bytes);
                    job->partial_seq += 1;
                    job->progress = std::max(job->progress, std::clamp(progress, 0.0, 1.0));
                } catch (const std::exception& e) {
                    std::cerr << "ERROR writing remote partial: " << e.what() << "\n";
                }
            },
            [job]() {
                return job->cancel_requested.load();
            });

        std::lock_guard<std::mutex> lock(job->mutex);
        job->result = std::move(result);
        job->progress = 1.0;
        job->done = true;
    } catch (const std::exception& e) {
        std::lock_guard<std::mutex> lock(job->mutex);
        if (job->cancel_requested.load()) {
            job->cancelled = true;
            job->progress = std::max(job->progress, 0.0);
        } else {
            job->failed = true;
            job->error = e.what();
        }
    }
}

std::string start_job(const HttpRequest& request, const ServerArgs& args) {
    if (request.body.empty()) throw std::runtime_error("empty render request body");

    auto job = std::make_shared<RenderJob>();
    job->id = make_job_id();
    {
        std::lock_guard<std::mutex> lock(g_jobs_mutex);
        g_jobs[job->id] = job;
    }

    std::map<std::string, std::string> params = parse_query(request.query);
    std::thread(run_job, job, request.body, params, args).detach();

    return "{\"job_id\":\"" + json_escape(job->id) + "\"}\n";
}

std::string job_id_from_path(const std::string& path, const std::string& suffix) {
    const std::string prefix = "/jobs/";
    if (path.rfind(prefix, 0) != 0) return "";
    if (path.size() <= prefix.size() + suffix.size()) return "";
    if (path.compare(path.size() - suffix.size(), suffix.size(), suffix) != 0) return "";
    return path.substr(prefix.size(), path.size() - prefix.size() - suffix.size());
}

int create_server_socket(const ServerArgs& args) {
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;

    addrinfo* result = nullptr;
    int rc = getaddrinfo(args.host.c_str(), args.port.c_str(), &hints, &result);
    if (rc != 0) {
        throw std::runtime_error(std::string("getaddrinfo failed: ") + gai_strerror(rc));
    }

    int listen_fd = -1;
    for (addrinfo* item = result; item != nullptr; item = item->ai_next) {
        listen_fd = socket(item->ai_family, item->ai_socktype, item->ai_protocol);
        if (listen_fd < 0) continue;

        int yes = 1;
        setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
        if (bind(listen_fd, item->ai_addr, item->ai_addrlen) == 0 &&
            listen(listen_fd, 16) == 0) {
            break;
        }
        close(listen_fd);
        listen_fd = -1;
    }

    freeaddrinfo(result);
    if (listen_fd < 0) throw std::runtime_error("cannot bind server socket");
    return listen_fd;
}

void handle_client(int client_fd, const ServerArgs& args) {
    try {
        HttpRequest request = read_request(client_fd, args.max_body_bytes);
        if (request.method == "GET" && request.path == "/health") {
            send_response(client_fd, 200, "OK", "application/json",
                          text_body("{\"status\":\"ok\",\"protocol\":1}\n"));
        } else if (request.method == "POST" && request.path == "/render") {
            std::vector<unsigned char> body = render_request(request, args);
            send_response(client_fd, 200, "OK", "application/octet-stream", body);
        } else if (request.method == "POST" && request.path == "/jobs") {
            std::string body = start_job(request, args);
            send_response(client_fd, 202, "Accepted", "application/json", text_body(body));
        } else if (request.method == "GET" && !job_id_from_path(request.path, "/progress").empty()) {
            std::string id = job_id_from_path(request.path, "/progress");
            std::shared_ptr<RenderJob> job = find_job(id);
            if (!job) {
                send_response(client_fd, 404, "Not Found", "text/plain", text_body("job not found\n"));
            } else {
                send_response(client_fd, 200, "OK", "application/json", text_body(job_status_json(job)));
            }
        } else if (request.method == "POST" && !job_id_from_path(request.path, "/cancel").empty()) {
            std::string id = job_id_from_path(request.path, "/cancel");
            std::shared_ptr<RenderJob> job = find_job(id);
            if (!job) {
                send_response(client_fd, 404, "Not Found", "text/plain", text_body("job not found\n"));
            } else {
                job->cancel_requested.store(true);
                send_response(client_fd, 202, "Accepted", "application/json", text_body(job_status_json(job)));
            }
        } else if (request.method == "GET" && !job_id_from_path(request.path, "/result").empty()) {
            std::string id = job_id_from_path(request.path, "/result");
            std::shared_ptr<RenderJob> job = find_job(id);
            if (!job) {
                send_response(client_fd, 404, "Not Found", "text/plain", text_body("job not found\n"));
            } else {
                std::vector<unsigned char> result;
                std::string status;
                std::string error;
                {
                    std::lock_guard<std::mutex> lock(job->mutex);
                    if (job->done) result = job->result;
                    else if (job->failed) error = job->error;
                    else if (job->cancelled) status = "cancelled";
                    else status = "rendering";
                }

                if (!result.empty()) {
                    send_response(client_fd, 200, "OK", "application/octet-stream", result);
                } else if (!error.empty()) {
                    send_response(client_fd, 500, "Internal Server Error", "text/plain",
                                  text_body("error: " + error + "\n"));
                } else if (status == "cancelled") {
                    send_response(client_fd, 409, "Conflict", "text/plain",
                                  text_body("job cancelled\n"));
                } else {
                    send_response(client_fd, 202, "Accepted", "application/json",
                                  text_body(job_status_json(job)));
                }
            }
        } else if (request.method == "GET" && !job_id_from_path(request.path, "/partial").empty()) {
            std::string id = job_id_from_path(request.path, "/partial");
            std::shared_ptr<RenderJob> job = find_job(id);
            if (!job) {
                send_response(client_fd, 404, "Not Found", "text/plain", text_body("job not found\n"));
            } else {
                std::vector<unsigned char> partial;
                std::string error;
                bool cancelled = false;
                {
                    std::lock_guard<std::mutex> lock(job->mutex);
                    partial = job->partial_result;
                    error = job->error;
                    cancelled = job->cancelled;
                }

                if (!partial.empty()) {
                    send_response(client_fd, 200, "OK", "application/octet-stream", partial);
                } else if (!error.empty()) {
                    send_response(client_fd, 500, "Internal Server Error", "text/plain",
                                  text_body("error: " + error + "\n"));
                } else if (cancelled) {
                    send_response(client_fd, 409, "Conflict", "text/plain",
                                  text_body("job cancelled\n"));
                } else {
                    send_response(client_fd, 202, "Accepted", "application/json",
                                  text_body(job_status_json(job)));
                }
            }
        } else {
            send_response(client_fd, 404, "Not Found", "text/plain",
                          text_body("not found\n"));
        }
    } catch (const std::exception& e) {
        std::string message = std::string("error: ") + e.what() + "\n";
        std::cerr << "ERROR " << e.what() << "\n";
        try {
            send_response(client_fd, 500, "Internal Server Error", "text/plain", text_body(message));
        } catch (...) {
        }
    }
    close(client_fd);
}

}  // namespace

int main(int argc, char* argv[]) {
    ServerArgs args;
    if (!parse_args(argc, argv, args)) {
        print_usage(argv[0]);
        return 1;
    }

    std::signal(SIGPIPE, SIG_IGN);

    try {
        int listen_fd = create_server_socket(args);
        std::cerr << "Raytracer server listening on " << args.host << ":" << args.port << "\n";

        while (true) {
            int client_fd = accept(listen_fd, nullptr, nullptr);
            if (client_fd < 0) {
                if (errno == EINTR) continue;
                throw std::runtime_error(std::string("accept failed: ") + std::strerror(errno));
            }
            if (args.once) {
                handle_client(client_fd, args);
                break;
            }
            std::thread(handle_client, client_fd, args).detach();
        }

        close(listen_fd);
    } catch (const std::exception& e) {
        std::cerr << "ERROR " << e.what() << "\n";
        return 1;
    }

    return 0;
}
