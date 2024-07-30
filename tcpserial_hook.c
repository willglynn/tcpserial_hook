#define _GNU_SOURCE
#include <termios.h>
#include <unistd.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <memory.h>
#include <pthread.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>

#define MAX_CLIENTS 8

// We want to [re]define three functions such that our functions get called instead of the operating system:
//   read()
//   write()
//   tcsetattr()
//
// But our functions need to call the underlying functions, which means we need to find them and make them
// callable by us. And... well, lots of things use these functions, including the C standard library.
// Tread lightly.

// Prepare some function pointers.
ssize_t (*real_write)(int, const void *, size_t);
ssize_t (*real_read)(int, void *, size_t);
int (*real_tcsetattr)(int, int, const struct termios *);

// Prepare the place where we write the FD to monitor
int serial_fd __attribute__ ((aligned (8)));

// Some forward definitions
void broadcast(const char *buffer, size_t bytes);
int init_tcpserial_hook_output();

// Make an init function, annotated in a way that `ld-linux.so` will call our initializer before the application's
// entrypoint.
static void init_tcpserial_hook() __attribute__((constructor));
void init_tcpserial_hook() {
	// Indicate that the serial port we need to hook is not yet known
	serial_fd = -1;

	// Ask the dynamic linker to get the version of these functions that existed before we clobbered them
	real_read = dlsym(RTLD_NEXT, "read");
	real_write = dlsym(RTLD_NEXT, "write");
	real_tcsetattr = dlsym(RTLD_NEXT, "tcsetattr");
	if (!real_read || !real_write || !real_tcsetattr) {
		fprintf(stderr, "dlsym() failed: %s", dlerror());
		abort();
	}

    // Attempt to ensure we're not inherited by child processes
    unsetenv("LD_PRELOAD");

	// Set up the output
	if (init_tcpserial_hook_output()) {
		abort();
	}
}

ssize_t read(int fd, void *buf, size_t count) {
	ssize_t bytes;

    // read(2)
	bytes = real_read(fd, buf, count);

    // Load which FD is the serial port
    int local_serial_fd __attribute__ ((aligned (8)));
    __atomic_load(&serial_fd, &local_serial_fd, __ATOMIC_SEQ_CST);
	if (bytes > 0 && fd == local_serial_fd) {
		// We read some bytes from the serial port
        // Broadcast them
        broadcast(buf, bytes);
	}

    return bytes;
}

ssize_t write(int fd, const void *buf, size_t count) {
	ssize_t bytes;

    // write(2)
	bytes = real_write(fd, buf, count);

    // Load which FD is the serial port
    int local_serial_fd __attribute__ ((aligned (8)));
    __atomic_load(&serial_fd, &local_serial_fd, __ATOMIC_SEQ_CST);
	if (bytes > 0 && fd == local_serial_fd) {
        // We wrote some bytes to the serial port
        // Broadcast them
        broadcast(buf, bytes);
	}

    return bytes;
}

int tcsetattr(int fd, int optional_actions, const struct termios *termios_p) {
    // Store this FD as the serial port
    __atomic_store(&serial_fd, &fd, __ATOMIC_SEQ_CST);

    // Make the actual syscall
	return real_tcsetattr(fd, optional_actions, termios_p);
}

struct listener_state_t {
    int fd;
};
int client_fds[MAX_CLIENTS] __attribute__ ((aligned (8)));

void *listener_thread(void *listener_state);

