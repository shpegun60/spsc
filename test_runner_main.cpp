#include "test_suite_catalog.h"
#include "test_suite_registry.h"
#include "test_build_config.hpp"

#include <QByteArray>
#include <QCoreApplication>
#include <QStringList>
#include <QTextStream>
#include <QVector>

int main(int argc, char* argv[])
{
    QCoreApplication app(argc, argv);

    const QStringList args = QCoreApplication::arguments();
    if (args.contains(QStringLiteral("--list-suites"))) {
        QTextStream out(stdout);
        for (const QString& suiteName : all_test_suite_names()) {
            out << suiteName << '\n';
        }
        return 0;
    }

    const int runSuiteIndex = args.indexOf(QStringLiteral("--run-suite"));
    if (runSuiteIndex < 0 || (runSuiteIndex + 1) >= args.size()) {
        QTextStream err(stderr);
        err << "Missing --run-suite <name> argument.\n";
        return 2;
    }

    const QString suiteName = args.at(runSuiteIndex + 1);
    const QString actualBuildConfig = spsc_test::actual_build_config_summary(suiteName);
    const QString expectedBuildConfig = spsc_test::expected_build_config_summary(suiteName);

    {
        QTextStream out(stdout);
        out << "[spsc-test-config] " << actualBuildConfig << '\n';
    }

    if (actualBuildConfig != expectedBuildConfig) {
        QTextStream err(stderr);
        err << "[spsc-test-config][ERROR] expected " << expectedBuildConfig << '\n';
        err << "[spsc-test-config][ERROR] actual   " << actualBuildConfig << '\n';
        return 3;
    }

    QVector<QByteArray> argStorage;
    argStorage.reserve(args.size() - 1);
    for (int i = 0; i < args.size(); ++i) {
        if (i == runSuiteIndex || i == (runSuiteIndex + 1)) {
            continue;
        }
        argStorage.push_back(args.at(i).toLocal8Bit());
    }

    QVector<char*> runnerArgv;
    runnerArgv.reserve(argStorage.size());
    for (QByteArray& arg : argStorage) {
        runnerArgv.push_back(arg.data());
    }

    return run_named_test_suite(suiteName, static_cast<int>(runnerArgv.size()), runnerArgv.data());
}
