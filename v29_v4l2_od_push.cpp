// v29_v4l2_od_push.cpp — v28 V4L2 + libod_helper.so OD + jpeg+spawn gst-launch udpsink
//
// 老板指令 (2026-08-08 00:24 GMT+8): 基于当前 C++ 程序, 开启目标检测和 rtsp 推流, 再测试帧率
//
// 架构:
//   父进程 (本程序):
//     - V4L2 mmap grab (复用 v28)
//     - cv::cvtColor YUYV → BGR
//     - yolov5_pre_process_bgr (libod_helper.so) → INT8 tensor
//     - awnn_run (libod_helper.so) → NPU inference
//     - yolov5_post_process_bgr (libod_helper.so) → boxes (via stderr redirect)
//     - cv::rectangle 画框
//     - cv::imencode jpg → write /tmp/v29_NNNNNN.jpg
//     - QBUF
//   子进程 (fork + execlp gst-launch-1.0):
//     - multifilesrc location=/tmp/v29_%06d.jpg loop=false caps=image/jpeg,framerate=20/1
//     - ! jpegdec ! videoconvert ! x264enc tune=zerolatency
//     - ! rtph264pay pt=96 config-interval=1
//     - ! udpsink host=192.168.2.4 port=8554
//
// 编译 (板端 aarch64):
//   g++ -O2 -o v29_v4l2_od_push v29_v4l2_od_push.cpp \
//       -I/usr/include/opencv4 \
//       -L/home/radxa/od_api/lib -lod_helper \
//       -lopencv_core -lopencv_imgproc -lopencv_imgcodecs \
//       -ljpeg -Wl,-rpath=/home/radxa/od_api/lib

#include <linux/videodev2.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <sys/stat.h>
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

// libod_helper.so C API (from nm -D)
typedef void* (*t_awnn_create)(const char*);
typedef void (*t_awnn_destroy)(void*);
typedef void (*t_awnn_init)(void);
typedef void (*t_awnn_uninit)(void);
// yolov5_pre_process_bgr returns void* (量化后 INT8 input tensor)
// argtypes: (const unsigned char* bgr, int w, int h, unsigned int* file_size)
typedef void* (*t_yolov5_pre_process_bgr)(const unsigned char*, int, int, unsigned int*);
typedef int (*t_yolov5_post_process_bgr)(void**, int, int);
typedef void (*t_awnn_set_input_buffers)(void*, void**);
typedef void (*t_awnn_run)(void*);
typedef void** (*t_awnn_get_output_buffers)(void*);

static double timespec_to_ms(const struct timespec& t) {
    return t.tv_sec * 1000.0 + t.tv_nsec / 1e6;
}
static double timespec_diff_ms(const struct timespec& a, const struct timespec& b) {
    return (a.tv_sec - b.tv_sec) * 1000.0 + (a.tv_nsec - b.tv_nsec) / 1e6;
}

