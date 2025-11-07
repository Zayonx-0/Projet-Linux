CC ?= cc
CFLAGS ?= -O2 -Wall -Wextra -pedantic -std=c11
LIBS = -lpthread

SRC = src

all: ServeurISY GroupeISY ClientISY AffichageISY

ServeurISY: $(SRC)/ServeurISY.c $(SRC)/Commun.h
	$(CC) $(CFLAGS) -o $@ $(SRC)/ServeurISY.c $(LIBS)

GroupeISY: $(SRC)/GroupeISY.c $(SRC)/Commun.h
	$(CC) $(CFLAGS) -o $@ $(SRC)/GroupeISY.c $(LIBS)

ClientISY: $(SRC)/ClientISY.c $(SRC)/Commun.h
	$(CC) $(CFLAGS) -o $@ $(SRC)/ClientISY.c $(LIBS)

AffichageISY: $(SRC)/AffichageISY.c $(SRC)/Commun.h
	$(CC) $(CFLAGS) -o $@ $(SRC)/AffichageISY.c $(LIBS)

clean:
	rm -f ServeurISY GroupeISY ClientISY AffichageISY
