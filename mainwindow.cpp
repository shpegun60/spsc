#include "mainwindow.h"
#include "test_suite_catalog.h"
#include "ui_mainwindow.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QFont>
#include <QFontDatabase>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QLabel>
#include <QLibraryInfo>
#include <QPlainTextEdit>
#include <QProcess>
#include <QProcessEnvironment>
#include <QPushButton>
#include <QRegularExpression>
#include <QScrollBar>
#include <QSet>
#include <QSplitter>
#include <QSpinBox>
#include <QStandardPaths>
#include <QStatusBar>
#include <QStringList>
#include <QTabWidget>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QTextStream>
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

constexpr int kResultColumnMode = 0;
constexpr int kResultColumnConfig = 1;
constexpr int kResultColumnShadow = 2;
constexpr int kResultColumnAllow32 = 3;
constexpr int kResultColumnHeuristic = 4;
constexpr int kResultColumnFinished = 5;
constexpr int kResultColumnPassed = 6;
constexpr int kResultColumnFailed = 7;
constexpr int kResultColumnTimeouts = 8;
constexpr int kResultColumnStatus = 9;
constexpr int kResultColumnCount = 10;

struct TestVariant {
    QString id;
    QString label;
    QString executableBaseName;
    int     enableShadow = 0;
    int     allow32Bit = 0;
    int     heuristic = 0;
};

struct ParsedConfigBanner {
    bool    valid = false;
    QString suiteName;
    QString variantName;
    int     enableShadow = -1;
    int     allow32Bit = -1;
    int     heuristic = -1;
    int     regBits = -1;
    int     atomicShadow = -1;
    int     cachedShadow = -1;
    QString compactText;
};

QVector<TestVariant> configuredVariants()
{
    return {
        {QStringLiteral("shadow_off"), QStringLiteral("Shadow Off"), QStringLiteral("spsc_test_shadow_off"), 0, 0, 0},
        {QStringLiteral("shadow_on"), QStringLiteral("Shadow On"), QStringLiteral("spsc_test_shadow_on"), 1, 0, 0},
        {QStringLiteral("shadow_heur"), QStringLiteral("Shadow Heuristic"), QStringLiteral("spsc_test_shadow_heur"), 1, 0, 1}
    };
}

QString executableFileName(const QString& baseName)
{
#ifdef Q_OS_WIN
    return baseName + QStringLiteral(".exe");
#else
    return baseName;
#endif
}

QStringList splitPathList(const QString& value)
{
#ifdef Q_OS_WIN
    return value.split(QLatin1Char(';'), Qt::SkipEmptyParts);
#else
    return value.split(QLatin1Char(':'), Qt::SkipEmptyParts);
#endif
}

QString joinPathList(const QStringList& value)
{
#ifdef Q_OS_WIN
    return value.join(QLatin1Char(';'));
#else
    return value.join(QLatin1Char(':'));
#endif
}

QString normalizedPathKey(const QString& value)
{
    return QDir::cleanPath(QDir::fromNativeSeparators(value)).toLower();
}

void appendUniqueExistingPath(QStringList& ordered,
                              QSet<QString>& seen,
                              const QString& candidate)
{
    if (candidate.isEmpty()) {
        return;
    }

    const QFileInfo info(candidate);
    if (!info.exists() || !info.isDir()) {
        return;
    }

    const QString absolute = info.absoluteFilePath();
    const QString key = normalizedPathKey(absolute);
    if (seen.contains(key)) {
        return;
    }

    seen.insert(key);
    ordered.push_back(absolute);
}

QProcessEnvironment makeRunnerEnvironment(const QString& programPath)
{
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    QStringList orderedPath;
    QSet<QString> seen;

    appendUniqueExistingPath(orderedPath, seen, QFileInfo(programPath).absolutePath());
    appendUniqueExistingPath(orderedPath, seen, QLibraryInfo::path(QLibraryInfo::BinariesPath));

    const QDir appDir(QCoreApplication::applicationDirPath());
    appendUniqueExistingPath(orderedPath, seen, appDir.absolutePath());

    const QStringList existingPath = splitPathList(env.value(QStringLiteral("PATH")));
    for (const QString& entry : existingPath) {
        appendUniqueExistingPath(orderedPath, seen, entry);
    }

    env.insert(QStringLiteral("PATH"), joinPathList(orderedPath));
    return env;
}

