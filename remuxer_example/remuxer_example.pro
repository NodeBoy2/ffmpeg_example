TEMPLATE = app
CONFIG += console c++11
CONFIG -= app_bundle
CONFIG -= qt

INCLUDEPATH += \
    $(FFMPEG_ROOT)/include


LIBS += -L$(FFMPEG_ROOT)/lib -lavformat -lavutil

SOURCES += \
        main.cpp
