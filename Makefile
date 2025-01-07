INSTALL_PATH = /usr/local/bin

all: janus-key

janus-key: janus-key.c janus-key.h config.h
	gcc `pkg-config --cflags libevdev` $< `pkg-config --libs libevdev` -o $@

.PHONY: install clean

install: janus-key
	cp $< $(INSTALL_PATH)
	cp janus-key.service /etc/systemd/system/
	systemctl enable janus-key.service
	systemctl start janus-key.service

uninstall:
	systemctl stop janus-key.service
	rm /etc/systemd/system/janus-key.service
	rm /usr/local/bin/janus-key

clean:
	rm janus-key
