#include <asm-generic/errno.h>
#include <assert.h>
#include <ctype.h>
#include <fcntl.h>
#include <liburing.h>
#include <liburing/io_uring.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define MAX_FDS 1024
#define SQ_ENTRIES 1024

#define BUFFER_SIZE 1024 * 4
#define BUF_RINGS 4
#define BG_ENTRIES 1024 * 4
#define CONN_BACKLOG 256

#define FD_UNUSED -1
#define FD_CLOSING -2
#define FD_OPEN 1

#define FD_MASK ((1ULL << 21) - 1) // 21 bits
#define FD_SHIFT 0
#define SRV_LIM_MAX_FD 2097151

#define BGID_MASK (((1ULL << 17) - 1) << 21) // 17 bits shifted by 21
#define BGID_SHIFT 21
#define SRV_LIM_MAX_BGS 131071

#define EVENT_MASK (3ULL << 38) // 2 bits shifted by 38
#define EVENT_SHIFT 38

#define EV_ACCEPT 0
#define EV_RECV 1
#define EV_SEND 2
#define EV_CLOSE 3

static inline void set_fd(uint64_t *data, uint32_t fd) {
  *data = (*data & ~FD_MASK) | ((uint64_t)fd << FD_SHIFT);
}

static inline void set_bgid(uint64_t *data, uint32_t index) {
  *data = (*data & ~BGID_MASK) | ((uint64_t)index << BGID_SHIFT);
}

static inline void set_event(uint64_t *data, uint8_t event) {
  *data = (*data & ~EVENT_MASK) | ((uint64_t)event << EVENT_SHIFT);
}

static inline uint32_t get_fd(uint64_t data) {
  return (data & FD_MASK) >> FD_SHIFT;
}
static inline uint32_t get_bgid(uint64_t data) {
  return (data & BGID_MASK) >> BGID_SHIFT;
}
static inline uint8_t get_event(uint64_t data) {
  return (data & EVENT_MASK) >> EVENT_SHIFT;
}

typedef struct {
  struct io_uring ring;
  struct io_uring_buf_ring *buf_rings[BUF_RINGS];
  int fds[MAX_FDS];
  int active_bgid;
} server_t;

server_t *server_init(void);
struct io_uring_buf_ring *server_register_bg(server_t *s, unsigned short bgid,
                                             unsigned int entries, char *buf,
                                             unsigned int buf_sz);
void server_register_buf_rings(server_t *s);
int server_socket_bind_listen(server_t *s, int port);
void server_ev_loop_start(server_t *s, int fd);

server_t *server_init(void) {
  server_t *s = (server_t *)calloc(1, sizeof(server_t));
  assert(s != NULL);

  struct io_uring_params params;
  assert(memset(&params, 0, sizeof(params)) != NULL);

  assert(memset(s->fds, FD_UNUSED, sizeof(int) * MAX_FDS) != NULL);

  // params.cq_entries = CQ_ENTRIES; also add IORING_SETUP_CQSIZE to flags
  params.flags = IORING_SETUP_COOP_TASKRUN | IORING_SETUP_SINGLE_ISSUER;

  assert(io_uring_queue_init_params(SQ_ENTRIES, &s->ring, &params) == 0);
  assert(io_uring_register_files_sparse(&s->ring, MAX_FDS) == 0);
  assert(io_uring_register_ring_fd(&s->ring) == 1);

  server_register_buf_rings(s);

  return s;
}
void server_register_buf_rings(server_t *s) {
  unsigned int i;
  for (i = 0; i < BUF_RINGS; ++i) {
    char *group_buf = (char *)calloc(BG_ENTRIES * BUFFER_SIZE, sizeof(char));
    assert(group_buf != NULL);
    s->buf_rings[i] =
        server_register_bg(s, i, BG_ENTRIES, group_buf, BUFFER_SIZE);
    assert(s->buf_rings[i] != NULL);
  }
}

struct io_uring_buf_ring *server_register_bg(server_t *s, unsigned short bgid,
                                             unsigned int entries, char *buf,
                                             unsigned int buf_sz) {
  struct io_uring_buf_ring *br;
  struct io_uring_buf_reg reg = {0};

  posix_memalign((void **)&br, getpagesize(),
                 entries * sizeof(struct io_uring_buf_ring));

  reg.ring_addr = (unsigned long)br;
  reg.ring_entries = entries;
  reg.bgid = bgid;

  int ret = io_uring_register_buf_ring(&s->ring, &reg, 0);
  if (ret != 0){
    printf("io_uring_register_buf_ring failed: %d\n", ret);
    exit(1);
  }

  io_uring_buf_ring_init(br);

  unsigned int i;
  unsigned int offset = 0;
  for (i = 0; i < entries; ++i) {
    io_uring_buf_ring_add(br, buf + offset, buf_sz, i,
                          io_uring_buf_ring_mask(entries), i);
    offset += buf_sz;
  }

  io_uring_buf_ring_advance(br, entries);

  return br;
}

