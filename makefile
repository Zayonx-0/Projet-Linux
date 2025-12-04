# Detect OS
UNAME := $(shell uname)
WSL   := $(shell grep -qi microsoft /proc/version && echo 1 || echo 0)

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

# WSL
ifeq ($(WSL),1)
    CFLAGS = -O2 -Wall -Wextra -pedantic -std=gnu11
    LIBS   = -lpthread -lrt
    PLATFORM_MSG = "[Make] Building for WSL"
endif

CC ?= cc
SRC = src

all:
	@echo $(PLATFORM_MSG)
	$(MAKE) ServeurISY
	$(MAKE) GroupeISY
	$(MAKE) ClientISY

ServeurISY: $(SRC)/ServeurISY.c $(SRC)/Commun.h
	$(CC) $(CFLAGS) -o $@ $(SRC)/ServeurISY.c $(LIBS)

GroupeISY: $(SRC)/GroupeISY.c $(SRC)/Commun.h
	$(CC) $(CFLAGS) -o $@ $(SRC)/GroupeISY.c $(LIBS)

ClientISY: $(SRC)/ClientISY.c $(SRC)/Commun.h
	$(CC) $(CFLAGS) -o $@ $(SRC)/ClientISY.c $(LIBS)

clean:
	rm -f ServeurISY GroupeISY ClientISY
