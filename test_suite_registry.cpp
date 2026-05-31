#include "test_suite_registry.h"
#include "test_suite_entries.h"

#include "src/tests/chunk_test.h"
#include "src/tests/buffer_pool_test.h"
#include "src/tests/fifo_test.h"
#include "src/tests/fifo_view_test.h"
#include "src/tests/latest_test.h"
#include "src/tests/pool_test.h"
#include "src/tests/pool_view_test.h"
#include "src/tests/queue_test.h"
#include "src/tests/typed_pool_test.h"

#include <iterator>
#include <QTextStream>

namespace {

using Runner = int (*)(int, char**);

struct SuiteEntry {
    const char* name;
    Runner      runner;
};

constexpr SuiteEntry kSuites[] = {
#define SPSC_TEST_SUITE_REGISTRY_ENTRY(name, runner) {name, &runner},
    SPSC_TEST_SUITE_TABLE(SPSC_TEST_SUITE_REGISTRY_ENTRY)
#undef SPSC_TEST_SUITE_REGISTRY_ENTRY
};

} // namespace

QVector<QString> registered_test_suite_names()
{
    QVector<QString> names;
    names.reserve(static_cast<int>(std::size(kSuites)));
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
    QTextStream err(stderr);
    err << "Unknown suite: " << suiteName << '\n';
    return 4;
}
