#include "mainwindow.h"
#include "test_suite_registry.h"

#include <QApplication>
#include <QByteArray>
#include <QCoreApplication>
#include <QStringList>
#include <QVector>

int main(int argc, char *argv[])
{
    QApplication a(argc, argv);

    const QStringList args = QCoreApplication::arguments();
    const int runSuiteIndex = args.indexOf(QStringLiteral("--run-suite"));
    if (runSuiteIndex >= 0 && (runSuiteIndex + 1) < args.size()) {
        const QString suiteName = args.at(runSuiteIndex + 1);

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

        return run_named_test_suite(suiteName, runnerArgv.size(), runnerArgv.data());
    }

    MainWindow w;
    w.show();
    return a.exec();
}
