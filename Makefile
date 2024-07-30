pwd = $(shell pwd)

all: build_in_docker

clean:
	rm tcpserial_hook.so

build_in_docker:
	docker run -it --rm -e CC=arm-linux-gnueabihf-gcc -v "$(pwd):$(pwd)" -w "$(pwd)" --platform linux/amd64 ghcr.io/cross-rs/armv7-unknown-linux-gnueabihf make V=1 tcpserial_hook.so

shell:
	docker run -it --rm -e CC=arm-linux-gnueabihf-gcc -v "$(pwd):$(pwd)" -w "$(pwd)" --platform linux/amd64 ghcr.io/cross-rs/armv7-unknown-linux-gnueabihf

tcpserial_hook.so: tcpserial_hook.c Makefile
	$(CC) -Os -shared -Wall -fPIC -o $@ $< -lpthread -ldl
