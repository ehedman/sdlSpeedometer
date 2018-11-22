SRCS=sdlSpeedometer.c i2cSpeedometer.c
HDRS=sdlSpeedometer.h LSM9DS0.h
BIN=sdlSpeedometer
CC=gcc
DEST=/usr/local

GETC=".git/HEAD"
SMDB=speedometer.db

ifeq ($(shell test -e $(GETC) && echo -n yes),yes)
CFLAGS=-DREV=\"$(shell git log --pretty=format:'%h' -n 1 2>/dev/null)\"
endif

all: $(BIN)

override CFLAGS+= -Wall -g -std=gnu99
LDFLAGS=-lSDL2 -lSDL2_image -lSDL2_ttf -lSDL2_net -lsqlite3 -lcurl -lm

$(BIN): $(SRCS) $(HDRS)
	$(CC) $(SRCS) $(CFLAGS) $(LDFLAGS) -o $(BIN)

install:
	rm -f $(BIN)
	make $(BIN) CFLAGS="-DPATH_INSTALL"
	sudo install -m 0755 -g root -o root $(BIN) -D $(DEST)/bin/$(BIN)
	sudo mkdir -p $(DEST)/share/images
	sudo install -m 0644 -g root -o root ./img/* -D $(DEST)/share/images
	sudo install -m 0644 -g root -o root sdlSpeedometer.env -D /etc/default/sdlSpeedometer
ifeq ($(shell test -e $(SMDB) && echo -n yes),yes)
	sudo install -m 0644 -g root -o root $(SMDB) -D $(DEST)/etc
endif
	-sudo systemctl stop sdlSpeedometer.service
	-sudo systemctl disable sdlSpeedometer.service
	-sudo install -m 0644 -g root -o root sdlSpeedometer.service -D /lib/systemd/system/
	-sudo systemctl enable sdlSpeedometer.service
	
clean:
	rm -f $(BIN) *~

stop:
	-sudo systemctl stop sdlSpeedometer.service

start:
	-sudo systemctl start sdlSpeedometer.service

status:
	-sudo systemctl status sdlSpeedometer.service

disable:
	-sudo systemctl disable sdlSpeedometer.service

enable:
	-sudo systemctl enable sdlSpeedometer.service

