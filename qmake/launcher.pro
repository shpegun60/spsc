QT += core gui widgets
CONFIG += console c++20
TEMPLATE = app
TARGET = spsc_launcher

INCLUDEPATH += \
    $$PWD/..

DEPENDPATH += \
    $$PWD/..

DESTDIR = $$OUT_PWD/../bin
OBJECTS_DIR = $$OUT_PWD/.obj
MOC_DIR = $$OUT_PWD/.moc
RCC_DIR = $$OUT_PWD/.rcc
UI_DIR = $$OUT_PWD/.ui

SOURCES += \
    ../main.cpp \
    ../mainwindow.cpp \
    ../test_suite_catalog.cpp

HEADERS += \
    ../mainwindow.h \
    ../test_suite_catalog.h

FORMS += \
    ../mainwindow.ui
