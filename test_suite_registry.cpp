#include "test_suite_registry.h"

#include "src/chunk_test.h"
#include "src/fifo_test.h"
#include "src/fifo_view_test.h"
#include "src/latest_test.h"
#include "src/pool_test.h"
#include "src/pool_view_test.h"
#include "src/queue_test.h"
#include "src/typed_pool_test.h"

#include <array>

namespace {

using Runner = int (*)(int, char**);

struct SuiteEntry {
    const char* name;
    Runner      runner;
};

constexpr std::array<SuiteEntry, 8> kSuites{{
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

QVector<QString> all_test_suite_names()
{
    QVector<QString> names;
    names.reserve(static_cast<qsizetype>(kSuites.size()));
    for (const SuiteEntry& suite : kSuites) {
        names.push_back(QString::fromLatin1(suite.name));
    }
    return names;
}

int run_named_test_suite(const QString& suiteName, int argc, char** argv)
{
    for (const SuiteEntry& suite : kSuites) {
        if (suiteName == QLatin1String(suite.name)) {
            return suite.runner(argc, argv);
        }
    }
    return -1;
}
