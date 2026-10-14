// benchmarks/concurrency_benchmark_main.cpp
//
// Concurrent-client stress benchmark for the MiniDB HTTP API.
//
// Unlike minidb_bench (one sequential client), this harness hammers the server
// from many client threads at the same time and sweeps the number of concurrent
// clients, so it characterises how throughput and request latency behave as
// load rises. It starts the real HTTP API in-process on an ephemeral loopback
// port, exactly like the sequential harness.
//
// Each phase runs for a fixed wall-clock duration with a fixed number of client
// threads, every thread issuing requests as fast as it can. Latencies of all
// requests in a phase are merged before computing percentiles.
//
// Client connections can be reused with --keep-alive: a connection is
// established up front and reused for the whole phase. Connection-per-request
// has a fixed per-request cost on loopback that has nothing to do with the
// database layer. Because http-lib caps how many connections its worker pool can
// hold at once, the sweep can also enlarge the pool with --worker-pool so the
// measurement reflects the database layer rather than that transport limit.
//
// Usage:
//   minidb_concurrency_bench [--objects N] [--seconds S] [--peak-seconds P]
//                            [--levels 1,2,4,...] [--port P] [--out PATH]
//
//   --objects       Objects created per configuration before the sweep (2000)
//   --seconds       Duration of each sweep phase in seconds (3)
//   --peak-seconds  Duration of the sustained peak phase in seconds (10)
//   --levels        Comma-separated client counts to sweep (1,2,4,8,16,32,64,128)
//   --port          Fixed port to bind; 0 (default) picks a free ephemeral port
//   --out           JSON output path (default benchmarks/results/concurrency.json)

#include "database_api.h"

#include <httplib.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <mutex>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

constexpr std::string_view resident_config = "resident";
constexpr std::string_view disk_config = "disk-backed";

// Physical operation a worker issues.
enum class Op { get, query, update, create };

std::string op_name(Op op) {
    switch (op) {
    case Op::get:
        return "get";
    case Op::query:
        return "query";
    case Op::update:
        return "update";
    case Op::create:
        return "create";
    }
    return "op";
}

// Mix of operations for a phase.
enum class Workload { read, create, mixed };

std::string workload_name(Workload workload) {
    switch (workload) {
    case Workload::read:
        return "read";
    case Workload::create:
        return "create";
    case Workload::mixed:
        return "mixed";
    }
    return "mixed";
}

// A JSON schema shared by both configurations so create/update payloads are
// identical and the comparison isolates the storage mode.
nlohmann::json benchmark_schema() {
    return {
        {"type", "object"},
        {"properties",
         {{"username", {{"type", "string"}}},
          {"email", {{"type", "string"}}},
          {"active", {{"type", "boolean"}}}}},
        {"required", {"username", "email"}}};
}

struct Options {
    std::size_t objects = 2000;
    double seconds = 3.0;
    double peak_seconds = 10.0;
    std::vector<std::size_t> levels{1, 2, 4, 8, 16, 32, 64, 128};
    int port = 0;
    bool keep_alive = false;
    std::size_t worker_pool = 0; // 0 = cpp-httplib default (max(8, hardware - 1))
    std::filesystem::path output = "benchmarks/results/concurrency.json";
};

std::size_t parse_size(std::string_view text, std::string_view name) {
    std::size_t value = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || end != text.data() + text.size()) {
        throw std::runtime_error("Invalid value for " + std::string(name) + ": " + std::string(text));
    }
    return value;
}

double parse_positive_double(std::string_view text, std::string_view name) {
    double value = 0.0;
    try {
        std::size_t consumed = 0;
        value = std::stod(std::string(text), &consumed);
        if (consumed != text.size()) {
            throw std::runtime_error("trailing characters");
        }
    } catch (const std::exception&) {
        throw std::runtime_error("Invalid value for " + std::string(name) + ": " + std::string(text));
    }
    if (!(value > 0.0)) {
        throw std::runtime_error(std::string(name) + " must be positive.");
    }
    return value;
}

