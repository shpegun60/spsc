#include "mainwindow.h"
#include "ui_mainwindow.h"

#include "src/fifo_test.h"
#include "src/fifo_bench.h"

#include "src/fifo_view_test.h"
#include "src/pool_test.h"
#include "src/pool_view_test.h"
#include "src/latest_test.h"
#include "src/queue_test.h"


MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
    , ui(new Ui::MainWindow)
{
    ui->setupUi(this);

    qDebug() << "\n" << "fifo test";
    run_tst_fifo_api_paranoid(-1, nullptr);

    qDebug() << "\n" << "fifo_view test";
    run_tst_fifo_view_api_paranoid(-1, nullptr);

    qDebug() << "\n" << "pool test";
    run_tst_pool_api_paranoid(false);

    qDebug() << "\n" << "pool_view test";
    run_tst_pool_view_api_paranoid(-1, nullptr);

    qDebug() << "\n" << "latest test";
    run_tst_latest_api_paranoid(-1, nullptr);

    qDebug() << "\n" << "queue test";
    run_tst_queue_api_paranoid(-1, nullptr);

    //run_fifo_bench();
}

MainWindow::~MainWindow()
{
    delete ui;
}
