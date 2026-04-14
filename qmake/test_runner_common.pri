QT += core testlib
CONFIG += console c++20
TEMPLATE = app
DEFINES += SPSC_TESTS_WITH_QT=1

INCLUDEPATH += \
    $$PWD/.. \
    $$PWD/../src \
    $$PWD/../src/tests

DEPENDPATH += \
    $$PWD/.. \
    $$PWD/../src \
    $$PWD/../src/tests

DESTDIR = $$OUT_PWD/../bin
OBJECTS_DIR = $$OUT_PWD/.obj/$${TARGET}
MOC_DIR = $$OUT_PWD/.moc/$${TARGET}
RCC_DIR = $$OUT_PWD/.rcc/$${TARGET}
UI_DIR = $$OUT_PWD/.ui/$${TARGET}

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
    ../test_suite_registry.h \
    ../basic_types.h \
    ../macro.h \
    ../src/tests/test_config.hpp \
    ../src/tests/test_build_config.hpp \
    ../src/tests/buffer_pool_test.h \
    ../src/tests/chunk_test.h \
    ../src/tests/queue_test.h \
    ../src/tests/latest_test.h \
    ../src/tests/pool_view_test.h \
    ../src/tests/fifo_test.h \
    ../src/tests/fifo_view_test.h \
    ../src/tests/pool_test.h \
    ../src/tests/typed_pool_test.h