int init_tcpserial_hook_output() {
    // Indicate that we have no clients
    for (int i = 0; i < MAX_CLIENTS; i++) {
        client_fds[i] = -1;
    }

    struct listener_state_t *listener_state = calloc(sizeof(struct listener_state_t), 1);

    listener_state->fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listener_state->fd < 0) {
        perror("socket");
        return 1;
    }

    // Bind on 0.0.0.0:7160
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(7160);
    if (bind(listener_state->fd, (struct sockaddr*) &addr, sizeof(addr)) < 0) {
        perror("bind");
        return 1;
    }

    // RST on close, allowing immediate listener reuse if `meshdcd` terminates
    struct linger lin;
    lin.l_onoff = 1;
    lin.l_linger = 0;
    if (setsockopt(listener_state->fd, SOL_SOCKET, SO_LINGER, (const char *)&lin, sizeof(struct linger)) < 0) {
        perror("setsockopt(SO_LINGER)");
        return 1;
    }

    // Listen for connections
    if (listen(listener_state->fd, MAX_CLIENTS) < 0) {
        perror("listen");
        return 1;
    }

    // Accept connections in the background
    pthread_t thread;
    if (pthread_create(&thread, NULL, listener_thread, listener_state)) {
        perror("pthread_create");
    }
    if (pthread_detach(thread)) {
        perror("pthread_create");
    }

    return 0;
}

void *listener_thread(void *listener_state_void) {
    struct listener_state_t *listener_state = listener_state_void;
    for (;;) {
        struct sockaddr_in addr;
        socklen_t addr_len = sizeof(addr);

        // Accept an incoming connection
        int client = accept(listener_state->fd, &addr, &addr_len);
        if (client < 0) {
            perror("accept");
            usleep(50000);
            continue;
        }

        // Set TCP_NODELAY on it
        int nodelay = 1;
        if (setsockopt(client, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay)) < 0) {
            perror("setsockopt(TCP_NODELAY)");
        }

        // Bump up the send buffer size
        int bufferSize = 256 << 10;
        if (setsockopt(client, SOL_SOCKET, SO_SNDBUF, &bufferSize, sizeof(bufferSize)) < 0) {
            perror("setsockopt(SO_SNDBUF)");
        }

        // RST on close, allowing immediate reuse if `meshdcd` terminates
        struct linger lin;
        lin.l_onoff = 1;
        lin.l_linger = 0;
        if (setsockopt(client, SOL_SOCKET, SO_LINGER, (const char *)&lin, sizeof(struct linger)) < 0) {
            perror("setsockopt(SO_LINGER)");
        }

        // Shut it down for reading, since we don't ever do that
        if (shutdown(client, SHUT_RD) < 0) {
            perror("shutdown");
        }

        // Attempt to store it in our client array
        int stored = 0;
        for (int i = 0; i < MAX_CLIENTS && !stored; i++) {
            // Do we get to store this client into this slot?
            int expected = -1;
            stored = __atomic_compare_exchange(&client_fds[i], &expected, &client, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
        }

        if (stored) {
            // We stored the child_fd, meaning it's broadcast()'s problem now
        } else {
            // We still own the socket but can't hold onto it
            // Hang up :(
            close(client);
        }
    }

    // Unreachable
    return NULL;
}

void broadcast(const char *buffer, size_t bytes) {
    // Loop over all our slots
    for (int i = 0; i < MAX_CLIENTS; i++) {
        // Load the FD
        int fd;
        __atomic_load(&client_fds[i], &fd, __ATOMIC_SEQ_CST);

        // Is this slot occupied by a file descriptor?
        if (fd >= 0) {
            // Attempt to write
            if (send(fd, buffer, bytes, MSG_NOSIGNAL|MSG_DONTWAIT) == bytes) {
                // Success!
                continue;
            }

            // Our write failed.
            // This could be due to EWOULDBLOCK, meaning the send buffer is full. We're not willing to block here.
            // This could be due to any other error... but it doesn't matter. We're not willing to attempt recovery.
            // The way to proceed is to close this connection and move on.

            // Attempt to clear this slot
            // Note that we could be racing ourselves -- broadcast() calls can overlap -- so attempting to compare and
            // swap fd to -1 once (strongly) is correct. Exactly one thread will succeed.
            int none = -1;
            int swapped = __atomic_compare_exchange(&client_fds[i], &fd, &none, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
            if (swapped) {
                // We were the thread that cleared the slot
                // Close the fd, ignoring its success or failure.
                close(fd);
            }
        }
    }
}
