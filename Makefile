SRCS=sdlSpeedometer.c i2cSpeedometer.c
HDRS=sdlSpeedometer.h LSM9DS0.h
BIN=sdlSpeedometer
CC=gcc
DEST=/usr/local
GRP=$(shell id -gn)

GETC=".git/HEAD"
SMDB=speedometer.db

ifeq ($(shell test -e /usr/include/i2c/smbus.h && echo -n yes),yes)
CFLAGS+=-DHAS_SMBUS_H
LDFLAGS+=-li2c
endif

ifeq ($(shell test -e $(GETC) && echo -n yes),yes)
CFLAGS+=-DREV=\"$(shell git log --pretty=format:'%h' -n 1 2>/dev/null)\"
endif

LDFLAGS+=-lSDL2 -lSDL2_image -lSDL2_ttf -lSDL2_net -lsqlite3 -lcurl -lm -lvncserver

ifeq ($(shell test -e /usr/local/include/plotsdl/plot.h && echo -n yes),yes)
CFLAGS+=-DPLOTSDL
LDFLAGS+=-lplotsdl
endif

all: $(BIN)

override CFLAGS+= -Wall -g -std=gnu99 

$(BIN): $(SRCS) $(HDRS)
	$(CC) $(SRCS) $(CFLAGS) $(EXTRA_CFLAGS) $(LDFLAGS) -o $(BIN)

install:
	rm -f $(BIN)
	make $(BIN)  EXTRA_CFLAGS="-DPATH_INSTALL -O0 -Wno-stringop-truncation"
	sudo install -m 0755 -g root -o root $(BIN) -D $(DEST)/bin/$(BIN)
	sudo install -m 0755 -g root -o root spawnSubtask -D $(DEST)/bin/spawnSubtask
	sudo install -m 0755 -g root -o root sdlSpeedometer-config -D $(DEST)/bin/sdlSpeedometer-config
	sudo install -m 0755 -g root -o root sdlSpeedometer-reset -D $(DEST)/bin/sdlSpeedometer-reset
	sudo install -m 0755 -g root -o root sdlSpeedometer-stat -D $(DEST)/bin/sdlSpeedometer-stat
	sudo install -m 0755 -g root -o root sdlSpeedometer-kiosk -D $(DEST)/bin/sdlSpeedometer-kiosk
	sudo mkdir -p $(DEST)/share/images
	sudo install -m 0644 -g root -o root ./img/* -D $(DEST)/share/images
	sudo mkdir -p $(DEST)/share/sounds
	sudo install -m 0644 -g root -o root ./sounds/* -D $(DEST)/share/sounds
	sudo mkdir -p $(DEST)/etc/devilspie2
	sudo install -m 0644 -g root -o root ./devilspie2/* -D $(DEST)/etc/devilspie2
ifeq ($(shell test -e $(SMDB) && echo -n yes),yes)
	sudo mkdir -p $(DEST)/etc/speedometer
	sudo chown $$LOGNAME:$(GRP) $(DEST)/etc/speedometer
	sudo install -m 0664 -g $(GRP) -o $$LOGNAME $(SMDB) -D $(DEST)/etc/speedometer
endif
ifeq ($(shell grep "define DIGIFLOW" sdlSpeedometer.h | cut -c2-7 | tr -d '\n'),define)
	sudo install -m 0755 -g root -o root digiflow.sh -D $(DEST)/bin/digiflow.sh
endif

install_x:
	-sudo systemctl stop xorg.service sdlSpeedometer.service 
	cp sdlSpeedometer_x.env /tmp
	echo "XDG_RUNTIME_DIR=/run/user/$$(id -u)" >> /tmp/sdlSpeedometer_x.env
	-sudo install -m 0644 -g root -o root /tmp/sdlSpeedometer_x.env -D /etc/default/sdlSpeedometer
	echo "d	/run/user/$$(id -u)	0700	$$(id -un)	$$(id -gn)	-	- " > /tmp/headless.conf
	-sudo install -m 0644 -g root -o root /tmp/headless.conf -D /etc/tmpfiles.d/headless.conf
	echo "XDG_RUNTIME_DIR=/run/user/$$(id -u)" >> /tmp/xorg.env
	-sudo install -m 0644 -g root -o root /tmp/xorg.env -D /etc/default/xorg
	-sudo install -m 0644 -g root -o root xorg.service -D /lib/systemd/system/
	 sed s/root/$$LOGNAME/ sdlSpeedometer.service > /tmp/sdlSpeedometer.service
	-sudo install -m 0644 -g root -o root /tmp/sdlSpeedometer.service -D /lib/systemd/system/
	-sudo systemctl daemon-reload
	-sudo systemctl enable xorg.service sdlSpeedometer.service
	-sudo systemctl start xorg.service

install_wayland:
	-sudo systemctl stop sdlSpeedometer.service xorg.service
	-sudo systemctl disable sdlSpeedometer.service xorg.service
	-sudo install -m 0644 -g root -o root sdlSpeedometer.service -D /lib/systemd/system/
	-sudo install -m 0644 -g root -o root sdlSpeedometer.env -D /etc/default/sdlSpeedometer
	 cp sdlSpeedometer.env /tmp
	 echo "XDG_RUNTIME_DIR=/run/user/$$(id -u)" >> /tmp/sdlSpeedometer.env
	-sudo install -m 0644 -g root -o root /tmp/sdlSpeedometer.env -D /etc/default/sdlSpeedometer
	 echo "d	/run/user/$$(id -u)	0700	$$(id -un)	$$(id -gn)	-	- " > /tmp/headless.conf
	-sudo install -m 0644 -g root -o root /tmp/headless.conf -D /etc/tmpfiles.d/headless.conf
	-sudo systemctl daemon-reload
	-sudo systemctl enable sdlSpeedometer.service

clean:
	rm -f $(BIN) *~

stop:
	-sudo systemctl stop sdlSpeedometer.service || true
	-sudo systemctl status sdlSpeedometer.service --no-pager -l || true

start:
	-sudo systemctl start sdlSpeedometer.service
	-systemctl status sdlSpeedometer.service --no-pager -l || true

status:
	-systemctl status sdlSpeedometer.service --no-pager -l || true

disable:
	-sudo systemctl disable sdlSpeedometer.service

enable:
	-sudo systemctl enable sdlSpeedometer.service

