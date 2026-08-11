TEMPLATE = lib
TARGET = percomedia
CONFIG += silent
QT -= gui
QT += core gui

DEFINES += PERCOMEDIA_LIBRARY

SOURCES += percomedia_stub.cpp

HEADERS += \
    percomedia.h \
    cb.h \
    percomedia_global.h \
    percomedia_config.h
