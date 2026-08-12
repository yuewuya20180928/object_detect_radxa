// v30_v4l2_od_rtsp.cpp — 基于 v29, 加 RTSP server 子进程推流
//
// 老板指令 (2026-08-08 00:37 GMT+8): 基于 v29, 实现摄像头数据采集+目标检测+画框+编码+RTSP推流
//
// 架构:
//   C++ 主进程 (本程序):
//     - V4L2 mmap grab (复用 v29)
//     - cv::cvtColor YUYV → BGR
//     - yolov5_pre_process_bgr + awnn_run + yolov5_post_process_bgr (复用 v29)
//     - cv::rectangle + cv::putText 画框 (post_process stderr redirect 解析 detection)
//     - 写 raw BGR 到 Unix socket /tmp/v30_bgr.sock (frame_size + frame_data)
//   Python 子进程 (v30_rtsp_server.py):
//     - gi GstRtspServer + RTSPMediaFactory with appsrc
//     - appsrc pull raw BGR from queue (background thread reads from unix socket)
//     - x264enc tune=zerolatency + rtph264pay + RTSP server 0.0.0.0:8554/test
//
// PC client: ffplay rtsp://192.168.2.3:8554/test
//
// 编译 (板端 aarch64):
//   g++ -O2 -o v30_v4l2_od_rtsp v30_v4l2_od_rtsp.cpp \
//       -I/usr/include/opencv4 \
//       -L/home/radxa/od_api/lib -lod_helper \
//       -lopencv_core -lopencv_imgproc -lopencv_imgcodecs \
//       -ljpeg -Wl,-rpath=/home/radxa/od_api/lib

#include <linux/videodev2.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vector>
#include <dlfcn.h>
#include <errno.h>
#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/imgcodecs.hpp>

// libod_helper.so C API
typedef void* (*t_awnn_create)(const char*);
typedef void (*t_awnn_init)(void);
typedef void* (*t_yolov5_pre_process_bgr)(const unsigned char*, int, int, unsigned int*);
typedef int (*t_yolov5_post_process_bgr)(void**, int, int);
typedef void (*t_awnn_set_input_buffers)(void*, void**);
typedef void (*t_awnn_run)(void*);
typedef void** (*t_awnn_get_output_buffers)(void*);

static double timespec_diff_ms(const struct timespec& a, const struct timespec& b) {
    return (a.tv_sec - b.tv_sec) * 1000.0 + (a.tv_nsec - b.tv_nsec) / 1e6;
}

static int parse_detections_from_stderr(const char* stderr_file, std::vector<std::tuple<int,int,int,int,float,std::string>>& dets) {
    FILE* f = fopen(stderr_file, "r");
    if (!f) return 0;
    char line[512];
    int count = 0;
    while (fgets(line, sizeof(line), f)) {
        int cls_idx, x1, y1, x2, y2;
        float conf;
        char cls_name[64];
        // 试 yolov5 实际输出格式: 解析 "label conf x1 y1 x2 y2" 或 "cls_idx x1 y1 x2 y2 conf"
        if (sscanf(line, "%s %f %d %d %d %d", cls_name, &conf, &x1, &y1, &x2, &y2) == 6) {
            dets.push_back(std::make_tuple(x1, y1, x2, y2, conf, std::string(cls_name)));
            count++;
        } else if (sscanf(line, "%d %d %d %d %d %f", &cls_idx, &x1, &y1, &x2, &y2, &conf) == 6) {
            char buf[32]; snprintf(buf, sizeof(buf), "cls%d", cls_idx);
            dets.push_back(std::make_tuple(x1, y1, x2, y2, conf, std::string(buf)));
            count++;
        }
    }
    fclose(f);
    return count;
}

