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
class QTabWidget;
class QTableWidget;
class QVBoxLayout;
class QWidget;

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    MainWindow(QWidget *parent = nullptr);
    ~MainWindow();

private:
    struct TestSuiteSpec {
        QString variantId;
        QString variantLabel;
        QString suiteName;
        QString executableName;
    };

    struct TestSuiteResult {
        QString variantId;
        QString variantLabel;
        QString suiteName;
        QString log;
        QString configBanner;
        QString configError;
        int     exitCode = -1;
        int     passed = -1;
        int     failed = -1;
        int     skipped = -1;
        int     blacklisted = -1;
        qint64  durationMs = -1;
        bool    timedOut = false;
        bool    crashed = false;
        bool    configMismatch = false;
    };

    struct VariantTab {
        QString         id;
        QString         label;
        QWidget*        page = nullptr;
        QLabel*         infoLabel = nullptr;
        QTableWidget*   suiteTable = nullptr;
        QPlainTextEdit* logOutput = nullptr;
    };

    struct ResultsTab {
        QWidget*      page = nullptr;
        QLabel*       summaryLabel = nullptr;
        QTableWidget* table = nullptr;
    };

    void setupDashboard();
    void setupVariantTabs(QVBoxLayout* layout);
    void configureSuiteTable(QTableWidget* table) const;
    void configureResultsTable(QTableWidget* table) const;
    QVector<TestSuiteSpec> suiteSpecs() const;
    TestSuiteResult runSuite(const TestSuiteSpec& spec, int timeoutMs) const;
    QString resolveProgramPath(const TestSuiteSpec& spec) const;
    VariantTab* findVariantTab(const QString& variantId);
    int suiteRowForName(const QString& suiteName) const;
    void resetTabs();
    void setRowRunning(const TestSuiteSpec& spec);
    void setRowResult(const TestSuiteResult& result);
    void appendLogEntry(const TestSuiteResult& result);
    void updateResultsTab();
    void updateSummaryLabel();
    QString writeSummaryReport() const;
    QString summaryReportText() const;

private slots:
    void runAllTests();

private:
    Ui::MainWindow *ui;
    QPushButton*    runButton_ = nullptr;
    QSpinBox*       timeoutSpin_ = nullptr;
    QLabel*         summaryLabel_ = nullptr;
    QTabWidget*     tabWidget_ = nullptr;
    ResultsTab      resultsTab_;
    QVector<VariantTab> variantTabs_;
    QVector<TestSuiteResult> suiteResults_;
    QString         lastSummaryPath_;
    bool isRunning_ = false;
};
#endif // MAINWINDOW_H
