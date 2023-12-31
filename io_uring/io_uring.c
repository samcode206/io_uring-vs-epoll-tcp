/*
MIT License

Copyright (c) 2023 Sam, H

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.

*/
#include <assert.h>
#include <liburing.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#define FD_COUNT 1024
#define LISTEN_BACKLOG 1024

#define SQ_DEPTH FD_COUNT
#define BG_ENTRIES FD_COUNT
#define BUF_BASE_OFFSET (sizeof(struct io_uring_buf) * BG_ENTRIES)

#define BUFF_CAP 1024 * 8
#define EV_ACCEPT 0
#define EV_RECV 1
#define EV_SEND 2
#define EV_CLOSE 3

#define LIKELY(x) __builtin_expect(!!(x), 1)
#define UNLIKELY(x) __builtin_expect(!!(x), 0)

static_assert(!(BG_ENTRIES & (BG_ENTRIES - 1)),
              "BG_ENTRIES must be a power of two");

static_assert(!(SQ_DEPTH & (SQ_DEPTH - 1)), "SQ_DEPTH must be a power of two");

typedef struct server_t server_t;
typedef void (*io_event_cb)(server_t *s, uint64_t ctx,
                            struct io_uring_cqe *cqe);

struct server_t {
  struct io_uring ring;               // the ring
  struct io_uring_buf_ring *buf_ring; // ring mapped buffer
  io_event_cb ev_handlers[4];         // completion queue entry handlers
};

void server_register_buf_ring(server_t *s);

int server_socket_bind_listen(int port, int sockopts);

static void server_add_multishot_accept(server_t *s, int listener_fd);

static void server_add_recv(server_t *s, int fd);

static inline void server_add_send(server_t *s, uint64_t *ctx,
                                   const void *data, size_t len,
                                   uint32_t sqe_flags, uint32_t send_flags);

static void server_add_close_direct(server_t *s, uint32_t fd);

static void on_accept(server_t *s, uint64_t ctx, struct io_uring_cqe *cqe);

static void on_read(server_t *s, uint64_t ctx, struct io_uring_cqe *cqe);

static void on_write(server_t *s, uint64_t ctx, struct io_uring_cqe *cqe);

static void on_close(server_t *s, uint64_t ctx, struct io_uring_cqe *cqe);

static inline unsigned char *server_get_selected_buffer(server_t *s,
                                                        uint32_t buf_idx);

static inline int server_conn_get_bgid(server_t *s);

static inline void server_recycle_buff(server_t *s, void *buf,
                                       uint32_t buf_idx);

struct io_uring_sqe *must_get_sqe(server_t *s);


static inline void conn_set_fd(uint64_t *data, uint32_t fd);
static inline void conn_set_bgid(uint64_t *data, uint32_t index);
static inline void conn_set_buf_idx(uint64_t *data, uint32_t index);
static inline void conn_set_event(uint64_t *data, uint8_t event);
static inline uint32_t conn_get_fd(uint64_t data);
static inline uint32_t conn_get_bgid(uint64_t data);
static inline uint32_t conn_get_buf_idx(uint64_t data);
static inline uint8_t conn_get_event(uint64_t data);

// ---------------------------------------------------------------------

int main(void) {
  int fd = server_socket_bind_listen(9919, SO_REUSEADDR);
  printf("io_uring backed TCP echo server starting on port: %d\n", 9919);

  server_t s;
  memset(&s, 0, sizeof s);

  s.ev_handlers[EV_ACCEPT] = on_accept;
  s.ev_handlers[EV_RECV] = on_read;
  s.ev_handlers[EV_SEND] = on_write;
  s.ev_handlers[EV_CLOSE] = on_close;

  struct io_uring_params params;
  assert(memset(&params, 0, sizeof(params)) != NULL);

  params.flags = IORING_SETUP_COOP_TASKRUN | IORING_SETUP_DEFER_TASKRUN |
                 IORING_SETUP_SINGLE_ISSUER;

  assert(io_uring_queue_init_params(SQ_DEPTH, &s.ring, &params) == 0);
  assert(io_uring_register_files_sparse(&s.ring, FD_COUNT) == 0);
  assert(io_uring_register_ring_fd(&s.ring) == 1);

  server_register_buf_ring(&s);
  server_add_multishot_accept(&s, fd);

  for (;;) {
    // printf("start loop iteration\n");
    int ret = io_uring_submit_and_wait(&s.ring, 1);
    assert(ret >= 0); // todo(sah): handle more gracefully

    // printf("io_uring_submit_and_wait: %d\n", ret);
    struct io_uring_cqe *cqe;
    unsigned head;
    unsigned i = 0;

    io_uring_for_each_cqe(&s.ring, head, cqe) {
      ++i;
      uint64_t ctx = io_uring_cqe_get_data64(cqe);
      uint8_t ev = conn_get_event(ctx);

      s.ev_handlers[ev](&s, ctx, cqe);
    };

    // printf("end loop iteration cqes seen %d\n", i);
    io_uring_cq_advance(&s.ring, i);
  }

  printf("exiting event loop\n");
  io_uring_queue_exit(&s.ring);

  close(fd);

  return 0;
}