std::vector<std::size_t> parse_levels(std::string_view text) {
    std::vector<std::size_t> levels;
    std::size_t begin = 0;
    while (begin <= text.size()) {
        const auto comma = text.find(',', begin);
        const auto end = comma == std::string_view::npos ? text.size() : comma;
        const auto token = text.substr(begin, end - begin);
        if (!token.empty()) {
            const auto level = parse_size(token, "--levels");
            if (level == 0) {
                throw std::runtime_error("--levels entries must be positive.");
            }
            levels.push_back(level);
        }
        if (comma == std::string_view::npos) {
            break;
        }
        begin = comma + 1;
    }
    if (levels.empty()) {
        throw std::runtime_error("--levels must contain at least one positive integer.");
    }
    std::sort(levels.begin(), levels.end());
    levels.erase(std::unique(levels.begin(), levels.end()), levels.end());
    return levels;
}

Options parse_options(int argc, char* argv[]) {
    Options options;
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument{argv[index]};
        const auto next = [&](std::string_view name) -> std::string_view {
            if (index + 1 >= argc) {
                throw std::runtime_error("Missing value for " + std::string(name));
            }
            return std::string_view{argv[++index]};
        };
        if (argument == "--objects") {
            options.objects = parse_size(next(argument), "--objects");
        } else if (argument == "--seconds") {
            options.seconds = parse_positive_double(next(argument), "--seconds");
        } else if (argument == "--peak-seconds") {
            options.peak_seconds = parse_positive_double(next(argument), "--peak-seconds");
        } else if (argument == "--levels") {
            options.levels = parse_levels(next(argument));
        } else if (argument == "--keep-alive") {
            options.keep_alive = true;
        } else if (argument == "--worker-pool") {
            options.worker_pool = parse_size(next(argument), "--worker-pool");
        } else if (argument == "--port") {
            options.port = static_cast<int>(parse_size(next(argument), "--port"));
        } else if (argument == "--out") {
            options.output = std::string(next(argument));
        } else if (argument == "--help") {
            std::cout << "Usage: minidb_concurrency_bench [--objects N] [--seconds S] "
                         "[--peak-seconds P] [--levels 1,2,4,...] [--keep-alive] "
                         "[--worker-pool N] [--port P] [--out PATH]\n";
            std::exit(0);
        } else {
            throw std::runtime_error("Unknown argument: " + std::string(argument));
        }
    }
    return options;
}

class TemporaryDirectory {
  public:
    TemporaryDirectory() {
        std::random_device random;
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        for (int attempt = 0; attempt < 10; ++attempt) {
            path_ = std::filesystem::temp_directory_path() /
                    ("minidb-concurrency-" + std::to_string(stamp) + "-" +
                     std::to_string(random()));
            if (std::filesystem::create_directory(path_)) {
                return;
            }
        }
        throw std::runtime_error("Could not create a unique benchmark directory.");
    }

    ~TemporaryDirectory() {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }

    TemporaryDirectory(const TemporaryDirectory&) = delete;
    TemporaryDirectory& operator=(const TemporaryDirectory&) = delete;

    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

  private:
    std::filesystem::path path_;
};

double percentile(const std::vector<double>& sorted, double percent) {
    if (sorted.empty()) {
        return 0.0;
    }
    if (sorted.size() == 1) {
        return sorted.front();
    }
    const double rank = percent / 100.0 * static_cast<double>(sorted.size() - 1);
    const auto low = static_cast<std::size_t>(std::floor(rank));
    const auto high = static_cast<std::size_t>(std::ceil(rank));
    const double weight = rank - static_cast<double>(low);
    return sorted[low] * (1.0 - weight) + sorted[high] * weight;
}

struct PhaseSample {
    std::string config;
    std::string workload;
    std::size_t concurrency = 0;
    bool sustained = false;
    std::size_t operations = 0;
    std::size_t failures = 0;
    double total_seconds = 0.0;
    double wall_seconds = 0.0;
    double throughput = 0.0;
    double mean_ms = 0.0;
    double p50_ms = 0.0;
    double p95_ms = 0.0;
    double p99_ms = 0.0;
};

struct WorkerStats {
    std::vector<double> latencies_us;
    std::size_t operations = 0;
    std::size_t failures = 0;
    std::vector<std::uint64_t> created;
};

nlohmann::json object_payload(std::size_t index, bool active) {
    const auto number = std::to_string(index);
    return {{"username", "user" + number},
            {"email", "user" + number + "@example.test"},
            {"active", active}};
}

bool ok_status(const httplib::Result& response, int expected) {
    return response && response->status == expected;
}

