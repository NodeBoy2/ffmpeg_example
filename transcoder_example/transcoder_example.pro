TEMPLATE = app
CONFIG += console
CONFIG -= app_bundle
CONFIG -= qt

INCLUDEPATH += \
    $(FFMPEG_ROOT)/include


LIBS += -L$(FFMPEG_ROOT)/lib -lavformat -lavcodec -lavutil

SOURCES += \
        main.cpp