// ---------------------------------------------------------------------

void server_register_buf_ring(server_t *s) {
  struct io_uring_buf_reg reg = {
      .ring_addr = 0, .ring_entries = BG_ENTRIES, .bgid = 0};

  void *mbr = mmap(NULL, (sizeof(struct io_uring_buf) + BUFF_CAP) * BG_ENTRIES,
                   PROT_READ | PROT_WRITE,
                   MAP_ANON | MAP_PRIVATE | MAP_POPULATE, -1, 0);
  assert(mbr != MAP_FAILED);

  s->buf_ring = (struct io_uring_buf_ring *)mbr;

  io_uring_buf_ring_init(s->buf_ring);

  reg.ring_addr = (unsigned long)s->buf_ring;

  assert(io_uring_register_buf_ring(&s->ring, &reg, 0) == 0);

  unsigned char *buf_addr;
  for (size_t i = 0; i < BG_ENTRIES; ++i) {
    buf_addr = (unsigned char *)s->buf_ring + BUF_BASE_OFFSET + (i * BUFF_CAP);
    io_uring_buf_ring_add(s->buf_ring, buf_addr, BUFF_CAP, i,
                          io_uring_buf_ring_mask(BG_ENTRIES), i);

    assert(server_get_selected_buffer(s, i) == buf_addr);
  }

  io_uring_buf_ring_advance(s->buf_ring, BG_ENTRIES);
}

int server_socket_bind_listen(int port, int sockopts) {
  int fd;
  struct sockaddr_in srv_addr;

  fd = socket(PF_INET, SOCK_STREAM, 0);

  int on = 1;
  setsockopt(fd, SOL_SOCKET, sockopts, &on, sizeof(int));
  memset(&srv_addr, 0, sizeof(srv_addr));
  srv_addr.sin_family = AF_INET;
  srv_addr.sin_port = htons(port);
  srv_addr.sin_addr.s_addr = htons(INADDR_ANY);

  assert(bind(fd, (const struct sockaddr *)&srv_addr, sizeof(srv_addr)) >= 0);
  assert(listen(fd, LISTEN_BACKLOG) >= 0);
  return fd;
}

static inline unsigned char *server_get_selected_buffer(server_t *s,
                                                        uint32_t buf_idx) {
  return (unsigned char *)s->buf_ring->bufs[buf_idx].addr;
}

static inline int server_conn_get_bgid(server_t *s) { return 0; }

static inline void server_recycle_buff(server_t *s, void *buf,
                                       uint32_t buf_idx) {

  io_uring_buf_ring_add(s->buf_ring, buf, BUFF_CAP, buf_idx,
                        io_uring_buf_ring_mask(BG_ENTRIES), 0);

  io_uring_buf_ring_advance(s->buf_ring, 1);
}

struct io_uring_sqe *must_get_sqe(server_t *s) {
  struct io_uring_sqe *sqe = io_uring_get_sqe(&s->ring);
  if (!sqe) {
    io_uring_submit(&s->ring);
    sqe = io_uring_get_sqe(&s->ring);
    if (!sqe) {
      printf("failed to get an sqe shutting it down...\n");
      exit(1);
      return NULL;
    }
  }

  return sqe;
}

static void server_add_multishot_accept(server_t *s, int listener_fd) {
  struct io_uring_sqe *accept_ms_sqe = must_get_sqe(s);
  struct sockaddr_in client_addr;

  socklen_t client_addr_len = sizeof(client_addr);
  assert(accept_ms_sqe != NULL);
  io_uring_prep_multishot_accept_direct(accept_ms_sqe, listener_fd,
                                        (struct sockaddr *)&client_addr,
                                        &client_addr_len, 0);

  uint64_t accept_ctx = 0;
  conn_set_event(&accept_ctx, EV_ACCEPT);
  io_uring_sqe_set_data64(accept_ms_sqe, accept_ctx);
}

static void server_add_recv(server_t *s, int fd) {
  struct io_uring_sqe *sqe = must_get_sqe(s);
  io_uring_prep_recv(sqe, fd, NULL, 0, 0);
  io_uring_sqe_set_flags(sqe, IOSQE_FIXED_FILE | IOSQE_BUFFER_SELECT);
  uint64_t recv_ctx = 0;
  conn_set_event(&recv_ctx, EV_RECV);
  conn_set_fd(&recv_ctx, fd);
  conn_set_bgid(&recv_ctx, server_conn_get_bgid(s));
  io_uring_sqe_set_data64(sqe, recv_ctx);
  sqe->buf_group = server_conn_get_bgid(s);
}

