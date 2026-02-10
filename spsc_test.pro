QT       += core gui
QT += testlib
CONFIG += console c++20
TEMPLATE = app

greaterThan(QT_MAJOR_VERSION, 4): QT += widgets

CONFIG += c++17

# You can make your code fail to compile if it uses deprecated APIs.
# In order to do so, uncomment the following line.
#DEFINES += QT_DISABLE_DEPRECATED_BEFORE=0x060000    # disables all the APIs deprecated before Qt 6.0.0

include(src/spsc/spsc.pri)

SOURCES += \
    main.cpp \
    mainwindow.cpp \
    src/chunk_test.cpp \
    src/latest_test.cpp \
    src/pool_view_test.cpp \
    src/fifo_bench.cpp \
    src/fifo_test.cpp \
    src/fifo_view_test.cpp \
    src/pool_test.cpp \
    src/queue_test.cpp \
    src/typed_pool_test.cpp

HEADERS += \
    mainwindow.h \
    basic_types.h \
    macro.h \
    src/chunk_test.h \
    src/queue_test.h \
    src/latest_test.h \
    src/pool_view_test.h \
    src/fifo_bench.h \
    src/fifo_test.h \
    src/fifo_view_test.h \
    src/pool_test.h \
    src/typed_pool_test.h

FORMS += \
    mainwindow.ui

# Default rules for deployment.
qnx: target.path = /tmp/$${TARGET}/bin
else: unix:!android: target.path = /opt/$${TARGET}/bin
!isEmpty(target.path): INSTALLS += target
