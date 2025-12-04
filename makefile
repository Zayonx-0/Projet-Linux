# Detect OS
UNAME := $(shell uname)
WSL   := $(shell grep -qi microsoft /proc/version && echo 1 || echo 0)

# Default values (Linux natif)
CFLAGS ?= -O2 -Wall -Wextra -pedantic -std=gnu11
LIBS    = -lpthread -lrt

# macOS adjustment
ifeq ($(UNAME),Darwin)
    CFLAGS = -O2 -Wall -Wextra -pedantic -std=c11
    LIBS   = -lpthread
endif

# WSL adjustment (overrides standard Linux)
ifeq ($(WSL),1)
    CFLAGS = -O2 -Wall -Wextra -pedantic -std=gnu11
    LIBS   = -lpthread -lrt
endif

# Binaries
SRV = ServeurISY
GRP = GroupeISY
CLI = ClientISY

# Sources
SRV_SRC = src/ServeurISY.c
GRP_SRC = src/GroupeISY.c
CLI_SRC = src/ClientISY.c

# Default rule
all: $(SRV) $(GRP) $(CLI)

$(SRV): $(SRV_SRC)
	$(CC) $(CFLAGS) -o $@ $^ $(LIBS)

$(GRP): $(GRP_SRC)
	$(CC) $(CFLAGS) -o $@ $^ $(LIBS)

$(CLI): $(CLI_SRC)
	$(CC) $(CFLAGS) -o $@ $^ $(LIBS)

clean:
	rm -f $(SRV) $(GRP) $(CLI)
