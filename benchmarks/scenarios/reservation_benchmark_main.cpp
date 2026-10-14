// benchmarks/reservation_benchmark_main.cpp
//
// Measures how the next-ID reservation block size affects write throughput.
//
// Creating an object advances the database's next ID. Historically the manifest
// was rewritten (and forced to disk) on every store to persist that ID. Reserving
// IDs in blocks means the manifest is only rewritten once per block, at the cost
// of skipping the unused remainder of a block after an unclean stop. This tool
// sweeps block sizes so the trade-off can be measured instead of guessed.
//
// Usage:
//   minidb_write_bench [--objects N] [--repeats R] [--sizes 1,2,4,...] [--out PATH]
//
//   --objects  Objects stored per measured run (default 4000)
//   --repeats  Runs per size; the fastest is reported (default 3)
//   --sizes    Comma-separated reservation sizes (default geometric sweep)
//   --out      JSON output path (default benchmarks/results/reservation-sweep.json)

#include "minidb/storage.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

struct Options {
    std::uint64_t objects = 4000;
    std::uint64_t repeats = 3;
    std::vector<std::uint64_t> sizes{1, 2, 4, 8, 16, 32, 64, 128, 256, 512, 1024, 2048, 4096};
    std::filesystem::path output = "benchmarks/results/reservation-sweep.json";
};

std::uint64_t parse_unsigned(std::string_view text, std::string_view name) {
    std::uint64_t value = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || end != text.data() + text.size()) {
        throw std::runtime_error("Invalid value for " + std::string(name) + ": " + std::string(text));
    }
    return value;
}

std::vector<std::uint64_t> parse_sizes(std::string_view text) {
    std::vector<std::uint64_t> sizes;
    std::size_t start = 0;
    while (start <= text.size()) {
        const auto comma = text.find(',', start);
        const auto end = comma == std::string_view::npos ? text.size() : comma;
        const auto piece = text.substr(start, end - start);
        if (!piece.empty()) {
            sizes.push_back(parse_unsigned(piece, "--sizes"));
        }
        if (comma == std::string_view::npos) {
            break;
        }
        start = comma + 1;
    }
    if (sizes.empty()) {
        throw std::runtime_error("--sizes must list at least one reservation size.");
    }
    return sizes;
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
            options.objects = parse_unsigned(next(argument), "--objects");
        } else if (argument == "--repeats") {
            options.repeats = parse_unsigned(next(argument), "--repeats");
        } else if (argument == "--sizes") {
            options.sizes = parse_sizes(next(argument));
        } else if (argument == "--out") {
            options.output = std::string(next(argument));
        } else if (argument == "--help") {
            std::cout << "Usage: minidb_write_bench [--objects N] [--repeats R] "
                         "[--sizes 1,2,4,...] [--out PATH]\n";
            std::exit(0);
        } else {
            throw std::runtime_error("Unknown argument: " + std::string(argument));
        }
    }
    if (options.objects == 0 || options.repeats == 0) {
        throw std::runtime_error("--objects and --repeats must be positive.");
    }
    return options;
}

class TemporaryDirectory {
  public:
    TemporaryDirectory() {
        std::random_device random;
        const auto stamp = Clock::now().time_since_epoch().count();
        for (int attempt = 0; attempt < 10; ++attempt) {
            path_ = std::filesystem::temp_directory_path() /
                    ("minidb-write-bench-" + std::to_string(stamp) + "-" +
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

minidb::ObjectSchema default_object_schema() {
    return {{"type", "object"},
            {"properties", minidb::Object::object()},
            {"additionalProperties", true}};
}

struct RunResult {
    std::uint64_t reservation = 0;
    std::uint64_t objects = 0;
    std::uint64_t manifest_writes = 0;
    double seconds = 0.0;
    double throughput = 0.0;
};

// Stores `options.objects` objects once and reports the best of `repeats` runs.
RunResult measure(std::uint64_t reservation, const Options& options) {
    RunResult best;
    best.reservation = reservation;
    best.objects = options.objects;
    best.throughput = 0.0;
    for (std::uint64_t repeat = 0; repeat < options.repeats; ++repeat) {
        TemporaryDirectory directory;
        minidb::SegmentedStorage storage{directory.path(), "bench", 1'000'000,
                                         default_object_schema(), {}, 0, reservation};
        static_cast<void>(storage.load());
        const auto baseline_writes = storage.manifest_writes();

        const auto start = Clock::now();
        for (std::uint64_t id = 0; id < options.objects; ++id) {
            storage.store(id, {{"name", "object"}, {"n", id}}, id + 1);
        }
        const auto end = Clock::now();
        const auto seconds = std::chrono::duration<double>(end - start).count();
        const auto throughput = static_cast<double>(options.objects) / seconds;
        if (throughput > best.throughput) {
            best.throughput = throughput;
            best.seconds = seconds;
            best.manifest_writes = storage.manifest_writes() - baseline_writes;
        }
    }
    return best;
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

} // namespace

int main(int argc, char* argv[]) {
    try {
        const Options options = parse_options(argc, argv);
        std::cout << "MiniDB next-ID reservation sweep\n";
        std::cout << "  objects/run: " << options.objects << ", repeats: " << options.repeats << '\n';
        std::cout << "  default reservation: " << minidb::default_next_id_reservation << '\n';

        std::vector<RunResult> results;
        results.reserve(options.sizes.size());
        std::cout << '\n' << std::left << std::setw(14) << "reservation" << std::right
                  << std::setw(12) << "ops/sec" << std::setw(12) << "manifest" << '\n';
        std::cout << std::string(38, '-') << '\n';
        for (const auto size : options.sizes) {
            const auto result = measure(size, options);
            std::cout << std::left << std::setw(14) << size << std::right << std::setw(12)
                      << std::fixed << std::setprecision(0) << result.throughput << std::setw(12)
                      << result.manifest_writes << '\n';
            results.push_back(result);
        }

        nlohmann::json output;
        output["generated_at"] = timestamp_utc();
        output["objects"] = options.objects;
        output["repeats"] = options.repeats;
        output["default_reservation"] = minidb::default_next_id_reservation;
        output["results"] = nlohmann::json::array();
        for (const auto& result : results) {
            output["results"].push_back({{"reservation", result.reservation},
                                         {"objects", result.objects},
                                         {"manifest_writes", result.manifest_writes},
                                         {"seconds", result.seconds},
                                         {"throughput", result.throughput}});
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
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "minidb_write_bench: " << error.what() << '\n';
        return 1;
    }
}
