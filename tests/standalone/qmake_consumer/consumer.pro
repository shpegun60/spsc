TEMPLATE = app
TARGET = spsc_pri_consumer

CONFIG += console c++17
CONFIG -= app_bundle qt

include(../../../src/spsc/spsc.pri)

SOURCES += main.cpp
