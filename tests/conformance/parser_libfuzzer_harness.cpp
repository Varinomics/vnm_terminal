#include "vnm_terminal/internal/terminal_screen_model.h"
#include "conformance_fixture_io.h"

#include <QByteArray>
#include <QByteArrayView>
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <limits>

namespace fs = std::filesystem;
namespace term = vnm_terminal::internal;
namespace fixture = vnm_terminal::tests::conformance;

namespace {

void exercise_parser(const std::uint8_t* data, std::size_t size)
{
    if (data == nullptr || size == 0U || size > 1024U * 1024U) {
        return;
    }

    const QByteArray bytes(
        reinterpret_cast<const char*>(data),
        static_cast<int>(std::min<std::size_t>(
            size,
            static_cast<std::size_t>(std::numeric_limits<int>::max()))));

    term::Terminal_screen_model model(
        {
            term::terminal_grid_size_t{24, 80},
            1000,
            8,
            true,
            });

    std::size_t offset = 0U;
    while (offset < size) {
        const std::size_t remaining = size - offset;
        const std::size_t chunk_size = std::min<std::size_t>(
            remaining,
            1U + ((offset * 131U) % 4096U));
        model.ingest(QByteArrayView(bytes).sliced(
            static_cast<qsizetype>(offset),
            static_cast<qsizetype>(chunk_size)));
        offset += chunk_size;
    }

    const term::Terminal_render_snapshot snapshot = model.render_snapshot(1U);
    (void)term::validate_render_snapshot(snapshot);
}

#ifndef VNM_TERMINAL_LIBFUZZER
bool run_corpus_directory(const fs::path& directory)
{
    if (!fs::is_directory(directory)) {
        std::cerr << "FAIL: corpus directory does not exist: " << directory << '\n';
        return false;
    }

    bool ok         = true;
    int  file_count = 0;

    for (const fs::directory_entry& entry : fs::recursive_directory_iterator(directory)) {
        if (!entry.is_regular_file()) {
            continue;
        }

        const QByteArray bytes = fixture::read_corpus_bytes(entry.path(), ok);
        if (bytes.isEmpty()) {
            continue;
        }

        ++file_count;
        exercise_parser(
            reinterpret_cast<const std::uint8_t*>(bytes.constData()),
            static_cast<std::size_t>(bytes.size()));
    }

    if (file_count == 0) {
        std::cerr << "FAIL: corpus directory has no non-empty files: "
            << directory << '\n';
        ok = false;
    }

    return ok;
}
#endif

}

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size)
{
    exercise_parser(data, size);
    return 0;
}

#ifndef VNM_TERMINAL_LIBFUZZER
int main(int argc, char** argv)
{
    if (argc != 2) {
        std::cerr << "usage: parser_fuzz_smoke <corpus-dir>\n";
        return 1;
    }

    return run_corpus_directory(fs::path(argv[1])) ? 0 : 1;
}
#endif
