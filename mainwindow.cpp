#include "mainwindow.h"
#include "test_suite_registry.h"
#include "ui_mainwindow.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFont>
#include <QFontDatabase>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QLabel>
#include <QPlainTextEdit>
#include <QProcess>
#include <QPushButton>
#include <QRegularExpression>
#include <QScrollBar>
#include <QSplitter>
#include <QSpinBox>
#include <QStandardPaths>
#include <QStatusBar>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QTimer>
#include <QUuid>
#include <QVBoxLayout>

namespace {

constexpr int kColumnSuite = 0;
constexpr int kColumnStatus = 1;
constexpr int kColumnPassed = 2;
constexpr int kColumnFailed = 3;
constexpr int kColumnSkipped = 4;
constexpr int kColumnBlacklisted = 5;
constexpr int kColumnDuration = 6;
constexpr int kColumnCount = 7;

QTableWidgetItem* ensureItem(QTableWidget* table, const int row, const int column)
{
    auto* item = table->item(row, column);
    if (item == nullptr) {
        item = new QTableWidgetItem;
        table->setItem(row, column, item);
    }
    return item;
}

void setStatusVisual(QTableWidgetItem* item, const QString& text, const QColor& background)
{
    item->setText(text);
    item->setTextAlignment(Qt::AlignCenter);
    item->setBackground(background);
    item->setForeground(QBrush(Qt::black));
}

QString formatMetric(const int value)
{
    return (value >= 0) ? QString::number(value) : QStringLiteral("-");
}

QString uniqueLogPath()
{
    const QString tempDir =
        QStandardPaths::writableLocation(QStandardPaths::TempLocation);
    return QDir(tempDir).filePath(
        QStringLiteral("spsc_test_%1.txt")
            .arg(QUuid::createUuid().toString(QUuid::WithoutBraces)));
}

QString appendSection(const QString& base, const QString& title, const QString& body)
{
    if (body.trimmed().isEmpty()) {
        return base;
    }

    QString out = base;
    if (!out.isEmpty() && !out.endsWith(QLatin1Char('\n'))) {
        out += QLatin1Char('\n');
    }
    out += QStringLiteral("[%1]\n").arg(title);
    out += body.trimmed();
    out += QLatin1Char('\n');
    return out;
}

} // namespace


MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
    , ui(new Ui::MainWindow)
{
    ui->setupUi(this);
    setupDashboard();
    QTimer::singleShot(0, this, &MainWindow::runAllTests);
}

MainWindow::~MainWindow()
{
    delete ui;
}

void MainWindow::setupDashboard()
{
    setWindowTitle(QStringLiteral("SPSC Test Dashboard"));
    resize(1200, 780);

    auto* layout = new QVBoxLayout(ui->centralwidget);
    layout->setContentsMargins(12, 12, 12, 12);
    layout->setSpacing(8);

    auto* headerLayout = new QHBoxLayout;
    headerLayout->setSpacing(10);

    auto* titleLabel = new QLabel(QStringLiteral("SPSC Paranoid Test Dashboard"), this);
    QFont titleFont = titleLabel->font();
    titleFont.setPointSize(titleFont.pointSize() + 4);
    titleFont.setBold(true);
    titleLabel->setFont(titleFont);

    summaryLabel_ = new QLabel(QStringLiteral("Ready"), this);
    runButton_ = new QPushButton(QStringLiteral("Run All Tests"), this);
    timeoutSpin_ = new QSpinBox(this);
    timeoutSpin_->setRange(1, 600);
    timeoutSpin_->setValue(30);
    timeoutSpin_->setSuffix(QStringLiteral(" s"));
    timeoutSpin_->setToolTip(QStringLiteral("Per-suite timeout. A hanging test process will be killed."));
    auto* timeoutLabel = new QLabel(QStringLiteral("Timeout:"), this);

    headerLayout->addWidget(titleLabel);
    headerLayout->addStretch(1);
    headerLayout->addWidget(timeoutLabel);
    headerLayout->addWidget(timeoutSpin_);
    headerLayout->addWidget(summaryLabel_);
    headerLayout->addWidget(runButton_);

    auto* splitter = new QSplitter(Qt::Vertical, ui->centralwidget);
    splitter->setChildrenCollapsible(false);

    suiteTable_ = new QTableWidget(splitter);
    suiteTable_->setColumnCount(kColumnCount);
    suiteTable_->setHorizontalHeaderLabels({
        QStringLiteral("Suite"),
        QStringLiteral("Status"),
        QStringLiteral("Passed"),
        QStringLiteral("Failed"),
        QStringLiteral("Skipped"),
        QStringLiteral("Blacklisted"),
        QStringLiteral("Duration (ms)")
    });
    suiteTable_->setAlternatingRowColors(true);
    suiteTable_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    suiteTable_->setSelectionBehavior(QAbstractItemView::SelectRows);
    suiteTable_->setSelectionMode(QAbstractItemView::SingleSelection);
    suiteTable_->setSortingEnabled(false);
    suiteTable_->verticalHeader()->setVisible(false);
    suiteTable_->horizontalHeader()->setStretchLastSection(false);
    suiteTable_->horizontalHeader()->setSectionResizeMode(kColumnSuite, QHeaderView::Stretch);
    for (int column = kColumnStatus; column < kColumnCount; ++column) {
        suiteTable_->horizontalHeader()->setSectionResizeMode(column, QHeaderView::ResizeToContents);
    }

    logOutput_ = new QPlainTextEdit(splitter);
    logOutput_->setReadOnly(true);
    logOutput_->setLineWrapMode(QPlainTextEdit::NoWrap);
    logOutput_->setPlaceholderText(
        QStringLiteral("QtTest logs will appear here after each suite finishes."));
    logOutput_->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));

    splitter->setStretchFactor(0, 0);
    splitter->setStretchFactor(1, 1);
    splitter->setSizes({320, 460});

    layout->addLayout(headerLayout);
    layout->addWidget(splitter, 1);

    statusBar()->showMessage(QStringLiteral("Ready to run tests"));

    connect(runButton_, &QPushButton::clicked, this, &MainWindow::runAllTests);
}

