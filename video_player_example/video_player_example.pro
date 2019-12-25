TEMPLATE = app
CONFIG += console
CONFIG -= app_bundle
CONFIG -= qt

INCLUDEPATH += \
    $(FFMPEG_ROOT)/include \
    $(SDL_ROOT)/include


LIBS += -L$(FFMPEG_ROOT)/lib -lavformat -lavcodec -lavutil -lswscale
LIBS += -L$(SDL_ROOT)/lib -lSDL2main -lSDL2

SOURCES += \
        main.cpp

