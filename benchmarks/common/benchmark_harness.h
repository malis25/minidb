#pragma once

// benchmarks/common/benchmark_harness.h
//
// Reusable plumbing shared by the HTTP-driving MiniDB benchmarks.
//
// minidb_bench (one sequential client) and minidb_concurrency_bench (many
// concurrent clients) both start the real HTTP API in-process on an ephemeral
// loopback port, drive it with an httplib client and serialise JSON results
// into benchmarks/results/. This header keeps that plumbing in one place so
// each scenario file only describes what it measures.

#include "database_api.h"

#include <httplib.h>
#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace benchmark {

// Monotonic clock used for every latency and throughput measurement.
using Clock = std::chrono::steady_clock;

// --- request payloads -------------------------------------------------------

// JSON schema shared by the HTTP benchmarks so create/update payloads are
// identical across storage configurations and the comparison isolates storage.
[[nodiscard]] nlohmann::json schema();

// A `{"username","email","active"}` document matching `schema()`.
[[nodiscard]] nlohmann::json object_payload(std::size_t index, bool active);

// True when `response` holds an HTTP response with exactly `expected` status.
[[nodiscard]] bool ok_status(const httplib::Result& response, int expected);

// Creates a database with the benchmark schema; throws when the request fails.
void ensure_database(httplib::Client& client, const std::string& name,
                     const nlohmann::json& indexed_fields, std::uint64_t cache_bytes);

// --- timing -----------------------------------------------------------------

// Linearly interpolated percentile of an already sorted sample vector.
[[nodiscard]] double percentile(const std::vector<double>& sorted, double percent);

// One measured operation group. The field names mirror the result JSON exactly
// so the plot scripts keep working unchanged.
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
// Sample. Batch operations record an amortized per-operation latency so their
// numbers stay comparable with single-request operations.
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

    // Records an already-measured duration that covered `operations`
    // operations. The per-operation latency is amortized, but throughput uses
    // the real wall time so batch numbers are not inflated.
    void add(long double microseconds, std::size_t operations, bool ok);

    [[nodiscard]] Sample finalize(std::string config, std::string operation) const;

  private:
    std::vector<double> latencies_;
    double raw_total_us_ = 0.0;
    std::size_t operations_ = 0;
    std::size_t failures_ = 0;
};

[[nodiscard]] nlohmann::json to_json(const Sample& sample);
void print_table(const std::vector<Sample>& samples);

// --- processes and files ----------------------------------------------------

// Creates a uniquely named directory under the system temp directory and removes
// it (and its contents) on destruction.
class TemporaryDirectory {
  public:
    explicit TemporaryDirectory(std::string prefix);

    ~TemporaryDirectory();

    TemporaryDirectory(const TemporaryDirectory&) = delete;
    TemporaryDirectory& operator=(const TemporaryDirectory&) = delete;

    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

  private:
    std::filesystem::path path_;
};

// Serialises `document` (pretty printed, trailing newline), creating parent
// directories first. Throws when the file cannot be written.
void write_json(const std::filesystem::path& path, const nlohmann::json& document);

// UTC timestamp in the `%Y-%m-%dT%H:%M:%SZ` form used in every result file.
[[nodiscard]] std::string timestamp_utc();

// Parses a non-negative integer argument; throws with `name` on malformed input.
[[nodiscard]] std::size_t parse_size(std::string_view text, std::string_view name);

// --- HTTP server and clients ------------------------------------------------

// Timeouts applied to every client connected to a benchmark server.
struct ClientSettings {
    bool keep_alive = true;
    int connection_timeout_seconds = 5;
    int read_timeout_seconds = 30;
    int write_timeout_seconds = 0; // 0 means no write timeout
};

// A client connected to a loopback benchmark server.
[[nodiscard]] std::unique_ptr<httplib::Client> make_client(int port,
                                                          const ClientSettings& settings = {});

// Starts the real HTTP API in-process on an ephemeral loopback port, waits until
// it answers /health, and stops it again on destruction.
class HttpServer {
  public:
    // `requested_port` of 0 picks a free ephemeral port; `directory_prefix`
    // only names the throw-away storage directory.
    explicit HttpServer(int requested_port = 0, std::string directory_prefix = "minidb-bench-");
    ~HttpServer();

    HttpServer(const HttpServer&) = delete;
    HttpServer& operator=(const HttpServer&) = delete;

    [[nodiscard]] int port() const noexcept { return port_; }

    // The worker-pool size http-lib uses when none is configured explicitly.
    [[nodiscard]] static std::size_t default_worker_pool();

    // Replaces the server's worker-pool factory. Kept separate from the
    // constructor because the harness historically configures it after the
    // listener has started.
    void set_worker_pool(std::size_t workers);

  private:
    TemporaryDirectory directory_;
    DatabaseCatalog catalog_;
    DatabaseApi api_;
    httplib::Server server_;
    int port_ = 0;
    std::thread listener_;
};

} // namespace benchmark
