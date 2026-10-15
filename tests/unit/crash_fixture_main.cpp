// A tiny helper process for crash-consistency tests.
//
// It stores `count` objects and then calls std::_Exit, so no destructor, stream
// flush, or graceful close runs. The parent test reopens the same directory and
// verifies that every acknowledged write survived the abrupt termination and
// that no ID is handed out twice.
//
// Usage: minidb_crash_fixture <directory> <count> <next_id_reservation>

#include "minidb/storage.h"

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>

int main(int argc, char** argv) {
    if (argc != 4) {
        std::cerr << "usage: minidb_crash_fixture <directory> <count> <reservation>\n";
        return 2;
    }
    try {
        const std::filesystem::path directory = argv[1];
        const auto count = static_cast<std::uint64_t>(std::stoull(argv[2]));
        const auto reservation = static_cast<std::uint64_t>(std::stoull(argv[3]));
        minidb::ObjectSchema schema = {{"type", "object"},
                                       {"properties", minidb::Object::object()},
                                       {"additionalProperties", true}};
        minidb::SegmentedStorage storage(directory, "users", 100000, schema, {}, 0, reservation);
        static_cast<void>(storage.load());
        for (std::uint64_t id = 0; id < count; ++id) {
            storage.store(id, {{"name", "crash"}, {"n", id}}, id + 1);
        }
        // Simulate an abrupt process termination: skip every destructor.
        std::_Exit(0);
    } catch (const std::exception& error) {
        std::cerr << "minidb_crash_fixture: " << error.what() << '\n';
        return 1;
    }
    return 0;
}