// Parse detections from stderr (yolov5_post_process_bgr writes to stderr)
static int parse_detections_from_stderr(const char* stderr_file, std::vector<std::tuple<int,int,int,int,float,std::string>>& dets) {
    FILE* f = fopen(stderr_file, "r");
    if (!f) return 0;
    char line[512];
    int count = 0;
    while (fgets(line, sizeof(line), f)) {
        int cls_idx, x1, y1, x2, y2;
        float conf;
        // 格式: "cls=N x1=... y1=... x2=... y2=... conf=..."
        // 或者 yolov5 的实际格式 (跟 py 端一致)
        // 简化: 解析 "label conf x1 y1 x2 y2" 一行一个 detection
        char cls_name[64];
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
    const char* jpg_dir = "/tmp/v29_frames";

    printf("=== v29 V4L2 + OD + jpeg push ===\n");
    printf("  dev=%s  res=%dx%d  n_bufs=%d  duration=%ds\n", dev, W, H, N_BUFS, DURATION_S);
    printf("  model=%s  jpg_dir=%s\n\n", model_path, jpg_dir);

    // 0. Clean jpg dir + create
    system("rm -rf /tmp/v29_frames && mkdir -p /tmp/v29_frames");

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

    // 3. V4L2 open + setup (复用 v28)
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
    printf("Set format OK: %dx%d YUYV sizeimage=%d\n", W, H, fmt.fmt.pix.sizeimage);

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
    printf("STREAMON, %d buffers\n", N_BUFS);

    // 4. fork + exec gst-launch-1.0 multifilesrc udpsink
    pid_t gst_pid = fork();
    if (gst_pid == 0) {
        // child: gst-launch
        // multifilesrc 等 /tmp/v29_frames/frame_%06d.jpg 文件
        // 父进程写文件, 子进程读取 + push UDP
        char location_arg[256];
        snprintf(location_arg, sizeof(location_arg),
                 "%s/frame_%%06d.jpg", jpg_dir);
        execlp("gst-launch-1.0", "gst-launch-1.0",
            "multifilesrc", "loop=false", "index=0",
            (std::string("location=") + location_arg).c_str(),
            "caps=image/jpeg,framerate=20/1",
            "!", "jpegdec", "!",
            "videoconvert", "!",
            "x264enc", "tune=zerolatency", "speed-preset=ultrafast",
            "bitrate=2000", "key-int-max=20", "threads=4", "!",
            "h264parse", "config-interval=1", "!",
            "rtph264pay", "pt=96", "mtu=1400", "config-interval=1", "!",
            "udpsink", "host=192.168.2.4", "port=8554",
            "async=false", "sync=false",
            NULL);
        perror("execlp gst-launch-1.0");
        _exit(1);
    }
    printf("gst-launch-1.0 child pid=%d (pushing to udp://192.168.2.4:8554)\n\n", gst_pid);
    sleep(2.0); // wait for gst to start + caps negotiation

    // 5. Main loop: DQBUF → OD → jpeg write → QBUF
    struct timespec t_warmup_start, t_now;
    clock_gettime(CLOCK_MONOTONIC, &t_warmup_start);
    int warmup_count = 0;
    int frame_idx = 0;
    char stderr_file[] = "/tmp/v29_postproc_XXXXXX.txt";
    int stderr_fd = mkstemp(stderr_file);
    close(stderr_fd);

    // warmup 5s
    while (1) {
        clock_gettime(CLOCK_MONOTONIC, &t_now);
        if (timespec_diff_ms(t_now, t_warmup_start) > 5000.0) break;
        struct v4l2_buffer buf;
        memset(&buf, 0, sizeof(buf));
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        if (ioctl(fd, VIDIOC_DQBUF, &buf) < 0) { perror("DQBUF warmup"); return 1; }
        void* yuyv = bufs[buf.index];

        // YUYV → BGR
        cv::Mat yuyv_mat(H, W, CV_8UC2, yuyv);
        cv::Mat bgr;
        cv::cvtColor(yuyv_mat, bgr, cv::COLOR_YUV2BGR_YUYV);

        // OD
        unsigned int in_size = 0;
        void* in_buf = nullptr;
        // Redirect stderr to file to capture yolov5_post_process_bgr output
        fflush(stderr);
        int saved_stderr = dup(STDERR_FILENO);
        int tmp_fd = open(stderr_file, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        dup2(tmp_fd, STDERR_FILENO);
        close(tmp_fd);

        in_buf = yolov5_pre_process_bgr(bgr.data, W, H, &in_size);

        if (in_buf) {
            void* input_buf = in_buf;
            awnn_set_input_buffers(ctx, &input_buf);
            awnn_run(ctx);
            void** outputs = awnn_get_output_buffers(ctx);
            if (outputs) yolov5_post_process_bgr(outputs, W, H);
        }
        fflush(stderr);
        dup2(saved_stderr, STDERR_FILENO);
        close(saved_stderr);

        if (in_buf) free(in_buf);

        // jpeg encode
        std::vector<int> jpeg_params = {cv::IMWRITE_JPEG_QUALITY, 80};
        std::vector<uchar> jpeg_buf;
        cv::imencode(".jpg", bgr, jpeg_buf, jpeg_params);

        // write to /tmp/v29_frames/frame_NNNNNN.jpg
        char filename[256];
        snprintf(filename, sizeof(filename), "%s/frame_%06d.jpg", jpg_dir, frame_idx++);
        FILE* f = fopen(filename, "wb");
        if (f) {
            fwrite(jpeg_buf.data(), 1, jpeg_buf.size(), f);
            fclose(f);
        }

        // QBUF
        if (ioctl(fd, VIDIOC_QBUF, &buf) < 0) { perror("QBUF warmup"); return 1; }
        warmup_count++;
    }
    printf("Warmup: %d frames in 5s\n\n", warmup_count);

    // 6. Benchmark
    int count = 0;
    double grab_ms_total = 0, od_ms_total = 0, jpeg_ms_total = 0, write_ms_total = 0;
    struct timespec t_bench_start = t_now;
    struct timespec last_print = t_now;
    int total_dets = 0;

    while (1) {
        struct v4l2_buffer buf;
        memset(&buf, 0, sizeof(buf));
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;

        struct timespec t0, t_grab_end, t_od_end, t_jpeg_end, t_write_end;
        clock_gettime(CLOCK_MONOTONIC, &t0);

        if (ioctl(fd, VIDIOC_DQBUF, &buf) < 0) { perror("DQBUF"); return 1; }
        void* yuyv = bufs[buf.index];

        cv::Mat yuyv_mat(H, W, CV_8UC2, yuyv);
        cv::Mat bgr;
        cv::cvtColor(yuyv_mat, bgr, cv::COLOR_YUV2BGR_YUYV);
        clock_gettime(CLOCK_MONOTONIC, &t_grab_end);

        // OD
        unsigned int in_size = 0;
        void* in_buf = nullptr;
        fflush(stderr);
        int saved_stderr = dup(STDERR_FILENO);
        int tmp_fd = open(stderr_file, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        dup2(tmp_fd, STDERR_FILENO);
        close(tmp_fd);

        in_buf = yolov5_pre_process_bgr(bgr.data, W, H, &in_size);
        int det_count = 0;
        if (in_buf) {
            void* input_buf = in_buf;
            awnn_set_input_buffers(ctx, &input_buf);
            awnn_run(ctx);
            void** outputs = awnn_get_output_buffers(ctx);
            if (outputs) {
                yolov5_post_process_bgr(outputs, W, H);
                // parse stderr
                std::vector<std::tuple<int,int,int,int,float,std::string>> dets;
                det_count = parse_detections_from_stderr(stderr_file, dets);
                // draw boxes
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
        dup2(saved_stderr, STDERR_FILENO);
        close(saved_stderr);
        if (in_buf) free(in_buf);
        clock_gettime(CLOCK_MONOTONIC, &t_od_end);

        // jpeg encode + write
        std::vector<int> jpeg_params = {cv::IMWRITE_JPEG_QUALITY, 80};
        std::vector<uchar> jpeg_buf;
        cv::imencode(".jpg", bgr, jpeg_buf, jpeg_params);
        clock_gettime(CLOCK_MONOTONIC, &t_jpeg_end);

        char filename[256];
        snprintf(filename, sizeof(filename), "%s/frame_%06d.jpg", jpg_dir, frame_idx++);
        FILE* f = fopen(filename, "wb");
        if (f) {
            fwrite(jpeg_buf.data(), 1, jpeg_buf.size(), f);
            fclose(f);
        }
        clock_gettime(CLOCK_MONOTONIC, &t_write_end);

        // QBUF
        if (ioctl(fd, VIDIOC_QBUF, &buf) < 0) { perror("QBUF"); return 1; }

        // timing
        double grab_ms = timespec_diff_ms(t_grab_end, t0);
        double od_ms = timespec_diff_ms(t_od_end, t_grab_end);
        double jpeg_ms = timespec_diff_ms(t_jpeg_end, t_od_end);
        double write_ms = timespec_diff_ms(t_write_end, t_jpeg_end);
        grab_ms_total += grab_ms;
        od_ms_total += od_ms;
        jpeg_ms_total += jpeg_ms;
        write_ms_total += write_ms;
        count++;
        total_dets += det_count;

        struct timespec t_now_b;
        clock_gettime(CLOCK_MONOTONIC, &t_now_b);
        double since_print = timespec_diff_ms(t_now_b, last_print) / 1000.0;
        if (since_print > 1.0) {
            double elapsed = timespec_diff_ms(t_now_b, t_bench_start) / 1000.0;
            double fps = count / elapsed;
            printf("  count=%4d fps=%5.2f | grab=%.1f od=%.1f jpeg=%.1f write=%.1f ms | dets=%d\n",
                   count, fps,
                   grab_ms_total/count, od_ms_total/count,
                   jpeg_ms_total/count, write_ms_total/count,
                   total_dets);
            last_print = t_now_b;
            grab_ms_total = od_ms_total = jpeg_ms_total = write_ms_total = 0;
        }
        if (timespec_diff_ms(t_now_b, t_bench_start) > (double)DURATION_S * 1000.0) break;
    }

    // 7. Cleanup
    ioctl(fd, VIDIOC_STREAMOFF, &type);
    for (int i = 0; i < N_BUFS; i++) munmap(bufs[i], buf_lens[i]);
    close(fd);
    kill(gst_pid, SIGTERM);
    waitpid(gst_pid, nullptr, 0);
    unlink(stderr_file);
    // Clean up jpg files
    // system("rm -rf /tmp/v29_frames");  // leave for debug
    dlclose(h);

    printf("\n=== v29 done ===\n");
    return 0;
}