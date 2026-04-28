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

LDFLAGS+=-lX11 -lSDL2 -lSDL2_image -lSDL2_ttf -lSDL2_net -lsqlite3 -lcurl -lm -lvncserver
LDFLAGS+=-lasound -lavformat -lavcodec -lavutil -lswresample

RED=\033[1;31m
GREEN=\033[1;32m
RESET=\033[0m

all: $(BIN)

override CFLAGS+= -Wall -Wno-format-truncation -Wno-stringop-truncation -g -std=gnu99 -D_REENTRANT

$(BIN): $(SRCS) $(HDRS)
	$(CC) $(SRCS) $(CFLAGS) $(EXTRA_CFLAGS) $(LDFLAGS) -o $(BIN)

install:
	rm -f $(BIN)
	make $(BIN)  EXTRA_CFLAGS="-DPATH_INSTALL -O2 -Wno-stringop-truncation"
	sudo install -m 0755 -g root -o root $(BIN) -D $(DEST)/bin/$(BIN)
	sudo install -m 0755 -g root -o root spawnSubtask -D $(DEST)/bin/spawnSubtask
	sudo install -m 0755 -g root -o root sdlSpeedometer-config -D $(DEST)/bin/sdlSpeedometer-config
	sudo install -m 0755 -g root -o root sdlSpeedometer-reset -D $(DEST)/bin/sdlSpeedometer-reset
	sudo install -m 0755 -g root -o root sdlSpeedometer-stat -D $(DEST)/bin/sdlSpeedometer-stat
	sudo install -m 0755 -g root -o root sdlSpeedometer-browser -D $(DEST)/bin/sdlSpeedometer-browser
	sudo install -m 0755 -g root -o root sdlSpeedometer-venus -D $(DEST)/bin/sdlSpeedometer-venus
	sudo mkdir -p $(DEST)/share/images
	sudo install -m 0644 -g root -o root ./img/* -D $(DEST)/share/images
	sudo mkdir -p $(DEST)/share/sounds
	sudo install -m 0644 -g root -o root ./sounds/* -D $(DEST)/share/sounds

ifeq ($(shell test -e $(SMDB) && echo -n yes),yes)
	sudo mkdir -p $(DEST)/etc/speedometer
	sudo chown $$LOGNAME:$(GRP) $(DEST)/etc/speedometer
	sudo install -m 0664 -g $(GRP) -o $$LOGNAME $(SMDB) -D $(DEST)/etc/speedometer
endif
ifeq ($(shell grep "define DIGIFLOW" sdlSpeedometer.h | cut -c2-7 | tr -d '\n'),define)
	sudo install -m 0755 -g root -o root digiflow.sh -D $(DEST)/bin/digiflow.sh
endif

install_system:
	-sudo systemctl stop sdlSpeedometer.service labwc-kiosk.service
	-sudo install -m 0644 -g root -o root sdlSpeedometer.env -D /etc/default/sdlSpeedometer
	echo "d	/run/user/$$(id -u)	0700	$$(id -un)	$$(id -gn)	-	- " > /tmp/headless.conf
	-sudo install -m 0644 -g root -o root /tmp/headless.conf -D /etc/tmpfiles.d/headless.conf
	 sed s/NOTYET/$$LOGNAME/ sdlSpeedometer.service > /tmp/sdlSpeedometer.service
	-sudo install -m 0644 -g root -o root /tmp/sdlSpeedometer.service -D /lib/systemd/system/
	 sed s/NOTYET/$$LOGNAME/ labwc-kiosk.service > /tmp/labwc-kiosk.service
	-sudo install -m 0644 -g root -o root /tmp/labwc-kiosk.service -D /lib/systemd/system/
	-sudo systemctl daemon-reload

	@if [ -e $(HOME)/.config/labwc/rc.xml ]; then \
		/usr/bin/echo -e "$(RED)$(HOME)/.config/labwc/rc.xml exists, not overwriting$(RESET)"; \
	else \
		install -m 0644 -g $(GRP) -o $$LOGNAME labwc/rc.xml -D $(HOME)/.config/labwc/rc.xml; \
		/usr/bin/echo -e "$(GREEN)$(HOME)/.config/labwc/rc.xml installed successfully$(RESET)"; \
		install -m 0644 -g $(GRP) -o $$LOGNAME labwc/autostart -D $(HOME)/.config/labwc/autostart; \
		/usr/bin/echo -e "$(GREEN)$(HOME)/.config/labwc/autostart installed successfully$(RESET)"; \
		install -m 0644 -g $(GRP) -o $$LOGNAME labwc/environment -D $(HOME)/.config/labwc/environment; \
		/usr/bin/echo -e "$(GREEN)$(HOME)/.config/labwc/environment installed successfully$(RESET)"; \
		sed s/NOTYET/$$LOGNAME/ labwc/config > /tmp/config.tmp; \
		install -m 0644 -g $(GRP) -o $$LOGNAME /tmp/config.tmp -D $(HOME)/.config/wayvnc/config; \
		/usr/bin/echo -e "$(GREEN)$(HOME)/.config/wayvnc/config installed successfully$(RESET)"; \
	fi

	-systemctl --user mask pipewire.service pipewire.socket pipewire-pulse.service pipewire-pulse.socket wireplumber.service
	-systemctl --user stop pipewire.service pipewire-pulse.service wireplumber.service
	-sudo systemctl enable labwc-kiosk.service  sdlSpeedometer.service
	-sudo systemctl restart labwc-kiosk.service sdlSpeedometer.service

clean:
	rm -f $(BIN) *~

stop:
	-sudo systemctl stop sdlSpeedometer.service || true
	-sudo systemctl status sdlSpeedometer.service --no-pager -l || true

start:
	-sudo systemctl start sdlSpeedometer.service
	-systemctl status sdlSpeedometer.service --no-pager -l || true

restart:
	-sudo systemctl restart sdlSpeedometer.service
	-systemctl status sdlSpeedometer.service --no-pager -l || true

status:
	-systemctl status sdlSpeedometer.service --no-pager -l || true

disable:
	-sudo systemctl disable sdlSpeedometer.service

enable:
	-sudo systemctl enable sdlSpeedometer.service

