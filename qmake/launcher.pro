QT += core gui widgets
CONFIG += console c++17
TEMPLATE = app
TARGET = spsc_launcher

CONFIG(debug, debug|release) {
    BUILD_CONFIG_NAME = debug
} else {
    BUILD_CONFIG_NAME = release
}

INCLUDEPATH += \
    $$PWD/..

DEPENDPATH += \
    $$PWD/..

DESTDIR = $$OUT_PWD/../bin/$${BUILD_CONFIG_NAME}
# Match test runners: generated paths must remain relative to the isolated
# qmake output directory so a fresh build never reuses stale MOC/object state.
OBJECTS_DIR = .obj/$${BUILD_CONFIG_NAME}/$${TARGET}
MOC_DIR = .moc/$${BUILD_CONFIG_NAME}/$${TARGET}
RCC_DIR = .rcc/$${BUILD_CONFIG_NAME}/$${TARGET}
UI_DIR = .ui/$${BUILD_CONFIG_NAME}/$${TARGET}

SOURCES += \
    ../main.cpp \
    ../mainwindow.cpp \
    ../test_suite_catalog.cpp

HEADERS += \
    ../mainwindow.h \
    ../test_suite_catalog.h \
    ../test_suite_entries.h

FORMS += \
    ../mainwindow.ui
