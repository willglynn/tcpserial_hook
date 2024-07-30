# `tcpserial_hook`

This is a library which can be inserted into a closed-source application for the purpose of monitoring its serial port
activity over TCP/IP. It causes the application to listen for incoming connections (TCP port 7160), to which it
transmits a copy of every byte exchanged over the serial port.

<details><summary>Theory of operation</summary>
<p><code>tcpserial_hook.so</code> intercepts the <code>read()</code>, <code>write()</code>, and <code>tcsetattr()</code>
system calls. When inserted into a target process using <code>LD_PRELOAD</code>, the target process will call
<code>tcpserial_hook.so</code> instead of the operating system. These three functions all pass through to the operating
system, but with side effects.</p>
<p>The preloaded library contains a static initializer which listens on port 7160 and spawns a thread to accept
incoming connections prior to the application's <code>main()</code> function. When the application configures a serial
port using <code>tcsetattr()</code>, the library remembers its file descriptor, at which point any data exchanged by
future <code>read()</code> or <code>write()</code> calls on that file descriptor are broadcast to all connected
clients.</p>
<p>If a client's TCP send buffer is filled, the library must choose between a) allocating an unbounded amount of memory,
b) blocking the application, c) discarding data for that client, or d) terminating that connection. The library chooses
to terminate the connection. This alerts the consumer that it fell behind, and the consumer is of course free to
reconnect and try again.</p>
<p>This library supports a fixed maximum number of simultaneous clients (8). Additional connections are terminated
immediately. This is intended to provide another bound on resource consumption and prevent an accidental
denial-of-service condition.</p>
</details>

It is intended for the [Tigo Cloud Connect Advanced (CCA)](https://www.tigoenergy.com/product/cloud-connect-advanced) or
similar, targeting the `meshdcd` process which communicates with a [Tigo Access Point
(TAP)](https://www.tigoenergy.com/product/tigo-access-point) using a serial port.

## Installation

Installation requires `root` access. Obtaining `root` access on a Tigo CCA is outside the scope of this document.

Start by downloading the shared library and ensure that the `wrap_mesh.sh` script does not exist:

```console
~ # curl -o /mnt/ffs/lib/tcpserial_hook.so https://…/tcpserial_hook.so
~ # ls -l /mnt/ffs/mgmtu/bin/wrap_mesh.sh
~ # 
```

Assuming `ls` did not show an existing `wrap_mesh.sh` script, create one and make it executable:

```console
~ # echo -e '#!/bin/sh\nLD_PRELOAD=/mnt/ffs/lib/tcpserial_hook.so exec /mnt/ffs/mgmtu/bin/meshdcd' > /mnt/ffs/mgmtu/bin/wrap_mesh.sh
~ # chmod +x  /mnt/ffs/mgmtu/bin/wrap_mesh.sh
```

Finally, restart `meshdcd`:

```console
~ # /mnt/ffs/etc/rc.d/S040_MeshDCD 
Shutting down meshdcd from meshdcd launcher
Killing program PID 3088
Shutting down wrap_mesh.sh from meshdcd launcher
Killing watcher PID 2815
Starting Tigo Mesh Data Collection service.
~ #
```

You can now connect to the CCA on TCP port 7160 to monitor the serial data.

## Uninstallation

Remove the files and restart `meshdcd`.

```console
~ # rm /mnt/ffs/mgmtu/bin/wrap_mesh.sh /mnt/ffs/lib/tcpserial_hook.so
~ # /mnt/ffs/etc/rc.d/S040_MeshDCD 
Shutting down meshdcd from meshdcd launcher
Killing program PID 8158
Shutting down wrap_mesh.sh from meshdcd launcher
Killing watcher PID 7120
Starting Tigo Mesh Data Collection service.
~ #
```