static inline void server_add_send(server_t *s, uint64_t *ctx,
                                   const void *data, size_t len,
                                   uint32_t sqe_flags, uint32_t send_flags) {
  int fd = conn_get_fd(*ctx);
  struct io_uring_sqe *sqe = must_get_sqe(s);
  io_uring_prep_send(sqe, fd, data, len, send_flags);
  io_uring_sqe_set_flags(sqe, sqe_flags);

  conn_set_event(ctx, EV_SEND);
  io_uring_sqe_set_data64(sqe, *ctx);
}

static void server_add_close_direct(server_t *s, uint32_t fd) {
  struct io_uring_sqe *sqe = must_get_sqe(s);
  sqe->fd = fd;
  io_uring_sqe_set_flags(sqe, IOSQE_FIXED_FILE);
  io_uring_prep_close_direct(sqe, fd);

  uint64_t close_ctx = 0;
  conn_set_event(&close_ctx, EV_CLOSE);
  conn_set_fd(&close_ctx, fd);

  io_uring_sqe_set_data64(sqe, close_ctx);
}

static void on_accept(server_t *s, uint64_t ctx,
                      struct io_uring_cqe *cqe) {
  if (UNLIKELY(cqe->res < 0)) {
    printf("accept error: %d exiting...\n", cqe->res);
    exit(1);
  }
  server_add_recv(s, cqe->res);
}

static void on_read(server_t *s, uint64_t ctx, struct io_uring_cqe *cqe) {
  if (UNLIKELY(cqe->res <= 0)) {
    if (cqe->res == -ENOBUFS) {
      fprintf(stderr, "ran out of buffers exiting program...\n");
      exit(-ENOBUFS);
    } else {
      server_add_close_direct(s, conn_get_fd(ctx));
    }
  } else {
    unsigned int buf_id = cqe->flags >> IORING_CQE_BUFFER_SHIFT;
    // printf("buffer-group: %d\tbuffer-id: %d\n", bgid, buf_id);
    unsigned char *buf = server_get_selected_buffer(s, buf_id);
    // printf("%s\n", recv_buf);
    conn_set_buf_idx(&ctx, buf_id);
    server_add_send(s, &ctx, buf, cqe->res, IOSQE_FIXED_FILE, 0);
  }
}

static void on_write(server_t *s, uint64_t ctx, struct io_uring_cqe *cqe) {
  uint32_t buf_idx = conn_get_buf_idx(ctx);
  //   printf("buffer-group: %d\tbuffer-id: %d\n", bgid, buf_idx);
  unsigned char *buf = server_get_selected_buffer(s, buf_idx);
  if (UNLIKELY(cqe->res <= 0)) {
    fprintf(stderr, "send(): %s\n", strerror(-cqe->res));
    server_add_close_direct(s, conn_get_fd(ctx));
  } else {
    server_add_recv(s, conn_get_fd(ctx));
  }

  server_recycle_buff(s, buf, buf_idx);
}

static void on_close(server_t *s, uint64_t ctx, struct io_uring_cqe *cqe) {
  if (cqe->res < 0) {
    fprintf(stderr, "close: %s\n", strerror(-cqe->res));
  }
}


#define FD_MASK ((1ULL << 21) - 1)
#define BGID_SHIFT 21
#define BGID_MASK (((1ULL << 15) - 1) << BGID_SHIFT)

#define EVENT_SHIFT 36
#define EVENT_MASK (3ULL << EVENT_SHIFT)

#define BUFIDX_SHIFT 39
#define BUFIDX_MASK (((1ULL << 16) - 1) << BUFIDX_SHIFT)


static inline void conn_set_fd(uint64_t *data, uint32_t fd) {
  *data = (*data & ~FD_MASK) | (uint64_t)fd;
}

static inline void conn_set_bgid(uint64_t *data, uint32_t index) {
  *data = (*data & ~BGID_MASK) | ((uint64_t)index << BGID_SHIFT);
}

static inline void conn_set_buf_idx(uint64_t *data, uint32_t index) {
  *data = (*data & ~BUFIDX_MASK) | ((uint64_t)index << BUFIDX_SHIFT);
}

static inline void conn_set_event(uint64_t *data, uint8_t event) {
  *data = (*data & ~EVENT_MASK) | ((uint64_t)event << EVENT_SHIFT);
}

static inline uint32_t conn_get_fd(uint64_t data) { return (data & FD_MASK); }

static inline uint32_t conn_get_bgid(uint64_t data) {
  return (data & BGID_MASK) >> BGID_SHIFT;
}

static inline uint32_t conn_get_buf_idx(uint64_t data) {
  return (data & BUFIDX_MASK) >> BUFIDX_SHIFT;
}

static inline uint8_t conn_get_event(uint64_t data) {
  return (data & EVENT_MASK) >> EVENT_SHIFT;
}