void ensure_database(httplib::Client& client, const std::string& name,
                     const nlohmann::json& indexed_fields, std::uint64_t cache_bytes) {
    nlohmann::json body = {{"name", name},
                           {"schema", benchmark_schema()},
                           {"maximum_objects_per_segment", 10000},
                           {"indexed_fields", indexed_fields},
                           {"cache_bytes", cache_bytes}};
    const auto response = client.Post("/databases", body.dump(), "application/json");
    if (!ok_status(response, 201)) {
        const std::string detail = response ? response->body : "no response";
        throw std::runtime_error("Could not create benchmark database '" + name + "': " + detail);
    }
}

std::vector<std::uint64_t> populate(httplib::Client& client, const std::string& db,
                                    std::size_t objects) {
    std::vector<std::uint64_t> ids;
    ids.reserve(objects);
    const std::string collection = "/databases/" + db;
    for (std::size_t index = 0; index < objects; ++index) {
        const auto response = client.Post(collection, object_payload(index, true).dump(),
                                          "application/json");
        if (!ok_status(response, 201)) {
            throw std::runtime_error("Could not populate benchmark database '" + db + "'.");
        }
        ids.push_back(nlohmann::json::parse(response->body)["id"].get<std::uint64_t>());
    }
    return ids;
}

std::unique_ptr<httplib::Client> make_client(int port, bool keep_alive) {
    auto client = std::make_unique<httplib::Client>("127.0.0.1", port);
    client->set_connection_timeout(5, 0);
    // A bounded per-request deadline keeps one overloaded request from hanging a
    // phase for tens of seconds; requests that miss it are reported as failures.
    client->set_read_timeout(5, 0);
    client->set_write_timeout(5, 0);
    client->set_keep_alive(keep_alive);
    return client;
}

