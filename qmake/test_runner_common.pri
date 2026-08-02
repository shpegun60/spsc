QT += core testlib
CONFIG += console c++17
TEMPLATE = app
DEFINES += SPSC_TESTS_WITH_QT=1
DEFINES += SPSC_HAS_SPAN=0
DEFINES += SPSC_TEST_ACTUAL_TARGET_NAME=\\\"$${TARGET}\\\"

CONFIG(debug, debug|release) {
    BUILD_CONFIG_NAME = debug
} else {
    BUILD_CONFIG_NAME = release
    DEFINES += NDEBUG
}

INCLUDEPATH += \
    $$PWD/.. \
    $$PWD/../src \
    $$PWD/../src/tests

DESTDIR = $$OUT_PWD/../bin/$${BUILD_CONFIG_NAME}
OBJECTS_DIR = $$OUT_PWD/.obj/$${BUILD_CONFIG_NAME}/$${TARGET}
MOC_DIR = $$OUT_PWD/.moc/$${BUILD_CONFIG_NAME}/$${TARGET}
RCC_DIR = $$OUT_PWD/.rcc/$${BUILD_CONFIG_NAME}/$${TARGET}
UI_DIR = $$OUT_PWD/.ui/$${BUILD_CONFIG_NAME}/$${TARGET}

INCLUDEPATH += $$MOC_DIR

DEPENDPATH += \
    $$PWD/../src \
    $$PWD/../src/tests

include(../src/spsc/spsc.pri)

SOURCES += \
    ../test_runner_main.cpp \
    ../test_suite_catalog.cpp \
    ../test_suite_registry.cpp \
    ../src/tests/buffer_pool_test.cpp \
    ../src/tests/chunk_test.cpp \
    ../src/tests/latest_test.cpp \
    ../src/tests/pool_view_test.cpp \
    ../src/tests/fifo_test.cpp \
    ../src/tests/fifo_view_test.cpp \
    ../src/tests/pool_test.cpp \
    ../src/tests/queue_test.cpp \
    ../src/tests/typed_pool_test.cpp

HEADERS += \
    ../test_suite_catalog.h \
    ../test_suite_entries.h \
    ../test_suite_registry.h \
    ../basic_types.h \
    ../macro.h \
    ../src/tests/test_config.hpp \
    ../src/tests/test_build_config.hpp \
    ../src/tests/test_spsc_layout.hpp \
    ../src/tests/buffer_pool_test.h \
    ../src/tests/chunk_test.h \
    ../src/tests/queue_test.h \
    ../src/tests/latest_test.h \
    ../src/tests/pool_view_test.h \
    ../src/tests/fifo_test.h \
    ../src/tests/fifo_view_test.h \
    ../src/tests/pool_test.h \
    ../src/tests/typed_pool_test.h
