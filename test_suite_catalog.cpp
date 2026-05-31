#include "test_suite_catalog.h"
#include "test_suite_entries.h"

QVector<QString> all_test_suite_names()
{
    return {
#define SPSC_TEST_SUITE_NAME_ENTRY(name, runner) QStringLiteral(name),
        SPSC_TEST_SUITE_TABLE(SPSC_TEST_SUITE_NAME_ENTRY)
#undef SPSC_TEST_SUITE_NAME_ENTRY
    };
}
