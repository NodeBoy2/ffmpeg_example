TEMPLATE = app
CONFIG += console
CONFIG -= app_bundle
CONFIG -= qt

INCLUDEPATH += \
    $(FFMPEG_ROOT)/include


LIBS += -L$(FFMPEG_ROOT)/lib -lavformat -lavfilter -lavcodec -lavutil

SOURCES += \
        main.cpp

DISTFILES += \
    README.md