QVector<MainWindow::TestSuiteSpec> MainWindow::suiteSpecs() const
{
    QVector<TestSuiteSpec> specs;
    const QVector<QString> names = all_test_suite_names();
    specs.reserve(names.size());
    for (const QString& name : names) {
        specs.push_back({name});
    }
    return specs;
}

MainWindow::TestSuiteResult MainWindow::runSuite(const TestSuiteSpec& spec, const int timeoutMs) const
{
    TestSuiteResult result;
    result.name = spec.name;

    const QString logPath = uniqueLogPath();
    const QString outputSpec = QStringLiteral("%1,txt").arg(logPath);
    QProcess process;
    process.setProgram(QCoreApplication::applicationFilePath());
    process.setArguments({
        QStringLiteral("--run-suite"),
        spec.name,
        QStringLiteral("-o"),
        outputSpec
    });

    QElapsedTimer timer;
    timer.start();
    process.start();
    if (!process.waitForStarted(5000)) {
        result.log = QStringLiteral("Failed to start test process: %1")
                         .arg(process.errorString());
        result.exitCode = -1;
        return result;
    }

    while (process.state() != QProcess::NotRunning) {
        process.waitForFinished(100);
        QCoreApplication::processEvents();

        if (timer.elapsed() > timeoutMs) {
            result.timedOut = true;
            process.kill();
            process.waitForFinished(3000);
            break;
        }
    }

    result.durationMs = timer.elapsed();
    result.crashed = (process.exitStatus() == QProcess::CrashExit) && !result.timedOut;
    if (!result.timedOut && !result.crashed) {
        result.exitCode = process.exitCode();
    } else {
        result.exitCode = -1;
    }

    QFile logFile(logPath);
    if (logFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
        result.log = QString::fromUtf8(logFile.readAll());
        logFile.close();
    }
    logFile.remove();

    result.log = appendSection(
        result.log,
        QStringLiteral("stdout"),
        QString::fromLocal8Bit(process.readAllStandardOutput()));
    result.log = appendSection(
        result.log,
        QStringLiteral("stderr"),
        QString::fromLocal8Bit(process.readAllStandardError()));

    if (result.timedOut) {
        result.log = appendSection(
            result.log,
            QStringLiteral("timeout"),
            QStringLiteral("Suite exceeded timeout of %1 ms and was terminated.")
                .arg(timeoutMs));
    } else if (result.crashed) {
        result.log = appendSection(
            result.log,
            QStringLiteral("crash"),
            QStringLiteral("Suite process crashed before completing."));
    } else if (result.log.trimmed().isEmpty()) {
        result.log = QStringLiteral("Suite finished with exit code %1 but produced no log output.")
                         .arg(result.exitCode);
    }

    const QRegularExpression totalsRe(
        QStringLiteral(
            R"(Totals:\s+(\d+)\s+passed,\s+(\d+)\s+failed,\s+(\d+)\s+skipped,\s+(\d+)\s+blacklisted,\s+(\d+)\s*ms)"));
    const QRegularExpressionMatch totalsMatch = totalsRe.match(result.log);
    if (totalsMatch.hasMatch()) {
        result.passed = totalsMatch.captured(1).toInt();
        result.failed = totalsMatch.captured(2).toInt();
        result.skipped = totalsMatch.captured(3).toInt();
        result.blacklisted = totalsMatch.captured(4).toInt();
        result.durationMs = totalsMatch.captured(5).toLongLong();
    }

    return result;
}

void MainWindow::resetTable(const QVector<TestSuiteSpec>& specs)
{
    suiteTable_->clearContents();
    suiteTable_->setRowCount(specs.size());

    for (int row = 0; row < specs.size(); ++row) {
        ensureItem(suiteTable_, row, kColumnSuite)->setText(specs[row].name);
        setStatusVisual(
            ensureItem(suiteTable_, row, kColumnStatus),
            QStringLiteral("PENDING"),
            QColor(216, 220, 226));

        for (int column = kColumnPassed; column < kColumnCount; ++column) {
            auto* item = ensureItem(suiteTable_, row, column);
            item->setText(QStringLiteral("-"));
            item->setTextAlignment(Qt::AlignCenter);
            item->setBackground(QBrush());
        }
    }
}

