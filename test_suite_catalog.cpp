#include "test_suite_catalog.h"

QVector<QString> all_test_suite_names()
{
    return {
        QStringLiteral("fifo"),
        QStringLiteral("fifo_view"),
        QStringLiteral("pool"),
        QStringLiteral("pool_view"),
        QStringLiteral("latest"),
        QStringLiteral("chunk"),
        QStringLiteral("queue"),
        QStringLiteral("typed_pool"),
        QStringLiteral("buffer_pool")
    };
}
