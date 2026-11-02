CC      = cc
CFLAGS  = -std=c11 -Wall -Wextra -O2 -Isrc
LDFLAGS =

# raylib : pkg-config si dispo, sinon via Homebrew
RAYLIB_PREFIX = $(shell brew --prefix raylib 2>/dev/null)
RAYLIB_CFLAGS = $(shell pkg-config --cflags raylib 2>/dev/null || echo -I$(RAYLIB_PREFIX)/include)
RAYLIB_LIBS   = $(shell pkg-config --libs raylib 2>/dev/null || echo -L$(RAYLIB_PREFIX)/lib -lraylib)

BUILD = build

COMMON_SRC = src/common/game.c

all: $(BUILD)/server $(BUILD)/client $(BUILD)/bot

$(BUILD):
	mkdir -p $(BUILD)

# Le serveur est "headless" : il ne dépend pas de raylib
$(BUILD)/server: src/server/main.c $(COMMON_SRC) src/common/protocol.h src/common/game.h | $(BUILD)
	$(CC) $(CFLAGS) -o $@ src/server/main.c $(COMMON_SRC) -lm

CLIENT_SRC = src/client/main.c src/client/models.c

$(BUILD)/client: $(CLIENT_SRC) src/client/models.h $(COMMON_SRC) src/common/protocol.h src/common/game.h | $(BUILD)
	$(CC) $(CFLAGS) $(RAYLIB_CFLAGS) -o $@ $(CLIENT_SRC) $(COMMON_SRC) $(RAYLIB_LIBS) -lm

# Le bot zombie est headless lui aussi : pas de raylib
$(BUILD)/bot: src/bot/main.c $(COMMON_SRC) src/common/protocol.h src/common/game.h | $(BUILD)
	$(CC) $(CFLAGS) -o $@ src/bot/main.c $(COMMON_SRC) -lm

clean:
	rm -rf $(BUILD)

.PHONY: all clean
