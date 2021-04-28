TEMPLATE = app
CONFIG -= app_bundle
CONFIG -= qt

INCLUDEPATH += \
    $(FFMPEG_ROOT)/include


LIBS += -L$(FFMPEG_ROOT)/lib -lavformat -lavcodec -lavutil -lswresample

SOURCES += \
        main.cpp

