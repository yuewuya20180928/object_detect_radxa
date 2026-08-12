// v28_v4l2_bench.cpp — C++ V4L2 tight loop benchmark, pure grab+release
// 老板指令 (2026-08-08 00:16 GMT+8): 写一个 C++ 程序, 只做取流+释放帧, 看最大帧率
//
// 对比 Python v27 (cv2 + opencv V4L2 backend) vs 本程序 (raw V4L2 ioctl + mmap)
//   - v27 (cv2 + Python): 19.88 FPS, grab_ms avg=49.96
//   - v28 (raw V4L2 C++): 应该接近 sensor 物理上限, 可能比 v27 略快 (去掉 cv2 + python 开销)
//
// 实现:
//   - open /dev/video0 (O_RDWR | O_NONBLOCK)
//   - VIDIOC_QUERYCAP / S_FMT (640x480 YUYV) / S_PARM (1/30 fps)
//   - VIDIOC_REQBUFS 4 mmap buffers
//   - mmap each buffer, VIDIOC_QBUF each to queue
//   - VIDIOC_STREAMON
//   - Tight loop: DQBUF → QBUF (即取即释放, 不解析 frame 数据)
//   - 计每次 DQBUF+QBUF 的 dt
//
// 编译 (板端 aarch64):
//   g++ -O2 -o v28_v4l2_bench v28_v4l2_bench.cpp
//
// 用法:
//   ./v28_v4l2_bench                # /dev/video0, 640x480, 4 buffers, 15s
//   ./v28_v4l2_bench /dev/video0 320 240 2 20  # 320x240, 2 buffers, 20s

#include <linux/videodev2.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <vector>

static double timespec_to_ms(const struct timespec& t) {
    return t.tv_sec * 1000.0 + t.tv_nsec / 1e6;
}

static double timespec_diff_ms(const struct timespec& a, const struct timespec& b) {
    return (a.tv_sec - b.tv_sec) * 1000.0 + (a.tv_nsec - b.tv_nsec) / 1e6;
}