int main(int argc, char** argv) {
    const char* dev = argc > 1 ? argv[1] : "/dev/video0";
    int W = argc > 2 ? atoi(argv[2]) : 640;
    int H = argc > 3 ? atoi(argv[3]) : 480;
    int N_BUFS = argc > 4 ? atoi(argv[4]) : 4;
    int DURATION_S = argc > 5 ? atoi(argv[5]) : 15;
    const char* model_path = argc > 6 ? argv[6] : "/etc/npu/yolov5/model/yolov5.nb";
    const char* sock_path = "/tmp/v30_bgr.sock";

    printf("=== v30 V4L2 + OD + RTSP push (via unix socket) ===\n");
    printf("  dev=%s  res=%dx%d  n_bufs=%d  duration=%ds\n", dev, W, H, N_BUFS, DURATION_S);
    printf("  model=%s  sock=%s\n\n", model_path, sock_path);

    // 0. Clean socket
    unlink(sock_path);

    // 1. Load libod_helper.so
    void* h = dlopen("/home/radxa/od_api/lib/libod_helper.so", RTLD_NOW | RTLD_GLOBAL);
    if (!h) { fprintf(stderr, "dlopen: %s\n", dlerror()); return 1; }
    auto awnn_init = (t_awnn_init)dlsym(h, "awnn_init");
    auto awnn_create = (t_awnn_create)dlsym(h, "awnn_create");
    auto awnn_run = (t_awnn_run)dlsym(h, "awnn_run");
    auto awnn_set_input_buffers = (t_awnn_set_input_buffers)dlsym(h, "awnn_set_input_buffers");
    auto awnn_get_output_buffers = (t_awnn_get_output_buffers)dlsym(h, "awnn_get_output_buffers");
    auto yolov5_pre_process_bgr = (t_yolov5_pre_process_bgr)dlsym(h, "yolov5_pre_process_bgr");
    auto yolov5_post_process_bgr = (t_yolov5_post_process_bgr)dlsym(h, "yolov5_post_process_bgr");
    if (!awnn_init || !awnn_create || !awnn_run || !awnn_set_input_buffers ||
        !awnn_get_output_buffers || !yolov5_pre_process_bgr || !yolov5_post_process_bgr) {
        fprintf(stderr, "dlsym failed: %s\n", dlerror()); return 1;
    }

    // 2. Init NPU
    awnn_init();
    void* ctx = awnn_create(model_path);
    if (!ctx) { fprintf(stderr, "awnn_create failed\n"); return 1; }
    printf("NPU ready (model=%s)\n", model_path);

    // 3. fork + exec Python RTSP server
    pid_t py_pid = fork();
    if (py_pid == 0) {
        execlp("python3", "python3", "/tmp/v30_rtsp_server.py", NULL);
        perror("execlp python3");
        _exit(1);
    }
    printf("python RTSP server child pid=%d\n", py_pid);
    sleep(3.0); // wait for python server to listen on /tmp/v30_bgr.sock

    // 4. V4L2 setup
    int fd = open(dev, O_RDWR);
    if (fd < 0) { perror("open"); return 1; }
    struct v4l2_format fmt;
    memset(&fmt, 0, sizeof(fmt));
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    fmt.fmt.pix.width = W;
    fmt.fmt.pix.height = H;
    fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_YUYV;
    fmt.fmt.pix.field = V4L2_FIELD_NONE;
    if (ioctl(fd, VIDIOC_S_FMT, &fmt) < 0) { perror("S_FMT"); return 1; }
    W = fmt.fmt.pix.width;
    H = fmt.fmt.pix.height;
    printf("Set format OK: %dx%d YUYV\n", W, H);

    struct v4l2_requestbuffers req;
    memset(&req, 0, sizeof(req));
    req.count = N_BUFS;
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_MMAP;
    if (ioctl(fd, VIDIOC_REQBUFS, &req) < 0) { perror("REQBUFS"); return 1; }
    N_BUFS = req.count;

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
        buf_lens[i] = buf.length;
        if (ioctl(fd, VIDIOC_QBUF, &buf) < 0) { perror("QBUF"); return 1; }
    }
    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(fd, VIDIOC_STREAMON, &type) < 0) { perror("STREAMON"); return 1; }
    printf("STREAMON, %d buffers\n\n", N_BUFS);

    // 5. connect to unix socket (Python RTSP server)
    int sock = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, sock_path, sizeof(addr.sun_path) - 1);
    int retry = 20;
    while (retry-- > 0) {
        if (connect(sock, (struct sockaddr*)&addr, sizeof(addr)) == 0) break;
        usleep(100000);
    }
    if (retry <= 0) { perror("connect to RTSP server socket"); return 1; }
    printf("connected to Python RTSP server at %s\n\n", sock_path);

    // 6. stderr redirect setup
    char stderr_file[] = "/tmp/v30_postproc_XXXXXX.txt";
    int stderr_fd = mkstemp(stderr_file);
    close(stderr_fd);

    // 7. Warmup 5s (no socket send)
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
        if (ioctl(fd, VIDIOC_DQBUF, &buf) < 0) { perror("DQBUF warmup"); return 1; }
        cv::Mat yuyv_mat(H, W, CV_8UC2, bufs[buf.index]);
        cv::Mat bgr;
        cv::cvtColor(yuyv_mat, bgr, cv::COLOR_YUV2BGR_YUYV);

        unsigned int in_size = 0;
        fflush(stderr);
        int saved = dup(STDERR_FILENO);
        int tmp_fd = open(stderr_file, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        dup2(tmp_fd, STDERR_FILENO);
        close(tmp_fd);
        void* in_buf = yolov5_pre_process_bgr(bgr.data, W, H, &in_size);
        if (in_buf) {
            void* ib = in_buf;
            awnn_set_input_buffers(ctx, &ib);
            awnn_run(ctx);
            void** outputs = awnn_get_output_buffers(ctx);
            if (outputs) yolov5_post_process_bgr(outputs, W, H);
        }
        fflush(stderr);
        dup2(saved, STDERR_FILENO);
        close(saved);
        if (in_buf) free(in_buf);
        if (ioctl(fd, VIDIOC_QBUF, &buf) < 0) { perror("QBUF warmup"); return 1; }
        warmup_count++;
    }
    printf("Warmup: %d frames in 5s\n\n", warmup_count);

    // 8. Benchmark + send BGR to socket
    int count = 0;
    double grab_ms_total = 0, od_ms_total = 0, send_ms_total = 0;
    struct timespec t_bench_start = t_now;
    struct timespec last_print = t_now;
    int total_dets = 0;
    int send_errors = 0;

    while (1) {
        struct v4l2_buffer buf;
        memset(&buf, 0, sizeof(buf));
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;

        struct timespec t0, t_grab_end, t_od_end, t_send_end;
        clock_gettime(CLOCK_MONOTONIC, &t0);

        if (ioctl(fd, VIDIOC_DQBUF, &buf) < 0) { perror("DQBUF"); return 1; }
        cv::Mat yuyv_mat(H, W, CV_8UC2, bufs[buf.index]);
        cv::Mat bgr;
        cv::cvtColor(yuyv_mat, bgr, cv::COLOR_YUV2BGR_YUYV);
        clock_gettime(CLOCK_MONOTONIC, &t_grab_end);

        // OD
        unsigned int in_size = 0;
        fflush(stderr);
        int saved = dup(STDERR_FILENO);
        int tmp_fd = open(stderr_file, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        dup2(tmp_fd, STDERR_FILENO);
        close(tmp_fd);
        void* in_buf = yolov5_pre_process_bgr(bgr.data, W, H, &in_size);
        int det_count = 0;
        if (in_buf) {
            void* ib = in_buf;
            awnn_set_input_buffers(ctx, &ib);
            awnn_run(ctx);
            void** outputs = awnn_get_output_buffers(ctx);
            if (outputs) {
                yolov5_post_process_bgr(outputs, W, H);
                std::vector<std::tuple<int,int,int,int,float,std::string>> dets;
                det_count = parse_detections_from_stderr(stderr_file, dets);
                for (auto& d : dets) {
                    int x1, y1, x2, y2; float conf; std::string cls;
                    std::tie(x1, y1, x2, y2, conf, cls) = d;
                    cv::rectangle(bgr, cv::Point(x1,y1), cv::Point(x2,y2), cv::Scalar(0,255,0), 2);
                    cv::putText(bgr, cls.c_str(), cv::Point(x1, std::max(15,y1-5)),
                                cv::FONT_HERSHEY_SIMPLEX, 0.5, cv::Scalar(0,255,0), 1);
                }
            }
        }
        fflush(stderr);
        dup2(saved, STDERR_FILENO);
        close(saved);
        if (in_buf) free(in_buf);
        clock_gettime(CLOCK_MONOTONIC, &t_od_end);

        // Send BGR via unix socket to Python RTSP server
        uint32_t frame_size = W * H * 3;
        struct iovec iov[2];
        iov[0].iov_base = &frame_size;
        iov[0].iov_len = sizeof(frame_size);
        iov[1].iov_base = bgr.data;
        iov[1].iov_len = frame_size;
        ssize_t sent = writev(sock, iov, 2);
        if (sent < 0) send_errors++;
        clock_gettime(CLOCK_MONOTONIC, &t_send_end);

        if (ioctl(fd, VIDIOC_QBUF, &buf) < 0) { perror("QBUF"); return 1; }

        double grab_ms = timespec_diff_ms(t_grab_end, t0);
        double od_ms = timespec_diff_ms(t_od_end, t_grab_end);
        double send_ms = timespec_diff_ms(t_send_end, t_od_end);
        grab_ms_total += grab_ms;
        od_ms_total += od_ms;
        send_ms_total += send_ms;
        count++;
        total_dets += det_count;

        struct timespec t_now_b;
        clock_gettime(CLOCK_MONOTONIC, &t_now_b);
        double since_print = timespec_diff_ms(t_now_b, last_print) / 1000.0;
        if (since_print > 1.0) {
            double elapsed = timespec_diff_ms(t_now_b, t_bench_start) / 1000.0;
            double fps = count / elapsed;
            printf("  count=%4d fps=%5.2f | grab=%.1f od=%.1f send=%.1f ms | dets=%d send_err=%d\n",
                   count, fps,
                   grab_ms_total/count, od_ms_total/count, send_ms_total/count,
                   total_dets, send_errors);
            last_print = t_now_b;
            grab_ms_total = od_ms_total = send_ms_total = 0;
        }
        if (timespec_diff_ms(t_now_b, t_bench_start) > (double)DURATION_S * 1000.0) break;
    }

    // 9. Cleanup
    ioctl(fd, VIDIOC_STREAMOFF, &type);
    for (int i = 0; i < N_BUFS; i++) munmap(bufs[i], buf_lens[i]);
    close(fd);
    close(sock);
    unlink(sock_path);
    kill(py_pid, SIGTERM);
    waitpid(py_pid, nullptr, 0);
    unlink(stderr_file);
    dlclose(h);

    printf("\n=== v30 done ===\n");
    return 0;
}