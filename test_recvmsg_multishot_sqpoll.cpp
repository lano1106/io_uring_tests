#include "liburing.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/socket.h>
#include <arpa/inet.h>

int t_create_socket_pair(int fd[2], bool stream)
{
	int ret;
	int type = stream ? SOCK_STREAM : SOCK_DGRAM;
	int val;
	struct sockaddr_in serv_addr;
	struct sockaddr *paddr;
	size_t paddrlen;

	type |= SOCK_CLOEXEC;
	fd[0] = socket(AF_INET, type, 0);
	if (fd[0] < 0)
		return errno;
	fd[1] = socket(AF_INET, type, 0);
	if (fd[1] < 0) {
		ret = errno;
		close(fd[0]);
		return ret;
	}

	val = 1;
	if (setsockopt(fd[0], SOL_SOCKET, SO_REUSEADDR, &val, sizeof(val)))
		goto errno_cleanup;

	memset(&serv_addr, 0, sizeof(serv_addr));
	serv_addr.sin_family = AF_INET;
	serv_addr.sin_port = 0;
	inet_pton(AF_INET, "127.0.0.1", &serv_addr.sin_addr);

	paddr = (struct sockaddr *)&serv_addr;
	paddrlen = sizeof(serv_addr);

	if (bind(fd[0], paddr, paddrlen)) {
		fprintf(stderr, "bind failed\n");
		goto errno_cleanup;
	}

	if (stream && listen(fd[0], 16)) {
		fprintf(stderr, "listen failed\n");
		goto errno_cleanup;
	}

	if (getsockname(fd[0], (struct sockaddr *)&serv_addr,
			(socklen_t *)&paddrlen)) {
		fprintf(stderr, "getsockname failed\n");
		goto errno_cleanup;
	}
	inet_pton(AF_INET, "127.0.0.1", &serv_addr.sin_addr);

	if (connect(fd[1], (struct sockaddr *)&serv_addr, paddrlen)) {
		fprintf(stderr, "connect failed\n");
		goto errno_cleanup;
	}

	if (!stream) {
		/* connect the other udp side */
		if (getsockname(fd[1], (struct sockaddr *)&serv_addr,
				(socklen_t *)&paddrlen)) {
			fprintf(stderr, "getsockname failed\n");
			goto errno_cleanup;
		}
		inet_pton(AF_INET, "127.0.0.1", &serv_addr.sin_addr);

		if (connect(fd[0], (struct sockaddr *)&serv_addr, paddrlen)) {
			fprintf(stderr, "connect failed\n");
			goto errno_cleanup;
		}
		return 0;
	}

	/* for stream case we must accept and cleanup the listen socket */

	ret = accept(fd[0], NULL, NULL);
	if (ret < 0)
		goto errno_cleanup;

	close(fd[0]);
	fd[0] = ret;

	return 0;

errno_cleanup:
	ret = errno;
	close(fd[0]);
	close(fd[1]);
	return ret;
}

int main(int argc, char *argv[])
{
    struct io_uring ring;
    struct io_uring_params params = { };
	int n_sqe = 32;
    int ret;
    int fds[2];
    size_t rxBufSize = 2048;
    char *buffers;
    struct io_uring_buf_ring *br;
    int n_bufnum = 2;

    params.flags |= IORING_SETUP_SQPOLL;
    params.sq_thread_idle = 50;
    ret = io_uring_queue_init_params(n_sqe, &ring, &params);
	if (ret) {
		fprintf(stderr, "queue init failed: %d\n", ret);
		return ret;
	}
	ret = t_create_socket_pair(fds, true);
	if (ret) {
		fprintf(stderr, "t_create_socket_pair failed: %d\n", ret);
		return ret;
	}
	ret = posix_memalign((void **)&buffers, 4096, n_bufnum*rxBufSize);
    if (ret) {
        fprintf(stderr, "posix_memalign: (%d) %s\n", ret, strerror(ret));
        return ret;
    }
    ret = posix_memalign((void **)&br, 4096, n_bufnum*sizeof(struct io_uring_buf));
    if (ret) {
        fprintf(stderr, "posix_memalign: (%d) %s\n", ret, strerror(ret));
        return ret;
    }
    int brmask = io_uring_buf_ring_mask(n_bufnum);
    char *ptr = buffers;

    io_uring_buf_ring_init(br);
    for (int i = 0; i < n_bufnum; ++i) {
        io_uring_buf_ring_add(br, ptr, rxBufSize, i, brmask, i);
        ptr += rxBufSize;
    }
    io_uring_buf_ring_advance(br, n_bufnum);

    struct io_uring_buf_reg reg = { };
    reg.ring_addr = (unsigned long)br;
    reg.ring_entries = n_bufnum;
    reg.bgid = 0;
    ret = io_uring_register_buf_ring(&ring, &reg, 0);
    if (ret < 0) {
        fprintf(stderr, "ev_iouring_provide_buffers: (%d) %s\n", -ret, strerror(-ret));
        return ret;
    }
    struct io_uring_sqe *sqe = io_uring_get_sqe(&ring);
    struct msghdr msgh = {};
    io_uring_prep_recvmsg_multishot(sqe, fds[1], &msgh, 0);
    io_uring_sqe_set_flags(sqe, IOSQE_BUFFER_SELECT);
    sqe->buf_group = 0;

    // submit the recvmsh multishot
    ret = io_uring_submit(&ring);
    if (ret != 1) {
        fprintf(stderr, "unexpected io_uring_submit() return value: %d\n", ret);
        return ret;
    }
    // use first buffer
    write(fds[0], "a", 1);

    // Wait for first cqe to be generated to be sure that the 2 writes are not concatenated.
    struct io_uring_cqe *cqe;
    ret = io_uring_wait_cqe_nr(&ring, &cqe, 1);
    if (ret) {
        fprintf(stderr, "io_uring_wait_cqe_nr: (%d) %s\n", -ret, strerror(-ret));
        return ret;
    }
    // do not return the buffer. The whole point of the test is to see the behavior at buffer exhaustion
    io_uring_cq_advance(&ring, 1);

    /*
     * make a second write.
     * The expected result is that the cq ring will have 2 cqes.
     * 1 for the second recvmsg, 1 to report ENOBUFS
     */
    write(fds[0], "a", 1);
    // Give plenty of time to the sqpoll thread to process
    sleep(1);

    if (IO_URING_READ_ONCE(*ring.sq.kflags) &
			   IORING_SQ_NEED_WAKEUP)
        fprintf(stderr, "sqpoll thread idle\n");
    fprintf(stderr, "io_uring_cq_ready: %u\n", io_uring_cq_ready(&ring));

    close(fds[0]);
	close(fds[1]);
	io_uring_queue_exit(&ring);
    free(buffers);
    return 0;
}