// Runs one timed phase with `concurrency` client threads for `seconds` seconds
// and returns the aggregated sample. Objects created by a create/mixed phase are
// deleted again afterwards so the database returns to its baseline size.
PhaseSample run_phase(const std::string& config, Workload workload, std::size_t concurrency,
                      double seconds, bool sustained, bool warmup, int port, const std::string& db,
                      const Options& options, const std::vector<std::uint64_t>& pool,
                      std::uint64_t seed) {
    const std::string collection = "/databases/" + db;
    const std::string query_indexed = collection + "/query?limit=10";

    std::vector<std::unique_ptr<httplib::Client>> clients;
    clients.reserve(concurrency);
    for (std::size_t index = 0; index < concurrency; ++index) {
        clients.push_back(make_client(port, options.keep_alive));
    }

    std::vector<WorkerStats> stats(concurrency);
    std::mutex log_mutex;
    std::vector<std::string> failure_log;
    std::atomic<std::size_t> ready{0};
    std::atomic<bool> go{false};
    std::atomic<long long> deadline_ns{0};

    const auto now_ns = [] {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch())
            .count();
    };

    std::vector<std::thread> threads;
    threads.reserve(concurrency);
    for (std::size_t worker = 0; worker < concurrency; ++worker) {
        threads.emplace_back([&, worker] {
            auto& client = *clients[worker];
            auto& local = stats[worker];
            std::mt19937_64 random{seed + worker * 0x9E3779B97F4A7C15ull};
            std::uniform_int_distribution<std::size_t> pick(0, pool.size() - 1);
            std::uniform_int_distribution<int> roll(0, 99);
            std::size_t counter = 0;
            local.latencies_us.reserve(4096);

            // Establish the keep-alive connection before the timed window so
            // TCP setup (and the server's short listen backlog) does not pollute
            // the measurement.
            if (warmup) {
                client.Get("/health");
            }

            ready.fetch_add(1, std::memory_order_release);
            while (!go.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }

            while (now_ns() < deadline_ns.load(std::memory_order_relaxed)) {
                Op op = Op::get;
                if (workload == Workload::read) {
                    op = Op::get;
                } else if (workload == Workload::create) {
                    op = Op::create;
                } else {
                    const int value = roll(random);
                    if (value < 70) {
                        op = Op::get;
                    } else if (value < 85) {
                        op = Op::query;
                    } else if (value < 95) {
                        op = Op::update;
                    } else {
                        op = Op::create;
                    }
                }

                const auto start = Clock::now();
                httplib::Result result;
                bool ok = false;
                const auto index = pick(random);
                const auto id = pool[index];
                // Retry once on a transport error: the server closes keep-alive
                // connections after a fixed request count, and a client can
                // otherwise lose a request when it races that close.
                for (int attempt = 0; attempt < 2; ++attempt) {
                    switch (op) {
                    case Op::get:
                        result = client.Get((collection + "/" + std::to_string(id)).c_str());
                        ok = ok_status(result, 200);
                        break;
                    case Op::query: {
                        const auto body = nlohmann::json{{"field", "email"},
                                                         {"equals", "user" + std::to_string(index) +
                                                                        "@example.test"}}
                                              .dump();
                        result = client.Post(query_indexed, body, "application/json");
                        ok = ok_status(result, 200);
                        break;
                    }
                    case Op::update: {
                        const auto body = object_payload(index, counter % 2 == 0).dump();
                        result = client.Put((collection + "/" + std::to_string(id)).c_str(), body,
                                            "application/json");
                        ok = ok_status(result, 200);
                        break;
                    }
                    case Op::create: {
                        const auto create_index = options.objects + worker * 1'000'003u + counter;
                        result = client.Post(collection, object_payload(create_index, true).dump(),
                                             "application/json");
                        ok = ok_status(result, 201);
                        if (ok) {
                            try {
                                local.created.push_back(
                                    nlohmann::json::parse(result->body)["id"].get<std::uint64_t>());
                            } catch (const std::exception&) {
                                // The object was stored; a malformed body is not a failure.
                            }
                        }
                        break;
                    }
                    }
                    if (result) {
                        break; // Got an HTTP response (any status), no retry needed.
                    }
                }
                const auto end = Clock::now();
                const double microseconds =
                    std::chrono::duration<double, std::micro>(end - start).count();
                local.latencies_us.push_back(microseconds);

                ++counter;
                ++local.operations;
                if (!ok) {
                    ++local.failures;
                    std::lock_guard lock(log_mutex);
                    if (failure_log.size() < 8) {
                        std::string detail =
                            result ? "HTTP " + std::to_string(result->status) +
                                         " body=" + result->body.substr(0, 120)
                                   : "transport error " +
                                         std::to_string(static_cast<int>(result.error()));
                        failure_log.push_back(std::string(workload_name(workload)) + " " + op_name(op) +
                                              ": " + detail);
                    }
                }
            }
        });
    }

    while (ready.load(std::memory_order_acquire) < concurrency) {
        std::this_thread::yield();
    }
    const auto start = Clock::now();
    deadline_ns.store(now_ns() + static_cast<long long>(seconds * 1'000'000'000.0));
    go.store(true, std::memory_order_release);
    for (auto& thread : threads) {
        thread.join();
    }
    const auto end = Clock::now();

    for (const auto& entry : failure_log) {
        std::cerr << "    phase failure (" << config << " " << workload_name(workload) << " x"
                  << concurrency << "): " << entry << '\n';
    }

    PhaseSample sample;
    sample.config = config;
    sample.workload = workload_name(workload);
    sample.concurrency = concurrency;
    sample.sustained = sustained;

    std::vector<double> latencies;
    for (auto& worker_stats : stats) {
        sample.operations += worker_stats.operations;
        sample.failures += worker_stats.failures;
        latencies.insert(latencies.end(), worker_stats.latencies_us.begin(),
                         worker_stats.latencies_us.end());
    }

    sample.total_seconds = std::chrono::duration<double>(end - start).count();
    sample.wall_seconds = sample.total_seconds;
    // Throughput is measured over the intended window, not the join wall time:
    // a single straggler must not deflate the sustained rate of every other
    // client. The tail is reported through the latency percentiles instead.
    sample.throughput = seconds > 0.0 ? static_cast<double>(sample.operations) / seconds : 0.0;
    sample.total_seconds = seconds;

    std::sort(latencies.begin(), latencies.end());
    double sum_us = 0.0;
    for (const double value : latencies) {
        sum_us += value;
    }
    sample.mean_ms = latencies.empty() ? 0.0 : sum_us / static_cast<double>(latencies.size()) / 1000.0;
    sample.p50_ms = percentile(latencies, 50.0) / 1000.0;
    sample.p95_ms = percentile(latencies, 95.0) / 1000.0;
    sample.p99_ms = percentile(latencies, 99.0) / 1000.0;

    // Restore the baseline object count so later phases start from equal state.
    std::vector<std::uint64_t> created;
    for (auto& worker_stats : stats) {
        created.insert(created.end(), worker_stats.created.begin(), worker_stats.created.end());
    }
    if (!created.empty()) {
        auto cleanup = make_client(port, true);
        for (const auto id : created) {
            cleanup->Delete((collection + "/" + std::to_string(id)).c_str());
        }
    }

    return sample;
}

std::string timestamp_utc() {
    const std::time_t now = std::time(nullptr);
    std::tm utc{};
#if defined(_WIN32)
    gmtime_s(&utc, &now);
#else
    gmtime_r(&now, &utc);
#endif
    std::ostringstream stream;
    stream << std::put_time(&utc, "%Y-%m-%dT%H:%M:%SZ");
    return stream.str();
}

void print_table(const std::vector<PhaseSample>& samples) {
    std::cout << '\n';
    std::cout << std::left << std::setw(14) << "config" << std::setw(10) << "workload" << std::right
              << std::setw(6) << "conc" << std::setw(6) << "sust" << std::setw(10) << "ops"
              << std::setw(10) << "fail" << std::setw(12) << "ops/sec" << std::setw(10) << "mean ms"
              << std::setw(9) << "p50" << std::setw(9) << "p95" << std::setw(9) << "p99" << '\n';
    std::cout << std::string(108, '-') << '\n';
    for (const auto& sample : samples) {
        std::cout << std::left << std::setw(14) << sample.config << std::setw(10) << sample.workload
                  << std::right << std::setw(6) << sample.concurrency << std::setw(6)
                  << (sample.sustained ? "yes" : "-") << std::setw(10) << sample.operations
                  << std::setw(10) << sample.failures << std::setw(12) << std::fixed
                  << std::setprecision(1) << sample.throughput << std::setw(10)
                  << std::setprecision(3) << sample.mean_ms << std::setw(9) << std::setprecision(3)
                  << sample.p50_ms << std::setw(9) << sample.p95_ms << std::setw(9) << sample.p99_ms
                  << '\n';
    }
}

nlohmann::json to_json(const PhaseSample& sample) {
    return {{"config", sample.config},
            {"workload", sample.workload},
            {"concurrency", sample.concurrency},
            {"sustained", sample.sustained},
            {"operations", sample.operations},
            {"failures", sample.failures},
            {"total_seconds", sample.total_seconds},
            {"wall_seconds", sample.wall_seconds},
            {"throughput", sample.throughput},
            {"mean_ms", sample.mean_ms},
            {"p50_ms", sample.p50_ms},
            {"p95_ms", sample.p95_ms},
            {"p99_ms", sample.p99_ms}};
}

} // namespace