QString currentBuildSubdir()
{
#ifdef QT_NO_DEBUG
    return QStringLiteral("release");
#else
    return QStringLiteral("debug");
#endif
}

QString displayName(const QString& variantLabel, const QString& suiteName)
{
    return QStringLiteral("%1 :: %2").arg(variantLabel, suiteName);
}

QString variantMacroInfo(const TestVariant& variant)
{
    return QStringLiteral(
               "Runner: %1\n"
               "SPSC_ENABLE_SHADOW_INDICES=%2\n"
               "SPSC_SHADOW_ALLOW_32BIT=%3\n"
               "SPSC_SHADOW_REFRESH_HEURISTIC=%4")
        .arg(executableFileName(variant.executableBaseName))
        .arg(variant.enableShadow)
        .arg(variant.allow32Bit)
        .arg(variant.heuristic);
}

ParsedConfigBanner parseConfigBanner(const QString& text)
{
    static const QRegularExpression re(
        QStringLiteral(
            R"(\[spsc-test-config\]\s+suite=([^\s]+)\s+variant=([^\s]+)\s+macros\{enable_shadow=(\d+)\s+allow_32bit=(\d+)\s+refresh_heuristic=(\d+)\}\s+effective\{reg_bits=(\d+)\s+atomic_A_shadow=(\d+)\s+cached_CA_shadow=(\d+)\})"));

    ParsedConfigBanner parsed;
    const QRegularExpressionMatch match = re.match(text);
    if (!match.hasMatch()) {
        return parsed;
    }

    parsed.valid = true;
    parsed.suiteName = match.captured(1);
    parsed.variantName = match.captured(2);
    parsed.enableShadow = match.captured(3).toInt();
    parsed.allow32Bit = match.captured(4).toInt();
    parsed.heuristic = match.captured(5).toInt();
    parsed.regBits = match.captured(6).toInt();
    parsed.atomicShadow = match.captured(7).toInt();
    parsed.cachedShadow = match.captured(8).toInt();
    parsed.compactText = QStringLiteral(
                             "variant=%1 macros{enable_shadow=%2 allow_32bit=%3 refresh_heuristic=%4} "
                             "effective{reg_bits=%5 atomic_A_shadow=%6 cached_CA_shadow=%7}")
                             .arg(parsed.variantName)
                             .arg(parsed.enableShadow)
                             .arg(parsed.allow32Bit)
                             .arg(parsed.heuristic)
                             .arg(parsed.regBits)
                             .arg(parsed.atomicShadow)
                             .arg(parsed.cachedShadow);
    return parsed;
}

QString extractConfigErrorText(const QString& text)
{
    static const QRegularExpression re(
        QStringLiteral(R"(\[spsc-test-config\]\[ERROR\][^\r\n]*)"));

    QStringList lines;
    auto it = re.globalMatch(text);
    while (it.hasNext()) {
        lines.push_back(it.next().captured(0));
    }
    return lines.join(QStringLiteral("\n"));
}

