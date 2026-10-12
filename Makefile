CC = gcc
CFLAGS = -O2 -Wall -Wextra
PREFIX = $(HOME)/.local
UNITDIR = $(HOME)/.config/systemd/user

gcpdot-music: gcpdot-music.c
	$(CC) $(CFLAGS) -o $@ $< $(shell pkg-config --cflags --libs libsystemd) -lm

install: gcpdot-music
	install -Dm755 gcpdot-music $(PREFIX)/bin/gcpdot-music
	install -Dm644 gcpdot-music.service $(UNITDIR)/gcpdot-music.service
	systemctl --user daemon-reload

clean:
	rm -f gcpdot-music

.PHONY: install clean