int main(int argc, char* argv[]) {
    try {
        const Options options = parse_options(argc, argv);
        TemporaryDirectory directory;

        DatabaseCatalog catalog{directory.path()};
        DatabaseApi api{catalog};
        httplib::Server server;
        api.register_routes(server);

        const int port = options.port > 0
                             ? (server.bind_to_port("127.0.0.1", options.port) ? options.port : -1)
                             : server.bind_to_any_port("127.0.0.1");
        if (port <= 0) {
            throw std::runtime_error("Could not bind the benchmark server to a loopback port.");
        }

        std::thread listener{[&server] { server.listen_after_bind(); }};
        auto client = make_client(port, true);

        bool ready = false;
        for (int attempt = 0; attempt < 200; ++attempt) {
            if (ok_status(client->Get("/health"), 200)) {
                ready = true;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        if (!ready) {
            server.stop();
            listener.join();
            throw std::runtime_error("Benchmark server did not become ready.");
        }

        const unsigned hardware = std::thread::hardware_concurrency();
        const std::size_t default_pool =
            std::max<std::size_t>(8, hardware > 0 ? hardware - 1 : 0);
        const std::size_t worker_pool = options.worker_pool > 0 ? options.worker_pool : default_pool;

        if (options.worker_pool > 0) {
            // Size the server's worker pool explicitly. http-lib's default caps
            // concurrent connections at max(8, hardware - 1); this lets the
            // sweep isolate the database layer from that transport limit.
            const auto pool_size = options.worker_pool;
            server.new_task_queue = [pool_size] { return new httplib::ThreadPool(pool_size); };
        }

        std::cout << "MiniDB concurrency stress harness\n";
        std::cout << "  server:   http://127.0.0.1:" << port << " (ephemeral free port)\n";
        std::cout << "  hardware: " << hardware << " threads, server worker pool " << worker_pool;
        if (options.worker_pool > 0) {
            std::cout << " (explicit; default is " << default_pool << ")";
        }
        std::cout << '\n';
        std::cout << "  objects:  " << options.objects << " per configuration\n";
        std::cout << "  levels:   ";
        for (std::size_t index = 0; index < options.levels.size(); ++index) {
            std::cout << (index == 0 ? "" : ",") << options.levels[index];
        }
        std::cout << " concurrent clients (" << options.seconds << "s each)\n";
        std::cout << "  peak:     mixed x" << options.levels.back() << " for "
                  << options.peak_seconds << "s\n";
        std::cout << "  transport: " << (options.keep_alive ? "keep-alive" : "connection-per-request")
                  << '\n';
        if (options.keep_alive) {
            std::cout << "  warmup:   connections pre-established before timing\n";
        }

        ensure_database(*client, "plain", nlohmann::json::array(), 0);
        ensure_database(*client, "disk", nlohmann::json::array({"email"}), 8 * 1024 * 1024);

        std::vector<PhaseSample> samples;
        const std::vector<std::pair<std::string, std::string>> configurations{
            {std::string(resident_config), "plain"}, {std::string(disk_config), "disk"}};
        const std::vector<Workload> workloads{Workload::read, Workload::create, Workload::mixed};

        // Pre-establishing a keep-alive connection per client only works while
        // the worker pool can hold every client; otherwise the setup would have
        // to wait on the keep-alive idle timer.
        const auto warmup_for = [&](std::size_t level) {
            return options.keep_alive && level <= worker_pool;
        };

        std::uint64_t seed = 20261014;
        for (const auto& [config, db] : configurations) {
            std::cout << "\nPopulating '" << config << "' configuration...\n";
            const auto pool = populate(*client, db, options.objects);

            for (const auto workload : workloads) {
                for (const auto level : options.levels) {
                    std::cout << "  " << config << " " << workload_name(workload) << " x" << level
                              << " ..." << std::flush;
                    auto sample = run_phase(config, workload, level, options.seconds, false,
                                            warmup_for(level), port, db, options, pool, seed);
                    std::cout << " " << std::fixed << std::setprecision(1) << sample.throughput
                              << " ops/s, p95 " << std::setprecision(3) << sample.p95_ms << " ms";
                    if (sample.failures != 0) {
                        std::cout << ", " << sample.failures << " failures";
                    }
                    std::cout << '\n';
                    samples.push_back(std::move(sample));
                    seed += 1000;
                }
            }

            std::cout << "  " << config << " mixed sustained peak x" << options.levels.back()
                      << " ..." << std::flush;
            auto peak = run_phase(config, Workload::mixed, options.levels.back(), options.peak_seconds,
                                  true, warmup_for(options.levels.back()), port, db, options, pool,
                                  seed);
            std::cout << " " << std::fixed << std::setprecision(1) << peak.throughput << " ops/s, p95 "
                      << std::setprecision(3) << peak.p95_ms << " ms";
            if (peak.failures != 0) {
                std::cout << ", " << peak.failures << " failures";
            }
            std::cout << '\n';
            samples.push_back(std::move(peak));
            seed += 1000;
        }

        server.stop();
        listener.join();

        std::size_t failures = 0;
        for (const auto& sample : samples) {
            failures += sample.failures;
        }

        print_table(samples);

        nlohmann::json output;
        output["generated_at"] = timestamp_utc();
        output["port"] = port;
        output["hardware_concurrency"] = hardware;
        output["server_thread_pool"] = worker_pool;
        output["options"] = {{"objects", options.objects},
                             {"seconds", options.seconds},
                             {"peak_seconds", options.peak_seconds},
                             {"levels", options.levels}};
        output["samples"] = nlohmann::json::array();
        for (const auto& sample : samples) {
            output["samples"].push_back(to_json(sample));
        }

        const auto output_path = options.output;
        if (output_path.has_parent_path()) {
            std::filesystem::create_directories(output_path.parent_path());
        }
        std::ofstream file(output_path);
        if (!file) {
            throw std::runtime_error("Could not write " + output_path.string());
        }
        file << output.dump(2) << '\n';

        std::cout << "\nWrote " << output_path.string() << '\n';
        std::cout << "Failures: " << failures << '\n';
        return failures == 0 ? 0 : 1;
    } catch (const std::exception& error) {
        std::cerr << "minidb_concurrency_bench: " << error.what() << '\n';
        return 1;
    }
}
