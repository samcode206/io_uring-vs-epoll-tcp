# IO_uring vs.Epoll: A Comparative Study of TCP Server IO Performance


This repository houses a comparison between two TCP Echo Server implementations, adhering to [RFC862](https://www.rfc-editor.org/rfc/rfc862). The primary objective is to assess the performance of Linux's io_uring under intense network I/O workloads in contrast to the well-established epoll mechanism.


## Prerequisites
- [liburing](https://github.com/axboe/liburing)
- Linux Kernel version 6.1 or above (earlier versions may work but are untested)

## benchmarks

Benchmark tests were conducted in two different scenarios to simulate common Network I/O workloads:

- Streaming Client: The client continuously writes data without waiting for an echo.
- Request-Response Client: The client writes data, waits for a response, and then initiates the next write.

### Observations 

io_uring outperforms epoll in request-response workloads but faces challenges in keeping up with epoll when clients are streaming data.

### Streaming client

![256 byte payloads](https://github.com/samcode206/io_uring-tcp-echo-server/blob/master/bench/stream/256/256.png?raw=true)

![512 byte payloads](https://github.com/samcode206/io_uring-tcp-echo-server/blob/master/bench/stream/512/512.png?raw=true)

![1kb payloads](https://github.com/samcode206/io_uring-tcp-echo-server/blob/master/bench/stream/1024/1kb.png?raw=true)

![4kb payloads](https://github.com/samcode206/io_uring-tcp-echo-server/blob/master/bench/stream/4096/4kb.png?raw=true)


### Request-response

![256 byte req-res payloads](https://github.com/samcode206/io_uring-tcp-echo-server/blob/master/bench/req-res/256/256-req-res.png?raw=true)

![512 byte req-res payloads](https://github.com/samcode206/io_uring-tcp-echo-server/blob/master/bench/req-res/512/512-req-res.png?raw=true)



To Run either server locally

1. Ensure that liburing is installed.
2. Adjust the buffer and max event/max connection settings in the source code for each server as necessary.

These tests were executed on a `11th Gen Intel® Core™ i9-11900K @ 3.50GHz` debian 12 (running directly on hardware no vm), with the servers pinned to CPU 15 via `taskset -cp 15 {{pid}}`. The kernel parameters were set as `mitigations=off isolcpus=15`.