void MainWindow::setRowRunning(const int row)
{
    setStatusVisual(
        ensureItem(suiteTable_, row, kColumnStatus),
        QStringLiteral("RUNNING"),
        QColor(255, 231, 153));
}

void MainWindow::setRowResult(const int row, const TestSuiteResult& result)
{
    ensureItem(suiteTable_, row, kColumnSuite)->setText(result.name);

    auto* statusItem = ensureItem(suiteTable_, row, kColumnStatus);
    if (result.timedOut) {
        setStatusVisual(statusItem, QStringLiteral("TIMEOUT"), QColor(255, 199, 206));
    } else if (result.crashed) {
        setStatusVisual(statusItem, QStringLiteral("CRASH"), QColor(234, 153, 153));
    } else if (result.exitCode == 0 && result.failed == 0) {
        setStatusVisual(statusItem, QStringLiteral("PASS"), QColor(183, 225, 205));
    } else {
        setStatusVisual(statusItem, QStringLiteral("FAIL"), QColor(244, 204, 204));
    }

    const QStringList metrics = {
        formatMetric(result.passed),
        formatMetric(result.failed),
        formatMetric(result.skipped),
        formatMetric(result.blacklisted),
        (result.durationMs >= 0) ? QString::number(result.durationMs) : QStringLiteral("-")
    };

    for (int i = 0; i < metrics.size(); ++i) {
        auto* item = ensureItem(suiteTable_, row, kColumnPassed + i);
        item->setText(metrics[i]);
        item->setTextAlignment(Qt::AlignCenter);
    }
}

void MainWindow::updateSummaryLabel()
{
    const int totalSuites = suiteResults_.size();
    int finishedSuites = 0;
    int passedSuites = 0;
    int failedSuites = 0;

    for (const TestSuiteResult& result : suiteResults_) {
        if (result.exitCode == -1 && !result.timedOut && !result.crashed) {
            continue;
        }

        ++finishedSuites;
        if (result.exitCode == 0 && result.failed == 0) {
            ++passedSuites;
        } else {
            ++failedSuites;
        }
    }

    QString summary = QStringLiteral("%1/%2 finished")
                          .arg(finishedSuites)
                          .arg(totalSuites);
    if (finishedSuites > 0) {
        summary += QStringLiteral("  |  PASS: %1  FAIL: %2")
                       .arg(passedSuites)
                       .arg(failedSuites);
    }
    summaryLabel_->setText(summary);
}

void MainWindow::runAllTests()
{
    if (isRunning_) {
        return;
    }

    const QVector<TestSuiteSpec> specs = suiteSpecs();
    const int timeoutMs = timeoutSpin_->value() * 1000;
    suiteResults_.clear();
    suiteResults_.resize(specs.size());

    resetTable(specs);
    updateSummaryLabel();
    logOutput_->clear();
    logOutput_->appendPlainText(
        QStringLiteral("Starting paranoid SPSC suite run...\nPer-suite timeout: %1 ms\n")
            .arg(timeoutMs));

    isRunning_ = true;
    runButton_->setEnabled(false);
    timeoutSpin_->setEnabled(false);

    for (int row = 0; row < specs.size(); ++row) {
        setRowRunning(row);
        statusBar()->showMessage(QStringLiteral("Running %1...").arg(specs[row].name));
        suiteTable_->selectRow(row);
        suiteTable_->scrollToItem(suiteTable_->item(row, kColumnSuite));
        QCoreApplication::processEvents();

        const TestSuiteResult result = runSuite(specs[row], timeoutMs);
        suiteResults_[row] = result;
        setRowResult(row, result);
        updateSummaryLabel();

        logOutput_->appendPlainText(QStringLiteral("=== %1 ===").arg(result.name));
        logOutput_->appendPlainText(result.log.trimmed());
        logOutput_->appendPlainText(QString());
        logOutput_->verticalScrollBar()->setValue(logOutput_->verticalScrollBar()->maximum());

        QCoreApplication::processEvents();
    }

    isRunning_ = false;
    runButton_->setEnabled(true);
    timeoutSpin_->setEnabled(true);

    int failedSuites = 0;
    int timedOutSuites = 0;
    for (const TestSuiteResult& result : suiteResults_) {
        if (result.timedOut) {
            ++timedOutSuites;
        }
        if (result.timedOut || result.crashed || result.exitCode != 0 || result.failed > 0) {
            ++failedSuites;
        }
    }

    statusBar()->showMessage(
        (failedSuites == 0)
            ? QStringLiteral("All test suites passed")
            : QStringLiteral("%1 test suite(s) failed, %2 timed out")
                  .arg(failedSuites)
                  .arg(timedOutSuites));
}
