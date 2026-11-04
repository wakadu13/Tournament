# EMA Tournament — serveur dedie, client raylib et bot zombie.
#
#   make                     tout compiler (raylib installee sur le systeme)
#   make RAYLIB_PATH=...     raylib compilee depuis ses sources
#   make server              le serveur seul (aucune dependance a raylib)
#   make windows             .exe Windows par cross-compilation MinGW
#                            (RAYLIB_WIN_PATH=/chemin/raylib-windows)
#   make clean
#
# L'APK Android se construit avec scripts/build_android.sh (voir le README).

CC      ?= cc
CFLAGS  += -std=c11 -Wall -Wextra -O2 -Isrc
BUILD    = build

UNAME_S := $(shell uname -s 2>/dev/null)

# --- raylib : pkg-config, sinon Homebrew, sinon des sources ---------------
ifdef RAYLIB_PATH
    RAYLIB_CFLAGS = -I$(RAYLIB_PATH)/src
    RAYLIB_LIBS   = -L$(RAYLIB_PATH)/src -lraylib
else
    RAYLIB_PREFIX = $(shell brew --prefix raylib 2>/dev/null)
    RAYLIB_CFLAGS = $(shell pkg-config --cflags raylib 2>/dev/null || echo -I$(RAYLIB_PREFIX)/include)
    RAYLIB_LIBS   = $(shell pkg-config --libs raylib 2>/dev/null || echo -L$(RAYLIB_PREFIX)/lib -lraylib)
endif

# --- Bibliotheques systeme, par plateforme -------------------------------
# WIN_LIBS : Winsock, exige par le serveur, le client ET le bot sous Windows.
ifeq ($(OS),Windows_NT)
    CLIENT_LIBS = -lopengl32 -lgdi32 -lwinmm
    NET_LIBS    = -lws2_32
    EXE         = .exe
else ifeq ($(UNAME_S),Darwin)
    CLIENT_LIBS = -framework OpenGL -framework Cocoa -framework IOKit \
                  -framework CoreVideo -framework CoreAudio
    NET_LIBS    =
    EXE         =
else
    CLIENT_LIBS = -lGL -lpthread -ldl -lrt -lX11
    NET_LIBS    =
    EXE         =
endif

COMMON_SRC = src/common/game.c
COMMON_HDR = src/common/protocol.h src/common/game.h src/common/platform.h
CLIENT_SRC = src/client/main.c src/client/models.c src/client/weapons.c src/client/touch.c \
             src/client/ui.c src/client/menu.c src/client/options.c
CLIENT_HDR = src/client/models.h src/client/weapons.h src/client/touch.h \
             src/client/ui.h src/client/menu.h src/client/options.h

all: $(BUILD)/server$(EXE) $(BUILD)/client$(EXE) $(BUILD)/bot$(EXE)

server: $(BUILD)/server$(EXE)
client: $(BUILD)/client$(EXE)
bot:    $(BUILD)/bot$(EXE)

$(BUILD):
	mkdir -p $(BUILD)

# Le serveur est "headless" : il ne depend pas de raylib, c'est ce qui lui
# permet de tourner sur un VPS Linux sans affichage.
$(BUILD)/server$(EXE): src/server/main.c $(COMMON_SRC) $(COMMON_HDR) | $(BUILD)
	$(CC) $(CFLAGS) -o $@ src/server/main.c $(COMMON_SRC) $(NET_LIBS) -lm

$(BUILD)/client$(EXE): $(CLIENT_SRC) $(CLIENT_HDR) $(COMMON_SRC) $(COMMON_HDR) | $(BUILD)
	$(CC) $(CFLAGS) $(RAYLIB_CFLAGS) -o $@ $(CLIENT_SRC) $(COMMON_SRC) \
		$(RAYLIB_LIBS) $(CLIENT_LIBS) $(NET_LIBS) -lm

# Le bot zombie est headless lui aussi : pas de raylib
$(BUILD)/bot$(EXE): src/bot/main.c $(COMMON_SRC) $(COMMON_HDR) | $(BUILD)
	$(CC) $(CFLAGS) -o $@ src/bot/main.c $(COMMON_SRC) $(NET_LIBS) -lm

# --- Executables Windows autonomes (cross-compilation MinGW) -------------
#   make windows RAYLIB_WIN_PATH=/chemin/raylib-windows
# -static : l'exe n'a besoin d'aucune DLL. -mwindows : pas de console noire
# derriere le jeu (le serveur et le bot, eux, GARDENT leur console : c'est
# la qu'ils affichent les connexions et les eliminations).
MINGW_CC ?= x86_64-w64-mingw32-gcc
WIN_OUT  ?= dist
windows: $(CLIENT_SRC) $(CLIENT_HDR) src/server/main.c src/bot/main.c $(COMMON_SRC) $(COMMON_HDR)
	mkdir -p $(WIN_OUT)
	$(MINGW_CC) -std=c11 -O2 -Wall -Wextra -Isrc -I$(RAYLIB_WIN_PATH)/src \
		$(CLIENT_SRC) $(COMMON_SRC) -o $(WIN_OUT)/EMATournament-windows-x64.exe \
		-L$(RAYLIB_WIN_PATH)/src -lraylib -lopengl32 -lgdi32 -lwinmm -lws2_32 \
		-static -static-libgcc -mwindows -lm
	$(MINGW_CC) -std=c11 -O2 -Wall -Wextra -Isrc \
		src/server/main.c $(COMMON_SRC) -o $(WIN_OUT)/EMATournament-server-windows-x64.exe \
		-lws2_32 -static -static-libgcc -lm
	$(MINGW_CC) -std=c11 -O2 -Wall -Wextra -Isrc \
		src/bot/main.c $(COMMON_SRC) -o $(WIN_OUT)/EMATournament-bot-windows-x64.exe \
		-lws2_32 -static -static-libgcc -lm

clean:
	rm -rf $(BUILD) $(WIN_OUT) build-android

.PHONY: all server client bot windows clean