int server_socket_bind_listen(server_t *s, int port) {
  int fd;
  struct sockaddr_in srv_addr;

  fd = socket(PF_INET, SOCK_STREAM, 0);

  int on = 1;
  setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(int));
  memset(&srv_addr, 0, sizeof(srv_addr));
  srv_addr.sin_family = AF_INET;
  srv_addr.sin_port = htons(port);
  srv_addr.sin_addr.s_addr = htons(INADDR_ANY); /* 0.0.0.0 */

  assert(bind(fd, (const struct sockaddr *)&srv_addr, sizeof(srv_addr)) >= 0);
  assert(listen(fd, CONN_BACKLOG) >= 0);
  return fd;
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

inline static char *server_get_selected_buffer(server_t *s, uint32_t bgid,
                                               uint32_t buf_idx) {
  char *buf = (char *)s->buf_rings[bgid]->bufs->addr;
  return buf + (buf_idx * (sizeof(char) * BUFFER_SIZE));
}

void server_ev_loop_start(server_t *s, int listener_fd) {
  struct io_uring_sqe *accept_ms_sqe = io_uring_get_sqe(&s->ring);
  struct sockaddr_in client_addr;

  socklen_t client_addr_len = sizeof(client_addr);
  assert(accept_ms_sqe != NULL);
  io_uring_prep_multishot_accept_direct(accept_ms_sqe, listener_fd,
                                        (struct sockaddr *)&client_addr,
                                        &client_addr_len, 0);

  uint64_t accept_ctx = 0;
  set_event(&accept_ctx, EV_ACCEPT);
  io_uring_sqe_set_data64(accept_ms_sqe, accept_ctx);

  for (;;) {
    // printf("start loop iteration\n");
    int ret = io_uring_submit_and_wait(&s->ring, 1);
    assert(ret >= 0); // todo remove and add real error handling

    // printf("io_uring_submit_and_wait: %d\n", ret);
    struct io_uring_cqe *cqe;
    unsigned head;
    unsigned i = 0;

    io_uring_for_each_cqe(&s->ring, head, cqe) {
      ++i;
      uint64_t ctx = io_uring_cqe_get_data64(cqe);
      uint8_t ev = get_event(ctx);

      if (ev == EV_ACCEPT) {
        // ACCEPT
        if (cqe->res < 0) {
          printf("accept error: %d exiting...\n", cqe->res);
          exit(1);
        }
        printf("ACCEPT %d\n", cqe->res);
        struct io_uring_sqe *recv_ms_sqe = must_get_sqe(s);
        s->fds[cqe->res] = FD_OPEN; // update fd status
        recv_ms_sqe->fd = cqe->res;
        recv_ms_sqe->buf_group = 0; // TODO dynamically pick bg
        io_uring_prep_recv_multishot(recv_ms_sqe, cqe->res, NULL, 0, 0);

        io_uring_sqe_set_flags(recv_ms_sqe,
                               IOSQE_FIXED_FILE | IOSQE_BUFFER_SELECT);

        uint64_t recv_ctx = 0;
        set_event(&recv_ctx, EV_RECV);
        set_fd(&recv_ctx, cqe->res);
        set_bgid(&recv_ctx, 0); // TODO dynamically pick bg
        io_uring_sqe_set_data64(recv_ms_sqe, recv_ctx);
      } else if (ev == EV_RECV) {
        printf("RECV %d\n", cqe->res);
        if (cqe->res <= 0) {
          if (cqe->res == 0) {
            uint32_t fd_to_close = get_fd(ctx);
            if (s->fds[fd_to_close] != FD_CLOSING) {
              struct io_uring_sqe *close_sqe = must_get_sqe(s);
              close_sqe->fd = fd_to_close;
              s->fds[fd_to_close] = FD_CLOSING;
              io_uring_sqe_set_flags(close_sqe, IOSQE_FIXED_FILE);
              io_uring_prep_close_direct(close_sqe, fd_to_close);
              uint64_t close_ctx = 0;
              set_event(&close_ctx, EV_CLOSE);
              set_fd(&close_ctx, fd_to_close);
              io_uring_sqe_set_data64(close_sqe, close_ctx);
            }
          }
        }
        // we have data to be read
        else {
          unsigned int buf_id = cqe->flags >> IORING_CQE_BUFFER_SHIFT;
          uint32_t bgid = get_bgid(ctx);

          char *recv_buf = server_get_selected_buffer(s, bgid, buf_id);
          printf("%s\n", recv_buf);
        }

      } else if (ev == EV_SEND) {
        printf("SEND %d\n", cqe->res);
      } else if (ev == EV_CLOSE) {
        printf("CLOSE %d\n", cqe->res);
        if (cqe->res == 0) {
          uint32_t closed_fd = get_fd(ctx);
          printf("file closed %d\n", closed_fd);
          s->fds[closed_fd] = FD_UNUSED;
        }

      } else {
        printf("here\n");
      }
    };
    // printf("end loop iteration cqes seen %d\n", i);
    io_uring_cq_advance(&s->ring, i);
  }
}

int main(void) {
  server_t *s = server_init();
  assert(s != NULL);
  int fd = server_socket_bind_listen(s, 9919);
  printf("server starting on port: %d\n", 9919);
  server_ev_loop_start(s, fd);

  close(fd);

  return 0;
}
