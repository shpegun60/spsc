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
OBJECTS_DIR = $$OUT_PWD/.obj/$${BUILD_CONFIG_NAME}/$${TARGET}
MOC_DIR = $$OUT_PWD/.moc/$${BUILD_CONFIG_NAME}/$${TARGET}
RCC_DIR = $$OUT_PWD/.rcc/$${BUILD_CONFIG_NAME}/$${TARGET}
UI_DIR = $$OUT_PWD/.ui/$${BUILD_CONFIG_NAME}/$${TARGET}

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
