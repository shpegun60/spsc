#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>
#include <QString>
#include <QVector>

QT_BEGIN_NAMESPACE
namespace Ui {
class MainWindow;
}
QT_END_NAMESPACE

class QLabel;
class QPlainTextEdit;
class QPushButton;
class QSpinBox;
class QTableWidget;

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    MainWindow(QWidget *parent = nullptr);
    ~MainWindow();

private:
    struct TestSuiteSpec {
        QString name;
    };

    struct TestSuiteResult {
        QString name;
        QString log;
        int     exitCode = -1;
        int     passed = -1;
        int     failed = -1;
        int     skipped = -1;
        int     blacklisted = -1;
        qint64  durationMs = -1;
        bool    timedOut = false;
        bool    crashed = false;
    };

    void setupDashboard();
    QVector<TestSuiteSpec> suiteSpecs() const;
    TestSuiteResult runSuite(const TestSuiteSpec& spec, int timeoutMs) const;
    void resetTable(const QVector<TestSuiteSpec>& specs);
    void setRowRunning(int row);
    void setRowResult(int row, const TestSuiteResult& result);
    void updateSummaryLabel();

private slots:
    void runAllTests();

private:
    Ui::MainWindow *ui;
    QPushButton*    runButton_ = nullptr;
    QSpinBox*       timeoutSpin_ = nullptr;
    QLabel*         summaryLabel_ = nullptr;
    QTableWidget*   suiteTable_ = nullptr;
    QPlainTextEdit* logOutput_ = nullptr;
    QVector<TestSuiteResult> suiteResults_;
    bool isRunning_ = false;
};
#endif // MAINWINDOW_H
