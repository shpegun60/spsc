#include "test_suite_registry.h"

#include "src/tests/chunk_test.h"
#include "src/tests/buffer_pool_test.h"
#include "src/tests/fifo_test.h"
#include "src/tests/fifo_view_test.h"
#include "src/tests/latest_test.h"
#include "src/tests/pool_test.h"
#include "src/tests/pool_view_test.h"
#include "src/tests/queue_test.h"
#include "src/tests/typed_pool_test.h"

#include <array>

namespace {

using Runner = int (*)(int, char**);

struct SuiteEntry {
    const char* name;
    Runner      runner;
};

constexpr std::array<SuiteEntry, 9> kSuites{{
    {"buffer_pool", &run_tst_buffer_pool_api_paranoid},
    {"fifo", &run_tst_fifo_api_paranoid},
    {"fifo_view", &run_tst_fifo_view_api_paranoid},
    {"pool", &run_tst_pool_api_paranoid},
    {"pool_view", &run_tst_pool_view_api_paranoid},
    {"latest", &run_tst_latest_api_paranoid},
    {"chunk", &run_tst_chunk_api_paranoid},
    {"queue", &run_tst_queue_api_paranoid},
    {"typed_pool", &run_tst_typed_pool_api_paranoid}
}};

} // namespace

int run_named_test_suite(const QString& suiteName, int argc, char** argv)
{
    for (const SuiteEntry& suite : kSuites) {
        if (suiteName == QLatin1String(suite.name)) {
            return suite.runner(argc, argv);
        }
    }
    return -1;
}