QString configMismatchReason(const ParsedConfigBanner& parsed,
                             const TestVariant& expectedVariant,
                             const QString& expectedSuiteName)
{
    if (!parsed.valid) {
        return QStringLiteral("Missing [spsc-test-config] banner in runner output.");
    }

    QStringList reasons;
    if (parsed.suiteName != expectedSuiteName) {
        reasons.push_back(
            QStringLiteral("suite=%1, expected %2")
                .arg(parsed.suiteName, expectedSuiteName));
    }
    if (parsed.variantName != expectedVariant.id) {
        reasons.push_back(
            QStringLiteral("variant=%1, expected %2")
                .arg(parsed.variantName, expectedVariant.id));
    }
    if (parsed.enableShadow != expectedVariant.enableShadow) {
        reasons.push_back(
            QStringLiteral("enable_shadow=%1, expected %2")
                .arg(parsed.enableShadow)
                .arg(expectedVariant.enableShadow));
    }
    if (parsed.allow32Bit != expectedVariant.allow32Bit) {
        reasons.push_back(
            QStringLiteral("allow_32bit=%1, expected %2")
                .arg(parsed.allow32Bit)
                .arg(expectedVariant.allow32Bit));
    }
    if (parsed.heuristic != expectedVariant.heuristic) {
        reasons.push_back(
            QStringLiteral("refresh_heuristic=%1, expected %2")
                .arg(parsed.heuristic)
                .arg(expectedVariant.heuristic));
    }

    const int expectedAtomicShadow =
        ((expectedVariant.enableShadow != 0) &&
         ((parsed.regBits >= 64) || (expectedVariant.allow32Bit != 0))) ? 1 : 0;
    if (parsed.atomicShadow != expectedAtomicShadow) {
        reasons.push_back(
            QStringLiteral("atomic_A_shadow=%1, expected %2")
                .arg(parsed.atomicShadow)
                .arg(expectedAtomicShadow));
    }
    if (parsed.cachedShadow != expectedAtomicShadow) {
        reasons.push_back(
            QStringLiteral("cached_CA_shadow=%1, expected %2")
                .arg(parsed.cachedShadow)
                .arg(expectedAtomicShadow));
    }

    return reasons.join(QStringLiteral("; "));
}

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

    layout->addLayout(headerLayout);
    setupVariantTabs(layout);

    statusBar()->showMessage(QStringLiteral("Ready to run tests"));

    connect(runButton_, &QPushButton::clicked, this, &MainWindow::runAllTests);
}

void MainWindow::setupVariantTabs(QVBoxLayout* layout)
{
    tabWidget_ = new QTabWidget(ui->centralwidget);
    tabWidget_->setDocumentMode(true);

    const QVector<TestVariant> variants = configuredVariants();
    resultsTab_ = {};
    variantTabs_.clear();
    variantTabs_.reserve(variants.size());

    resultsTab_.page = new QWidget(tabWidget_);
    auto* resultsLayout = new QVBoxLayout(resultsTab_.page);
    resultsLayout->setContentsMargins(0, 0, 0, 0);
    resultsLayout->setSpacing(8);

    resultsTab_.summaryLabel = new QLabel(QStringLiteral("No runs yet."), resultsTab_.page);
    resultsTab_.summaryLabel->setWordWrap(true);

    resultsTab_.table = new QTableWidget(resultsTab_.page);
    configureResultsTable(resultsTab_.table);

    resultsLayout->addWidget(resultsTab_.summaryLabel);
    resultsLayout->addWidget(resultsTab_.table, 1);
    tabWidget_->addTab(resultsTab_.page, QStringLiteral("Results"));

    for (const TestVariant& variant : variants) {
        auto* page = new QWidget(tabWidget_);
        auto* pageLayout = new QVBoxLayout(page);
        pageLayout->setContentsMargins(0, 0, 0, 0);
        pageLayout->setSpacing(8);

        auto* infoLabel = new QLabel(page);
        infoLabel->setWordWrap(true);
        infoLabel->setText(variantMacroInfo(variant));
        infoLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);

        auto* splitter = new QSplitter(Qt::Vertical, page);
        splitter->setChildrenCollapsible(false);

        auto* table = new QTableWidget(splitter);
        configureSuiteTable(table);

        auto* logOutput = new QPlainTextEdit(splitter);
        logOutput->setReadOnly(true);
        logOutput->setLineWrapMode(QPlainTextEdit::NoWrap);
        logOutput->setPlaceholderText(
            QStringLiteral("QtTest logs for this variant will appear here."));
        logOutput->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));

        splitter->setStretchFactor(0, 0);
        splitter->setStretchFactor(1, 1);
        splitter->setSizes({320, 460});
        pageLayout->addWidget(infoLabel);
        pageLayout->addWidget(splitter, 1);

        tabWidget_->addTab(page, variant.label);
        variantTabs_.push_back({variant.id, variant.label, page, infoLabel, table, logOutput});
    }

    layout->addWidget(tabWidget_, 1);
}

