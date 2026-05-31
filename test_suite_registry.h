#ifndef TEST_SUITE_REGISTRY_H
#define TEST_SUITE_REGISTRY_H

#include <QString>
#include <QVector>

QVector<QString> registered_test_suite_names();
int run_named_test_suite(const QString& suiteName, int argc, char** argv);

#endif // TEST_SUITE_REGISTRY_H
