# Makefile — macOS / WSL / Linux (avec AffichageISY)

CC ?= cc

# Detect OS
UNAME := $(shell uname)
WSL   := $(shell grep -qi microsoft /proc/version 2>/dev/null && echo 1 || echo 0)

# Default (Linux natif)
CFLAGS ?= -O2 -Wall -Wextra -pedantic -std=gnu11
LIBS    = -lpthread -lrt
PLATFORM_MSG = "[Make] Building for Linux"

# macOS
ifeq ($(UNAME),Darwin)
  CFLAGS = -O2 -Wall -Wextra -pedantic -std=c11
  LIBS   = -lpthread
  PLATFORM_MSG = "[Make] Building for macOS"
endif

# WSL (reste Linux, mais on garde le message spécifique)
ifeq ($(WSL),1)
  CFLAGS = -O2 -Wall -Wextra -pedantic -std=gnu11
  LIBS   = -lpthread -lrt
  PLATFORM_MSG = "[Make] Building for WSL"
endif

SRC = src

BIN = ServeurISY GroupeISY ClientISY AffichageISY

all: info $(BIN)

info:
	@echo $(PLATFORM_MSG)

ServeurISY: $(SRC)/ServeurISY.c $(SRC)/Commun.h
	$(CC) $(CFLAGS) -o $@ $(SRC)/ServeurISY.c $(LIBS)

GroupeISY: $(SRC)/GroupeISY.c $(SRC)/Commun.h
	$(CC) $(CFLAGS) -o $@ $(SRC)/GroupeISY.c $(LIBS)

ClientISY: $(SRC)/ClientISY.c $(SRC)/Commun.h
	$(CC) $(CFLAGS) -o $@ $(SRC)/ClientISY.c $(LIBS)

AffichageISY: $(SRC)/AffichageISY.c $(SRC)/Commun.h
	$(CC) $(CFLAGS) -o $@ $(SRC)/AffichageISY.c $(LIBS)

clean:
	rm -f $(BIN)