void MainWindow::configureSuiteTable(QTableWidget* table) const
{
    table->setColumnCount(kColumnCount);
    table->setHorizontalHeaderLabels({
        QStringLiteral("Suite"),
        QStringLiteral("Status"),
        QStringLiteral("Passed"),
        QStringLiteral("Failed"),
        QStringLiteral("Skipped"),
        QStringLiteral("Blacklisted"),
        QStringLiteral("Duration (ms)")
    });
    table->setAlternatingRowColors(true);
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table->setSelectionBehavior(QAbstractItemView::SelectRows);
    table->setSelectionMode(QAbstractItemView::SingleSelection);
    table->setSortingEnabled(false);
    table->verticalHeader()->setVisible(false);
    table->horizontalHeader()->setStretchLastSection(false);
    table->horizontalHeader()->setSectionResizeMode(kColumnSuite, QHeaderView::Stretch);
    for (int column = kColumnStatus; column < kColumnCount; ++column) {
        table->horizontalHeader()->setSectionResizeMode(column, QHeaderView::ResizeToContents);
    }
}

void MainWindow::configureResultsTable(QTableWidget* table) const
{
    table->setColumnCount(kResultColumnCount);
    table->setHorizontalHeaderLabels({
        QStringLiteral("Mode"),
        QStringLiteral("Config"),
        QStringLiteral("Shadow"),
        QStringLiteral("Allow 32-bit"),
        QStringLiteral("Heuristic"),
        QStringLiteral("Finished"),
        QStringLiteral("Passed"),
        QStringLiteral("Failed"),
        QStringLiteral("Timeouts"),
        QStringLiteral("Status")
    });
    table->setAlternatingRowColors(true);
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table->setSelectionBehavior(QAbstractItemView::SelectRows);
    table->setSelectionMode(QAbstractItemView::SingleSelection);
    table->setSortingEnabled(false);
    table->verticalHeader()->setVisible(false);
    table->horizontalHeader()->setStretchLastSection(false);
    table->horizontalHeader()->setSectionResizeMode(kResultColumnMode, QHeaderView::ResizeToContents);
    table->horizontalHeader()->setSectionResizeMode(kResultColumnConfig, QHeaderView::Stretch);
    for (int column = kResultColumnShadow; column < kResultColumnCount; ++column) {
        table->horizontalHeader()->setSectionResizeMode(column, QHeaderView::ResizeToContents);
    }
}

QVector<MainWindow::TestSuiteSpec> MainWindow::suiteSpecs() const
{
    QVector<TestSuiteSpec> specs;
    const QVector<TestVariant> variants = configuredVariants();
    const QVector<QString> names = all_test_suite_names();
    specs.reserve(variants.size() * names.size());
    for (const TestVariant& variant : variants) {
        for (const QString& name : names) {
            specs.push_back({
                variant.id,
                variant.label,
                name,
                executableFileName(variant.executableBaseName)
            });
        }
    }
    return specs;
}

QString MainWindow::resolveProgramPath(const TestSuiteSpec& spec) const
{
    const QDir appDir(QCoreApplication::applicationDirPath());
    const QString buildSubdir = currentBuildSubdir();
    const QString appDirName = QFileInfo(appDir.absolutePath()).fileName();

    if (appDirName.compare(buildSubdir, Qt::CaseInsensitive) == 0) {
        return appDir.filePath(spec.executableName);
    }

    return appDir.filePath(buildSubdir + QLatin1Char('/') + spec.executableName);
}

