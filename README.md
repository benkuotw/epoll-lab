# epoll-lab

A small edge-triggered `epoll` echo server and a threaded stress client, for seeing the
edge-triggered (ET) rules work, and fail when they're broken.

Written while working through the epoll chapter of Michael Kerrisk's *The Linux Programming
Interface* (TLPI). TLPI explains the API but has no complete ET socket server, and
[epoll(7)](https://man7.org/linux/man-pages/man7/epoll.7.html) lists the rules (non-blocking
fds, read until `EAGAIN`) but leaves the server to the reader.

| Program | What it does |
|---|---|
| `epoll_server` | Single-threaded echo server. Client sockets are always ET; flags choose how the listening socket is handled |
| `client` | Stress client: one thread per connection; each thread connects, sends one message, reads the echo and closes |

## Requirements

- Linux and glibc 2.30 or later (`accept4()`, `gettid()`)
- gcc and make

Tested on Debian 13 (kernel 6.12, arm64), 4 vCPUs, over loopback.

## Build

```bash
make
```

The `Makefile` defines `_GNU_SOURCE`, which `accept4()` and `gettid()` need. If clangd reports
them as undeclared, generate a compilation database with `make clean && bear -- make`.

## ET rules covered

I add each rule to the server once I've seen it work and seen it fail:

- [x] non-blocking listener and client sockets
- [x] accept until `EAGAIN` (`-a`), then wait for `EPOLLIN`
- [x] read until `EAGAIN`, then wait for `EPOLLIN`
- [x] write until `EAGAIN`, then wait for `EPOLLOUT`

## Usage

### epoll_server: echo server

```console
$ ./epoll_server [-p PORT] [-e] [-a]
```

| Flag | Meaning | Default |
|---|---|---|
| `-p PORT` | listen port | 10000 |
| `-e` | edge-triggered listener | level-triggered, as in the epoll(7) example |
| `-a` | accept until `EAGAIN` on each wakeup | one `accept4()` per wakeup |

```
$ ./epoll_server -a
./epoll_server[882108]:listening on port 10000, LT listener, accept until EAGAIN
[882108]Received from 127.0.0.1:58968: thrd[882111]
[882108]Received from 127.0.0.1:58970: thrd[882113]
[882108]Received from 127.0.0.1:58966: thrd[882112]
```

The server prints every message it receives. Send stdout to `/dev/null` when testing load, or
the terminal becomes the bottleneck.

### client: stress client

```console
$ ./client [-p PORT] [-t THREADS]
```

`-p` is the server port (default 10000) and `-t` the number of threads, one connection each
(default 1000).

```
$ ./client -t 1000
threads 1000
failed_create: 0
failed: 0
echoed:1000
```

### Comparing the listener modes

The two server flags give four listener modes. Results with `./client -t 1000` and
`strace -c` on the server, except where noted:

| Flags | Listener | Accept | `epoll_wait()` calls | Result |
|---|---|---|---|---|
| (none) | LT | one per wakeup | 1002 | all echoed |
| `-a` | LT | loop until `EAGAIN` | ~10 | all echoed |
| `-e -a` | ET | loop until `EAGAIN` | ~10 | all echoed |
| `-e` | ET | one per wakeup | – | **stuck** (with `-t 2000`) |

On arm64, glibc's `epoll_wait()` calls the `epoll_pwait` system call, so `strace` counts it as
`epoll_pwait`.

`-e` without `-a` is broken on purpose. With 2000 threads, the client never finishes, and the
listening socket shows the connections that were never accepted:

```
$ ss -ltn 'sport = :10000'
State  Recv-Q Send-Q Local Address:Port  Peer Address:Port
LISTEN 1420   4096         0.0.0.0:10000      0.0.0.0:*
```

1420 connections are waiting in the accept queue, and the client still has 1421 threads
(1420 blocked in `read()` plus the main thread).

## What I learned

- With an ET listener, the accept loop is required. While the server is busy, several new
  connections merge into one event, so one `accept4()` leaves the rest queued.
- With an LT listener, the accept loop is optional, but it reduces `epoll_wait()` calls:
  1002 without it and about 10 with it, for 1000 connections.
- 10,000 concurrent connections are easy for one thread. Raise the open-file limit first,
  e.g. `ulimit -n 10240`; the default is often 1024.

## References

- Michael Kerrisk, *The Linux Programming Interface*, chapter 63 (alternative I/O models)
- Man pages: [epoll(7)](https://man7.org/linux/man-pages/man7/epoll.7.html)

## License

MIT. See [LICENSE](LICENSE).