int main(int argc, char** argv) {
    const char* dev = argc > 1 ? argv[1] : "/dev/video0";
    int W = argc > 2 ? atoi(argv[2]) : 640;
    int H = argc > 3 ? atoi(argv[3]) : 480;
    int N_BUFS = argc > 4 ? atoi(argv[4]) : 4;
    int DURATION_S = argc > 5 ? atoi(argv[5]) : 15;

    printf("=== v28 V4L2 tight loop bench ===\n");
    printf("  dev=%s  res=%dx%d  n_bufs=%d  duration=%ds\n\n",
           dev, W, H, N_BUFS, DURATION_S);

    // 1. Open (BLOCKING mode: DQBUF 阻塞等 frame ready, 不需要 select)
    int fd = open(dev, O_RDWR);
    if (fd < 0) { perror("open"); return 1; }

    // 2. Query capability
    struct v4l2_capability cap;
    if (ioctl(fd, VIDIOC_QUERYCAP, &cap) < 0) { perror("QUERYCAP"); return 1; }
    printf("Driver: %s  Card: %s  Bus: %s\n", cap.driver, cap.card, cap.bus_info);

    // 3. Set format
    struct v4l2_format fmt;
    memset(&fmt, 0, sizeof(fmt));
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    fmt.fmt.pix.width = W;
    fmt.fmt.pix.height = H;
    fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_YUYV;
    fmt.fmt.pix.field = V4L2_FIELD_NONE;
    if (ioctl(fd, VIDIOC_S_FMT, &fmt) < 0) { perror("S_FMT"); return 1; }
    char pixfmt[5] = {0};
    memcpy(pixfmt, &fmt.fmt.pix.pixelformat, 4);
    printf("Set format OK: %dx%d pixfmt=%s bytesperline=%d sizeimage=%d\n",
           fmt.fmt.pix.width, fmt.fmt.pix.height, pixfmt,
           fmt.fmt.pix.bytesperline, fmt.fmt.pix.sizeimage);
    W = fmt.fmt.pix.width;  // actual after driver adjust
    H = fmt.fmt.pix.height;

    // 4. Try set parm 30 fps (driver 会 reject, 但我们记录实际值)
    struct v4l2_streamparm parm;
    memset(&parm, 0, sizeof(parm));
    parm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    parm.parm.capture.timeperframe.numerator = 1;
    parm.parm.capture.timeperframe.denominator = 30;
    if (ioctl(fd, VIDIOC_S_PARM, &parm) == 0) {
        printf("Set parm OK: requested 30 fps\n");
    } else {
        printf("Set parm failed: %s (driver 拒绝, expected)\n", strerror(errno));
    }
    memset(&parm, 0, sizeof(parm));
    parm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(fd, VIDIOC_G_PARM, &parm) == 0) {
        printf("Actual: %d/%d fps = %.2f fps\n",
               parm.parm.capture.timeperframe.denominator,
               parm.parm.capture.timeperframe.numerator,
               (double)parm.parm.capture.timeperframe.denominator /
               parm.parm.capture.timeperframe.numerator);
    }

    // 5. Request mmap buffers
    struct v4l2_requestbuffers req;
    memset(&req, 0, sizeof(req));
    req.count = N_BUFS;
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_MMAP;
    if (ioctl(fd, VIDIOC_REQBUFS, &req) < 0) {
        perror("REQBUFS");
        return 1;
    }
    if (req.count < 2) {
        printf("REQBUFS returned %d buffers (need >=2)\n", req.count);
        return 1;
    }
    printf("REQBUFS got %d buffers\n", req.count);
    N_BUFS = req.count;

    // 6. mmap + QBUF each
    std::vector<void*> bufs(N_BUFS);
    std::vector<size_t> buf_lens(N_BUFS);
    for (int i = 0; i < N_BUFS; i++) {
        struct v4l2_buffer buf;
        memset(&buf, 0, sizeof(buf));
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = i;
        if (ioctl(fd, VIDIOC_QUERYBUF, &buf) < 0) { perror("QUERYBUF"); return 1; }
        bufs[i] = mmap(NULL, buf.length, PROT_READ, MAP_SHARED, fd, buf.m.offset);
        if (bufs[i] == MAP_FAILED) { perror("mmap"); return 1; }
        buf_lens[i] = buf.length;
        if (ioctl(fd, VIDIOC_QBUF, &buf) < 0) { perror("QBUF"); return 1; }
    }

    // 7. STREAMON
    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(fd, VIDIOC_STREAMON, &type) < 0) { perror("STREAMON"); return 1; }
    printf("STREAMON\n\n");

    // 8. Warmup 5s (filling kernel queue, sensor warm up)
    struct timespec t_warmup_start, t_now;
    clock_gettime(CLOCK_MONOTONIC, &t_warmup_start);
    int warmup_count = 0;
    while (1) {
        clock_gettime(CLOCK_MONOTONIC, &t_now);
        if (timespec_diff_ms(t_now, t_warmup_start) > 5000.0) break;
        struct v4l2_buffer buf;
        memset(&buf, 0, sizeof(buf));
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        // BLOCKING DQBUF: 等 frame ready
        if (ioctl(fd, VIDIOC_DQBUF, &buf) < 0) { perror("DQBUF warmup"); return 1; }
        if (ioctl(fd, VIDIOC_QBUF, &buf) < 0) { perror("QBUF warmup"); return 1; }
        warmup_count++;
    }
    printf("Warmup: %d frames in 5s\n\n", warmup_count);

    // 9. Benchmark
    int count = 0;
    double total_dt_ms = 0;
    double min_dt_ms = 99999, max_dt_ms = 0;
    struct timespec t_bench_start = t_now;
    struct timespec last_print = t_now;

    while (1) {
        struct v4l2_buffer buf;
        memset(&buf, 0, sizeof(buf));
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;

        struct timespec t0, t1;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        if (ioctl(fd, VIDIOC_DQBUF, &buf) < 0) { perror("DQBUF"); return 1; }
        // 不读 buf 的数据, 不解析 frame, 立刻 requeue
        if (ioctl(fd, VIDIOC_QBUF, &buf) < 0) { perror("QBUF"); return 1; }
        clock_gettime(CLOCK_MONOTONIC, &t1);

        double dt_ms = timespec_diff_ms(t1, t0);
        total_dt_ms += dt_ms;
        if (dt_ms < min_dt_ms) min_dt_ms = dt_ms;
        if (dt_ms > max_dt_ms) max_dt_ms = dt_ms;
        count++;

        struct timespec t_now_b;
        clock_gettime(CLOCK_MONOTONIC, &t_now_b);
        double since_print = timespec_diff_ms(t_now_b, last_print) / 1000.0;
        if (since_print > 1.0) {
            double elapsed = timespec_diff_ms(t_now_b, t_bench_start) / 1000.0;
            double fps = count / elapsed;
            printf("  count=%4d fps=%5.2f | DQBUF+QBUF ms avg=%.2f min=%.2f max=%.2f\n",
                   count, fps, total_dt_ms/count, min_dt_ms, max_dt_ms);
            last_print = t_now_b;
            total_dt_ms = 0;
            min_dt_ms = 99999;
            max_dt_ms = 0;
        }
        if (timespec_diff_ms(t_now_b, t_bench_start) > (double)DURATION_S * 1000.0) break;
    }

    // 10. STREAMOFF + cleanup
    ioctl(fd, VIDIOC_STREAMOFF, &type);
    for (int i = 0; i < N_BUFS; i++) munmap(bufs[i], buf_lens[i]);
    close(fd);

    return 0;
}