MainWindow::TestSuiteResult MainWindow::runSuite(const TestSuiteSpec& spec, const int timeoutMs) const
{
    TestSuiteResult result;
    result.variantId = spec.variantId;
    result.variantLabel = spec.variantLabel;
    result.suiteName = spec.suiteName;

    const QString programPath = resolveProgramPath(spec);
    if (!QFileInfo::exists(programPath)) {
        result.exitCode = 127;
        result.log = QStringLiteral(
                         "Missing test runner executable: %1\n"
                         "Launcher dir: %2\n"
                         "Selected build subdir: %3")
                         .arg(programPath,
                              QCoreApplication::applicationDirPath(),
                              currentBuildSubdir());
        return result;
    }

    const QString logPath = uniqueLogPath();
    const QString outputSpec = QStringLiteral("%1,txt").arg(logPath);
    QProcess process;
    process.setProgram(programPath);
    process.setProcessEnvironment(makeRunnerEnvironment(programPath));
    process.setArguments({
        QStringLiteral("--run-suite"),
        spec.suiteName,
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

    const ParsedConfigBanner parsedConfig = parseConfigBanner(result.log);
    if (parsedConfig.valid) {
        result.configBanner = parsedConfig.compactText;
    }

    const QVector<TestVariant> variants = configuredVariants();
    for (const TestVariant& variant : variants) {
        if (variant.id != spec.variantId) {
            continue;
        }

        const QString runnerConfigError = extractConfigErrorText(result.log);
        const QString launcherMismatchReason =
            configMismatchReason(parsedConfig, variant, spec.suiteName);

        if (!runnerConfigError.isEmpty()) {
            result.configMismatch = true;
            result.configError = runnerConfigError;
        } else if (!launcherMismatchReason.isEmpty() &&
                   (!result.timedOut && !result.crashed)) {
            result.configMismatch = true;
            result.configError = QStringLiteral("[launcher-config-check] %1")
                                     .arg(launcherMismatchReason);
        }

        if (result.configMismatch) {
            result.log = appendSection(
                result.log,
                QStringLiteral("launcher-config-check"),
                result.configError);
        }
        break;
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

MainWindow::VariantTab* MainWindow::findVariantTab(const QString& variantId)
{
    for (VariantTab& variantTab : variantTabs_) {
        if (variantTab.id == variantId) {
            return &variantTab;
        }
    }
    return nullptr;
}

int MainWindow::suiteRowForName(const QString& suiteName) const
{
    const QVector<QString> suiteNames = all_test_suite_names();
    for (int row = 0; row < suiteNames.size(); ++row) {
        if (suiteNames[row] == suiteName) {
            return row;
        }
    }
    return -1;
}

void MainWindow::resetTabs()
{
    const QVector<QString> suiteNames = all_test_suite_names();
    lastSummaryPath_.clear();

    if (resultsTab_.table != nullptr) {
        resultsTab_.table->clearContents();
        resultsTab_.table->setRowCount(configuredVariants().size());
    }
    if (resultsTab_.summaryLabel != nullptr) {
        resultsTab_.summaryLabel->setText(
            QStringLiteral("No completed runs yet. Summary will appear here."));
    }

    for (VariantTab& variantTab : variantTabs_) {
        variantTab.suiteTable->clearContents();
        variantTab.suiteTable->setRowCount(suiteNames.size());

        for (int row = 0; row < suiteNames.size(); ++row) {
            ensureItem(variantTab.suiteTable, row, kColumnSuite)->setText(suiteNames[row]);
            setStatusVisual(
                ensureItem(variantTab.suiteTable, row, kColumnStatus),
                QStringLiteral("PENDING"),
                QColor(216, 220, 226));

            for (int column = kColumnPassed; column < kColumnCount; ++column) {
                auto* item = ensureItem(variantTab.suiteTable, row, column);
                item->setText(QStringLiteral("-"));
                item->setTextAlignment(Qt::AlignCenter);
                item->setBackground(QBrush());
            }
        }

        variantTab.logOutput->clear();
        variantTab.logOutput->appendPlainText(
            QStringLiteral("[%1]\nWaiting for run...\n").arg(variantTab.label));
    }
}

void MainWindow::setRowRunning(const TestSuiteSpec& spec)
{
    VariantTab* variantTab = findVariantTab(spec.variantId);
    if (variantTab == nullptr) {
        return;
    }

    const int row = suiteRowForName(spec.suiteName);
    if (row < 0) {
        return;
    }

    setStatusVisual(
        ensureItem(variantTab->suiteTable, row, kColumnStatus),
        QStringLiteral("RUNNING"),
        QColor(255, 231, 153));
}

void MainWindow::setRowResult(const TestSuiteResult& result)
{
    VariantTab* variantTab = findVariantTab(result.variantId);
    if (variantTab == nullptr) {
        return;
    }

    const int row = suiteRowForName(result.suiteName);
    if (row < 0) {
        return;
    }

    ensureItem(variantTab->suiteTable, row, kColumnSuite)->setText(result.suiteName);

    auto* statusItem = ensureItem(variantTab->suiteTable, row, kColumnStatus);
    if (result.timedOut) {
        setStatusVisual(statusItem, QStringLiteral("TIMEOUT"), QColor(255, 199, 206));
    } else if (result.crashed) {
        setStatusVisual(statusItem, QStringLiteral("CRASH"), QColor(234, 153, 153));
    } else if (result.configMismatch) {
        setStatusVisual(statusItem, QStringLiteral("CFG MISMATCH"), QColor(255, 204, 153));
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
        auto* item = ensureItem(variantTab->suiteTable, row, kColumnPassed + i);
        item->setText(metrics[i]);
        item->setTextAlignment(Qt::AlignCenter);
    }
}

void MainWindow::appendLogEntry(const TestSuiteResult& result)
{
    VariantTab* variantTab = findVariantTab(result.variantId);
    if (variantTab == nullptr) {
        return;
    }

    variantTab->logOutput->appendPlainText(QStringLiteral("=== %1 ===").arg(result.suiteName));
    variantTab->logOutput->appendPlainText(result.log.trimmed());
    variantTab->logOutput->appendPlainText(QString());
    variantTab->logOutput->verticalScrollBar()->setValue(
        variantTab->logOutput->verticalScrollBar()->maximum());
}

void MainWindow::updateResultsTab()
{
    if (resultsTab_.table == nullptr || resultsTab_.summaryLabel == nullptr) {
        return;
    }

    const QVector<TestVariant> variants = configuredVariants();
    const int totalPerVariant = all_test_suite_names().size();

    int totalFinished = 0;
    int totalPassed = 0;
    int totalFailed = 0;
    int totalTimeouts = 0;

    resultsTab_.table->setRowCount(variants.size());

    for (int row = 0; row < variants.size(); ++row) {
        const TestVariant& variant = variants[row];
        int finished = 0;
        int passed = 0;
        int failed = 0;
        int timeouts = 0;
        bool variantConfigMismatch = false;
        QString configBanner = QStringLiteral("-");

        for (const TestSuiteResult& result : suiteResults_) {
            if (result.variantId != variant.id) {
                continue;
            }
            if (!result.configBanner.trimmed().isEmpty()) {
                if (configBanner == QStringLiteral("-")) {
                    configBanner = result.configBanner;
                } else if (configBanner != result.configBanner) {
                    variantConfigMismatch = true;
                    configBanner = QStringLiteral("MULTIPLE CONFIG BANNERS");
                }
            }
            if (result.configMismatch) {
                variantConfigMismatch = true;
            }
            if (result.exitCode == -1 && !result.timedOut && !result.crashed) {
                continue;
            }

            ++finished;
            if (result.timedOut) {
                ++timeouts;
            }
            if (!result.configMismatch &&
                result.exitCode == 0 &&
                result.failed == 0) {
                ++passed;
            } else {
                ++failed;
            }
        }

        totalFinished += finished;
        totalPassed += passed;
        totalFailed += failed;
        totalTimeouts += timeouts;

        QString statusText = QStringLiteral("PENDING");
        QColor statusColor(216, 220, 226);
        if (variantConfigMismatch) {
            statusText = QStringLiteral("CFG MISMATCH");
            statusColor = QColor(255, 204, 153);
        } else if (finished > 0 && finished < totalPerVariant) {
            statusText = QStringLiteral("RUNNING");
            statusColor = QColor(255, 231, 153);
        } else if (finished == totalPerVariant && failed == 0) {
            statusText = QStringLiteral("PASS");
            statusColor = QColor(183, 225, 205);
        } else if (finished == totalPerVariant) {
            statusText = QStringLiteral("FAIL");
            statusColor = QColor(244, 204, 204);
        }

        ensureItem(resultsTab_.table, row, kResultColumnMode)->setText(variant.label);
        auto* configItem = ensureItem(resultsTab_.table, row, kResultColumnConfig);
        configItem->setText(configBanner);
        configItem->setToolTip(configBanner);
        ensureItem(resultsTab_.table, row, kResultColumnShadow)->setText(QString::number(variant.enableShadow));
        ensureItem(resultsTab_.table, row, kResultColumnAllow32)->setText(QString::number(variant.allow32Bit));
        ensureItem(resultsTab_.table, row, kResultColumnHeuristic)->setText(QString::number(variant.heuristic));
        ensureItem(resultsTab_.table, row, kResultColumnFinished)->setText(
            QStringLiteral("%1/%2").arg(finished).arg(totalPerVariant));
        ensureItem(resultsTab_.table, row, kResultColumnPassed)->setText(QString::number(passed));
        ensureItem(resultsTab_.table, row, kResultColumnFailed)->setText(QString::number(failed));
        ensureItem(resultsTab_.table, row, kResultColumnTimeouts)->setText(QString::number(timeouts));
        setStatusVisual(
            ensureItem(resultsTab_.table, row, kResultColumnStatus),
            statusText,
            statusColor);

        for (int column = kResultColumnShadow; column <= kResultColumnTimeouts; ++column) {
            ensureItem(resultsTab_.table, row, column)->setTextAlignment(Qt::AlignCenter);
        }
        ensureItem(resultsTab_.table, row, kResultColumnMode)->setTextAlignment(Qt::AlignLeft | Qt::AlignVCenter);
        ensureItem(resultsTab_.table, row, kResultColumnConfig)->setTextAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    }

    QString summaryText =
        QStringLiteral("Finished: %1/%2  |  Passed: %3  |  Failed: %4  |  Timeouts: %5")
            .arg(totalFinished)
            .arg(variants.size() * totalPerVariant)
            .arg(totalPassed)
            .arg(totalFailed)
            .arg(totalTimeouts);
    if (!lastSummaryPath_.isEmpty()) {
        summaryText += QStringLiteral("\nSummary file: %1").arg(lastSummaryPath_);
    }
    resultsTab_.summaryLabel->setText(summaryText);
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
        if (!result.configMismatch &&
            result.exitCode == 0 &&
            result.failed == 0) {
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
    updateResultsTab();
}

QString MainWindow::summaryReportText() const
{
    QString text;
    QTextStream out(&text);

    const QVector<TestVariant> variants = configuredVariants();
    const int totalPerVariant = all_test_suite_names().size();

    out << "SPSC Launcher Summary\n";
    out << "Generated: "
        << QDateTime::currentDateTime().toString(Qt::ISODate)
        << '\n';
    out << "Application dir: " << QCoreApplication::applicationDirPath() << '\n';
    out << "Per-suite timeout: " << (timeoutSpin_->value() * 1000) << " ms\n\n";

    for (const TestVariant& variant : variants) {
        int finished = 0;
        int passed = 0;
        int failed = 0;
        int timeouts = 0;
        bool variantConfigMismatch = false;
        QString configBanner = QStringLiteral("-");

        for (const TestSuiteResult& result : suiteResults_) {
            if (result.variantId != variant.id) {
                continue;
            }
            if (!result.configBanner.trimmed().isEmpty()) {
                if (configBanner == QStringLiteral("-")) {
                    configBanner = result.configBanner;
                } else if (configBanner != result.configBanner) {
                    variantConfigMismatch = true;
                    configBanner = QStringLiteral("MULTIPLE CONFIG BANNERS");
                }
            }
            if (result.configMismatch) {
                variantConfigMismatch = true;
            }
            if (result.exitCode == -1 && !result.timedOut && !result.crashed) {
                continue;
            }

            ++finished;
            if (result.timedOut) {
                ++timeouts;
            }
            if (!result.configMismatch &&
                result.exitCode == 0 &&
                result.failed == 0) {
                ++passed;
            } else {
                ++failed;
            }
        }

        QString statusText = QStringLiteral("PENDING");
        if (variantConfigMismatch) {
            statusText = QStringLiteral("CFG MISMATCH");
        } else if (finished > 0 && finished < totalPerVariant) {
            statusText = QStringLiteral("RUNNING");
        } else if (finished == totalPerVariant && failed == 0) {
            statusText = QStringLiteral("PASS");
        } else if (finished == totalPerVariant) {
            statusText = QStringLiteral("FAIL");
        }

        out << variant.label << '\n';
        out << "  Status: " << statusText << '\n';
        out << "  Finished: " << finished << '/' << totalPerVariant << '\n';
        out << "  Passed: " << passed << '\n';
        out << "  Failed: " << failed << '\n';
        out << "  Timeouts: " << timeouts << '\n';
        out << "  Config: " << configBanner << '\n';
        out << "  Expected macros: "
            << "enable_shadow=" << variant.enableShadow
            << ", allow_32bit=" << variant.allow32Bit
            << ", refresh_heuristic=" << variant.heuristic
            << '\n';

        for (const TestSuiteResult& result : suiteResults_) {
            if (result.variantId == variant.id && result.configMismatch) {
                out << "  Config mismatch in " << result.suiteName << ": "
                    << result.configError << '\n';
            }
        }
        out << '\n';
    }

    return text;
}

QString MainWindow::writeSummaryReport() const
{
    const QString path =
        QDir(QCoreApplication::applicationDirPath()).filePath(
            QStringLiteral("spsc_launcher_last_summary.txt"));

    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate)) {
        return QString();
    }

    QTextStream out(&file);
    out << summaryReportText();
    file.close();
    return path;
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

    resetTabs();
    updateSummaryLabel();
    for (VariantTab& variantTab : variantTabs_) {
        variantTab.logOutput->clear();
        variantTab.logOutput->appendPlainText(
            QStringLiteral("[%1]\nStarting paranoid SPSC run for this variant...\nPer-suite timeout: %2 ms\n")
                .arg(variantTab.label)
                .arg(timeoutMs));
    }

    isRunning_ = true;
    runButton_->setEnabled(false);
    timeoutSpin_->setEnabled(false);
    if (resultsTab_.page != nullptr) {
        tabWidget_->setCurrentWidget(resultsTab_.page);
    }

    for (int resultIndex = 0; resultIndex < specs.size(); ++resultIndex) {
        const TestSuiteSpec& spec = specs[resultIndex];
        VariantTab* variantTab = findVariantTab(spec.variantId);
        const int row = suiteRowForName(spec.suiteName);

        setRowRunning(spec);
        statusBar()->showMessage(
            QStringLiteral("Running %1...")
                .arg(displayName(spec.variantLabel, spec.suiteName)));
        if (variantTab != nullptr) {
            tabWidget_->setCurrentWidget(variantTab->page);
            if (row >= 0) {
                variantTab->suiteTable->selectRow(row);
                variantTab->suiteTable->scrollToItem(
                    variantTab->suiteTable->item(row, kColumnSuite));
            }
        }
        QCoreApplication::processEvents();

        const TestSuiteResult result = runSuite(spec, timeoutMs);
        suiteResults_[resultIndex] = result;
        setRowResult(result);
        updateSummaryLabel();
        appendLogEntry(result);

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
        if (result.timedOut ||
            result.crashed ||
            result.configMismatch ||
            result.exitCode != 0 ||
            result.failed > 0) {
            ++failedSuites;
        }
    }

    statusBar()->showMessage(
        (failedSuites == 0)
            ? QStringLiteral("All suite variants passed")
            : QStringLiteral("%1 suite variant run(s) failed, %2 timed out")
                  .arg(failedSuites)
                  .arg(timedOutSuites));
    lastSummaryPath_ = writeSummaryReport();
    updateResultsTab();
    if (resultsTab_.page != nullptr) {
        tabWidget_->setCurrentWidget(resultsTab_.page);
    }
}
