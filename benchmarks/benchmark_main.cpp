// benchmarks/benchmark_main.cpp
//
// HTTP benchmark harness for MiniDB.
//
// The harness starts the real HTTP API in-process on an ephemeral loopback port
// (bind_to_any_port), so it never collides with a server that may already be
// using the default 8080 or any other port. It then drives two storage
// configurations through the same set of operations and records throughput and
// latency percentiles.
//
// Usage:
//   minidb_bench [--objects N] [--queries N] [--batches N] [--batch-size B]
//                [--port P] [--out PATH]
//
//   --objects     Objects created per configuration (default 2000)
//   --queries     Read/query/update/delete iterations (default 2000)
//   --batches     Number of batches executed (default 20)
//   --batch-size  Creates inside each batch (default 100)
//   --port        Fixed port to bind; 0 (default) picks a free ephemeral port
//   --out         JSON output path (default benchmarks/results/benchmark.json)

#include "database_api.h"

#include <httplib.h>
#include <nlohmann/json.hpp>

#include <algorithm>
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
    std::size_t queries = 2000;
    std::size_t batches = 20;
    std::size_t batch_size = 100;
    int port = 0;
    std::filesystem::path output = "benchmarks/results/benchmark.json";
};

std::size_t parse_size(std::string_view text, std::string_view name) {
    std::size_t value = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || end != text.data() + text.size()) {
        throw std::runtime_error("Invalid value for " + std::string(name) + ": " + std::string(text));
    }
    return value;
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
        } else if (argument == "--queries") {
            options.queries = parse_size(next(argument), "--queries");
        } else if (argument == "--batches") {
            options.batches = parse_size(next(argument), "--batches");
        } else if (argument == "--batch-size") {
            options.batch_size = parse_size(next(argument), "--batch-size");
        } else if (argument == "--port") {
            options.port = static_cast<int>(parse_size(next(argument), "--port"));
        } else if (argument == "--out") {
            options.output = std::string(next(argument));
        } else if (argument == "--help") {
            std::cout << "Usage: minidb_bench [--objects N] [--queries N] [--batches N] "
                         "[--batch-size B] [--port P] [--out PATH]\n";
            std::exit(0);
        } else {
            throw std::runtime_error("Unknown argument: " + std::string(argument));
        }
    }
    if (options.batch_size == 0) {
        throw std::runtime_error("--batch-size must be positive.");
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
                    ("minidb-bench-" + std::to_string(stamp) + "-" +
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

struct Sample {
    std::string config;
    std::string operation;
    std::size_t operations = 0;
    std::size_t failures = 0;
    double total_seconds = 0.0;
    double throughput = 0.0;
    double mean_ms = 0.0;
    double p50_ms = 0.0;
    double p95_ms = 0.0;
    double p99_ms = 0.0;
};

// Collects per-operation latencies (microseconds) and converts them into a
// summary sample. Batch operations record an amortized per-operation latency so
// their numbers stay comparable with single-request operations.
class Aggregator {
  public:
    template <typename Fn>
    void run(std::size_t count, Fn&& operation) {
        latencies_.reserve(latencies_.size() + count);
        for (std::size_t index = 0; index < count; ++index) {
            const auto start = Clock::now();
            const bool ok = operation(index);
            const auto end = Clock::now();
            const double microseconds = std::chrono::duration<double, std::micro>(end - start).count();
            latencies_.push_back(microseconds);
            raw_total_us_ += microseconds;
            ++operations_;
            if (!ok) {
                ++failures_;
            }
        }
    }

    // Records an already-measured duration that covered `operations` operations.
    // The per-operation latency is amortized, but throughput uses the real wall
    // time so batch numbers are not inflated.
    void add(long double microseconds, std::size_t operations, bool ok) {
        const double value = static_cast<double>(microseconds);
        latencies_.push_back(value / static_cast<double>(operations));
        raw_total_us_ += value;
        operations_ += operations;
        if (!ok) {
            ++failures_;
        }
    }

    [[nodiscard]] Sample finalize(std::string config, std::string operation) const {
        Sample sample;
        sample.config = std::move(config);
        sample.operation = std::move(operation);
        sample.operations = operations_;
        sample.failures = failures_;
        sample.total_seconds = raw_total_us_ / 1'000'000.0;
        sample.throughput =
            sample.total_seconds > 0.0 ? static_cast<double>(operations_) / sample.total_seconds : 0.0;
        auto sorted = latencies_;
        std::sort(sorted.begin(), sorted.end());
        sample.mean_ms = operations_ == 0 ? 0.0 : sample.total_seconds * 1000.0 / static_cast<double>(operations_);
        sample.p50_ms = percentile(sorted, 50.0) / 1000.0;
        sample.p95_ms = percentile(sorted, 95.0) / 1000.0;
        sample.p99_ms = percentile(sorted, 99.0) / 1000.0;
        return sample;
    }

  private:
    std::vector<double> latencies_;
    double raw_total_us_ = 0.0;
    std::size_t operations_ = 0;
    std::size_t failures_ = 0;
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

std::vector<Sample> run_configuration(httplib::Client& client, const std::string& db,
                                      const std::string& config, const Options& options,
                                      std::mt19937_64& random) {
    std::vector<Sample> samples;
    const std::string collection = "/databases/" + db;
    const std::string query_indexed = collection + "/query?limit=10";
    const std::string batch_endpoint = collection + "/batch";

    std::vector<std::uint64_t> ids;
    ids.reserve(options.objects);

    auto random_id = [&]() -> std::uint64_t {
        std::uniform_int_distribution<std::size_t> distribution(0, ids.size() - 1);
        return ids[distribution(random)];
    };

    // 1. create
    {
        Aggregator aggregate;
        std::size_t logged = 0;
        aggregate.run(options.objects, [&](std::size_t index) {
            const auto response = client.Post(collection, object_payload(index, true).dump(),
                                              "application/json");
            if (!ok_status(response, 201)) {
                if (logged < 5) {
                    ++logged;
                    if (!response) {
                        std::cerr << "create failure: no response\n";
                    } else {
                        std::cerr << "create failure: status " << response->status << " body "
                                  << response->body << '\n';
                    }
                }
                return false;
            }
            ids.push_back(nlohmann::json::parse(response->body)["id"].get<std::uint64_t>());
            return true;
        });
        samples.push_back(aggregate.finalize(config, "create"));
    }

    // 2. read by id
    if (!ids.empty()) {
        Aggregator aggregate;
        aggregate.run(options.queries, [&](std::size_t) {
            const auto response = client.Get((collection + "/" + std::to_string(random_id())).c_str());
            return ok_status(response, 200);
        });
        samples.push_back(aggregate.finalize(config, "read (by id)"));
    }

    // 3. query on an indexed field (email is a B+ tree index in the disk config)
    if (!ids.empty()) {
        Aggregator aggregate;
        aggregate.run(options.queries, [&](std::size_t) {
            const auto index = std::uniform_int_distribution<std::size_t>(0, ids.size() - 1)(random);
            const auto body = nlohmann::json{{"field", "email"},
                                             {"equals", "user" + std::to_string(index) +
                                                            "@example.test"}}
                                  .dump();
            const auto response = client.Post(query_indexed, body, "application/json");
            return ok_status(response, 200);
        });
        samples.push_back(aggregate.finalize(config, "query (indexed email)"));
    }

    // 4. query on a field that is only hash-indexed in memory but scanned on disk
    if (!ids.empty()) {
        Aggregator aggregate;
        aggregate.run(options.queries, [&](std::size_t) {
            const auto index = std::uniform_int_distribution<std::size_t>(0, ids.size() - 1)(random);
            const auto body =
                nlohmann::json{{"field", "username"}, {"equals", "user" + std::to_string(index)}}.dump();
            const auto response = client.Post(query_indexed, body, "application/json");
            return ok_status(response, 200);
        });
        samples.push_back(aggregate.finalize(config, "query (username)"));
    }

    // 5. update
    if (!ids.empty()) {
        Aggregator aggregate;
        aggregate.run(options.queries, [&](std::size_t iteration) {
            const auto id = random_id();
            const auto body = object_payload(static_cast<std::size_t>(id), iteration % 2 == 0).dump();
            const auto response =
                client.Put((collection + "/" + std::to_string(id)).c_str(), body, "application/json");
            return ok_status(response, 200);
        });
        samples.push_back(aggregate.finalize(config, "update"));
    }

    // 6. batch of creates
    {
        Aggregator aggregate;
        std::size_t created = 0;
        for (std::size_t batch = 0; batch < options.batches; ++batch) {
            auto operations = nlohmann::json::array();
            for (std::size_t offset = 0; offset < options.batch_size; ++offset) {
                operations.push_back(
                    {{"type", "create"},
                     {"object", object_payload(options.objects + created, true)}});
                ++created;
            }
            const nlohmann::json body = {{"operations", operations}};
            const auto start = Clock::now();
            const auto response = client.Post(batch_endpoint, body.dump(), "application/json");
            const auto end = Clock::now();
            const auto microseconds =
                std::chrono::duration<long double, std::micro>(end - start).count();
            aggregate.add(microseconds, options.batch_size, ok_status(response, 200));
        }
        samples.push_back(aggregate.finalize(config, "batch x" + std::to_string(options.batch_size)));
    }

    // 7. delete (each ID is removed at most once, so 404s are real failures)
    if (!ids.empty()) {
        auto shuffled = ids;
        std::shuffle(shuffled.begin(), shuffled.end(), random);
        const std::size_t count = std::min<std::size_t>(options.queries, shuffled.size());
        Aggregator aggregate;
        aggregate.run(count, [&](std::size_t index) {
            const auto response =
                client.Delete((collection + "/" + std::to_string(shuffled[index])).c_str());
            return ok_status(response, 200);
        });
        samples.push_back(aggregate.finalize(config, "delete"));
    }

    return samples;
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

void print_table(const std::vector<Sample>& samples) {
    std::cout << '\n';
    std::cout << std::left << std::setw(14) << "config" << std::setw(22) << "operation"
              << std::right << std::setw(10) << "ops" << std::setw(12) << "ops/sec"
              << std::setw(10) << "mean ms" << std::setw(9) << "p50" << std::setw(9) << "p95"
              << std::setw(9) << "p99" << '\n';
    std::cout << std::string(95, '-') << '\n';
    for (const auto& sample : samples) {
        std::cout << std::left << std::setw(14) << sample.config << std::setw(22) << sample.operation
                  << std::right << std::setw(10) << sample.operations << std::setw(12) << std::fixed
                  << std::setprecision(1) << sample.throughput << std::setw(10) << std::setprecision(3)
                  << sample.mean_ms << std::setw(9) << std::setprecision(3) << sample.p50_ms
                  << std::setw(9) << sample.p95_ms << std::setw(9) << sample.p99_ms << '\n';
    }
}

nlohmann::json to_json(const Sample& sample) {
    return {{"config", sample.config},
            {"operation", sample.operation},
            {"operations", sample.operations},
            {"failures", sample.failures},
            {"total_seconds", sample.total_seconds},
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

        const int port = options.port > 0 ? (server.bind_to_port("127.0.0.1", options.port)
                                                 ? options.port
                                                 : -1)
                                          : server.bind_to_any_port("127.0.0.1");
        if (port <= 0) {
            throw std::runtime_error("Could not bind the benchmark server to a loopback port.");
        }

        std::thread listener{[&server] { server.listen_after_bind(); }};
        httplib::Client client{"127.0.0.1", port};
        client.set_connection_timeout(5, 0);
        client.set_read_timeout(30, 0);
        client.set_keep_alive(true);

        bool ready = false;
        for (int attempt = 0; attempt < 200; ++attempt) {
            if (ok_status(client.Get("/health"), 200)) {
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

        std::cout << "MiniDB benchmark harness\n";
        std::cout << "  server:  http://127.0.0.1:" << port << " (ephemeral free port)\n";
        std::cout << "  objects: " << options.objects << ", queries: " << options.queries
                  << ", batches: " << options.batches << " x " << options.batch_size << '\n';

        ensure_database(client, "plain", nlohmann::json::array(), 0);
        ensure_database(client, "disk", nlohmann::json::array({"email"}), 8 * 1024 * 1024);

        std::mt19937_64 random{20261014};
        std::vector<Sample> samples;
        std::cout << "\nRunning '" << resident_config << "' configuration...\n";
        {
            auto produced = run_configuration(client, "plain", std::string(resident_config), options, random);
            samples.insert(samples.end(), produced.begin(), produced.end());
        }
        std::cout << "Running '" << disk_config << "' configuration...\n";
        {
            auto produced = run_configuration(client, "disk", std::string(disk_config), options, random);
            samples.insert(samples.end(), produced.begin(), produced.end());
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
        output["options"] = {{"objects", options.objects},
                             {"queries", options.queries},
                             {"batches", options.batches},
                             {"batch_size", options.batch_size}};
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
        std::cerr << "minidb_bench: " << error.what() << '\n';
        return 1;
    }
}
