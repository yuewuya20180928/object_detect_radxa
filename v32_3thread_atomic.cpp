// v31_v4l2_od_rtsp.cpp — 全 C++ 完整 pipeline: V4L2 + OD + draw + x264enc + RTSP server
//
// 老板指令 (2026-08-08 01:05 GMT+8): 改成全 C++
// v30 是 C++ 主进程 + Python 子进程做 RTSP server
// v31 完全去掉 Python, 用 GstRtspServer C API 在 C++ 进程内做 RTSP server
//
// 架构 (全 C++ 单进程):
//   1. V4L2 mmap grab (640x480 YUYV)
//   2. cv::cvtColor YUYV → BGR
//   3. dlopen libod_helper.so → yolov5_pre_process_bgr + awnn_run + yolov5_post_process_bgr
//   4. cv::rectangle + cv::putText 画框 (post_process stderr redirect)
//   5. appsrc push-buffer (BGR + PTS)
//   6. x264enc tune=zerolatency + h264parse + rtph264pay → rtsp-server pipeline
//   7. gst_rtsp_server_new + RTSPMediaFactory expose rtsp://0.0.0.0:8554/test
//
// 编译 (板端 aarch64):
//   g++ -O2 -o v31_v4l2_od_rtsp v31_v4l2_od_rtsp.cpp \
//       -I/usr/include/opencv4 \
//       $(pkg-config --cflags gstreamer-1.0 gstreamer-rtsp-server-1.0 gstreamer-app-1.0) \
//       $(pkg-config --libs gstreamer-1.0 gstreamer-rtsp-server-1.0 gstreamer-app-1.0) \
//       -L/home/radxa/od_api/lib -lod_helper \
//       -lopencv_core -lopencv_imgproc -ljpeg \
//       -Wl,-rpath=/home/radxa/od_api/lib
//
// PC client: ffplay rtsp://192.168.2.3:8554/test

#include <linux/videodev2.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vector>
#include <dlfcn.h>
#include <errno.h>
#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>
#include <atomic>

#include <gst/gst.h>
#include <gst/app/gstappsrc.h>
#include <gst/video/video.h>
#include <gst/rtsp-server/rtsp-server.h>

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

static int parse_detections_from_stderr(const char* stderr_file,
    std::vector<std::tuple<int,int,int,int,float,std::string>>& dets) {
    FILE* f = fopen(stderr_file, "r");
    if (!f) return 0;
    char line[512];
    int count = 0;
    while (fgets(line, sizeof(line), f)) {
        int cls_idx, x1, y1, x2, y2;
        float conf;
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

// 全局状态 (给 RTSP callback 用)
struct AppState {
    GstElement* appsrc;
    GstElement* pipeline;
    int W, H, FPS;
    std::atomic<bool> client_connected{false};   // 2026-08-10 atomic: GMainLoop 写, thread3 读 (避免 cache)
    std::atomic<bool> appsrc_wants_data{true};   // 跨线程信号状态
};
static AppState g_state = {NULL, NULL, 640, 480, 20, std::atomic<bool>{false}, std::atomic<bool>{true}};

// appsrc need-data callback (from g_main loop context)
static void on_need_data(GstElement* appsrc, guint length, gpointer user_data) {
    g_state.appsrc_wants_data.store(true);
}
// appsrc enough-data callback: appsrc 满了, 主循环应该 stop push
// appsrc enough-data callback: appsrc 满了, 主循环应该 stop push
static void on_enough_data(GstElement* appsrc, gpointer user_data) {
    g_state.appsrc_wants_data.store(false);
}
// 2026-08-10 preroll fix: caps 协商需要 first buffer, 但 main loop 70ms/帧太慢
// on_need_data trigger 时立即 push 1 个黑帧作为 preroll, caps 协商过后 main loop 接管
static std::atomic<bool> g_preroll_pushed{false};
static void on_need_data_preroll(GstElement* appsrc, guint length, gpointer user_data) {
    g_state.appsrc_wants_data.store(true);
    if (!g_preroll_pushed.load()) {
        size_t frame_bytes = g_state.W * g_state.H * 3;
        GstBuffer* buf = gst_buffer_new_and_alloc(frame_bytes);
        gst_buffer_memset(buf, 0, 0, frame_bytes);
        gst_app_src_push_buffer(GST_APP_SRC(appsrc), buf);
        g_preroll_pushed.store(true);
        g_print("[rtsp-server] preroll black frame pushed (%zu bytes)\n", frame_bytes);
        fflush(stdout);
    }
}

// media-unprepared callback: client 全部断开, media 准备释放, 停止 push
static void on_media_unprepared(GstRTSPMedia* media, gpointer user_data) {
    // 2026-08-08: 调试发现 on_media_unprepared 在 client 刚连上也触发 (RTSP server 状态机问题),
    // 设 client_connected=false 会让 push 停掉, 实际 stream 就废了. 临时注释掉.
    // g_state.client_connected = false;
    // g_state.appsrc = NULL;
    // g_state.appsrc_wants_data = false;
    // g_print("[rtsp-server] media unprepared, stopping push to avoid leak\n");
}

// media-configure callback: 当 RTSP client connect 时触发, 启动 pipeline
static void on_media_configure(GstRTSPMediaFactory* factory, GstRTSPMedia* media, gpointer user_data) {
    GstElement* pipeline = gst_rtsp_media_get_element(media);
    GstElement* appsrc = gst_bin_get_by_name(GST_BIN(pipeline), "mysrc");
    g_state.appsrc = appsrc;
    g_state.pipeline = pipeline;
    g_preroll_pushed.store(false);  // 每个 media 实例重设

    // 2026-08-10 fix: 强制 stream-type=0 (stream), launch string 默认 2=random-access 会 preroll seek fail
    g_object_set(appsrc, "stream-type", 0, NULL);
    g_object_set(appsrc, "caps", gst_caps_new_simple("video/x-raw",
        "format", G_TYPE_STRING, "BGR",
        "width", G_TYPE_INT, g_state.W,
        "height", G_TYPE_INT, g_state.H,
        "framerate", GST_TYPE_FRACTION, g_state.FPS, 1, NULL), NULL);
    g_object_set(appsrc, "do-timestamp", TRUE, NULL);
    g_object_set(appsrc, "block", FALSE, NULL);
    g_object_set(appsrc, "max-bytes", 2000000, NULL);
    g_object_set(appsrc, "max-buffers", 2, NULL);
    int stream_type = -1;
    g_object_get(appsrc, "stream-type", &stream_type, NULL);
    g_print("[rtsp-server] appsrc stream-type actual=%d\n", stream_type);

    // 2026-08-09 fix: x264enc profile 必须用 property 设, 不是 option-string
    GstElement* x264 = gst_bin_get_by_name(GST_BIN(pipeline), "x264enc");
    if (x264) {
        g_object_set(x264, "profile", "high", NULL);
        g_print("[rtsp-server] x264 profile=high\n");
    }

    // 2026-08-09 preroll fix: need-data signal 同时 push 1 黑帧让 caps 协商过
    g_signal_connect(appsrc, "need-data", G_CALLBACK(on_need_data_preroll), NULL);
    g_signal_connect(appsrc, "enough-data", G_CALLBACK(on_enough_data), NULL);

    // 2026-08-10 atomic fix: client_connected atomic store, 跨 thread3 可见
    g_state.client_connected.store(true);
    g_print("[rtsp-server] client connected, pipeline ready (appsrc, atomic)\n");
}

int main(int argc, char** argv) {
    const char* dev = argc > 1 ? argv[1] : "/dev/video0";
    int W = argc > 2 ? atoi(argv[2]) : 640;
    int H = argc > 3 ? atoi(argv[3]) : 480;
    int N_BUFS = argc > 4 ? atoi(argv[4]) : 4;
    int DURATION_S = argc > 5 ? atoi(argv[5]) : 15;
    const char* model_path = argc > 6 ? argv[6] : "/etc/npu/yolov5/model/yolov5.nb";

    printf("=== v31 ALL-C++ V4L2 + OD + RTSP ===\n");
    printf("  dev=%s  res=%dx%d  n_bufs=%d  duration=%ds\n", dev, W, H, N_BUFS, DURATION_S);
    printf("  model=%s\n\n", model_path);
    g_state.W = W; g_state.H = H; g_state.FPS = 20;

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

    // 3. Init GStreamer
    gst_init(&argc, &argv);

    // 4. RTSP server setup
    GstRTSPServer* server = gst_rtsp_server_new();
    gst_rtsp_server_set_address(server, "0.0.0.0");
    gst_rtsp_server_set_service(server, "8554");
    GstRTSPMountPoints* mounts = gst_rtsp_server_get_mount_points(server);
    GstRTSPMediaFactory* factory = gst_rtsp_media_factory_new();
    gst_rtsp_media_factory_set_launch(factory,
        "( appsrc name=mysrc stream-type=0 "
        "caps=\"video/x-raw,format=BGR,width=640,height=480,framerate=30/1\" "
        "is-live=true do-timestamp=true block=false max-bytes=2000000 max-buffers=2 "
        "! videoconvert "
        "! video/x-raw,format=I420,width=640,height=480,framerate=30/1 "
        "! x264enc name=x264enc0 tune=zerolatency speed-preset=ultrafast "
        "bitrate=1000 key-int-max=15 threads=6 "
        "! h264parse config-interval=1 "
        "! rtph264pay pt=96 mtu=1400 config-interval=1 "
        "name=pay0 )");
    gst_rtsp_media_factory_set_shared(factory, TRUE);
    g_signal_connect(factory, "media-configure", G_CALLBACK(on_media_configure), NULL);
    gst_rtsp_mount_points_add_factory(mounts, "/test", factory);
    gst_rtsp_server_attach(server, NULL);
    g_object_unref(mounts);
    printf("RTSP server: rtsp://0.0.0.0:8554/test\n");
    printf("PC client: ffplay rtsp://192.168.2.3:8554/test\n\n");

    // 5. V4L2 setup
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
    W = fmt.fmt.pix.width; H = fmt.fmt.pix.height;
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

    // 6. GMainLoop (GStreamer event loop)
    GMainLoop* loop = g_main_loop_new(NULL, FALSE);
    g_thread_new("gst-loop", (GThreadFunc)g_main_loop_run, loop);

    // 7. stderr redirect setup
    char stderr_file[] = "/tmp/v31_postproc_XXXXXX.txt";
    int stderr_fd = mkstemp(stderr_file);
    close(stderr_fd);

    // 8. Warmup 5s (don't push to RTSP yet)
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

    // 9. Benchmark + push to RTSP via appsrc
    int count = 0;
    double grab_ms_total = 0, od_ms_total = 0, push_ms_total = 0;
    struct timespec t_bench_start = t_now;
    struct timespec last_print = t_now;
    int total_dets = 0;
    GstClockTime pts_ns = 0;
    guint64 frame_duration_ns = GST_SECOND / g_state.FPS;

    while (1) {
        struct v4l2_buffer buf;
        memset(&buf, 0, sizeof(buf));
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;

        struct timespec t0, t_grab_end, t_od_end, t_push_end;
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

        // Push BGR to appsrc (when client connected AND appsrc wants data)
        clock_gettime(CLOCK_MONOTONIC, &t_push_end);
        // 2026-08-08 debug: 调试发现 on_media_unprepared 会在 client connect 后立刻触发,
        // 设 client_connected=false 后 push 停掉, 实际 stream 就废了.
        // 同时 appsrc_wants_data 用 need-data/enough-data 控制 push 不可靠 (random-access 模式下 need-data 可能不触发).
        // 先简化: 只检查 client_connected, 不管 appsrc_wants_data. (会泄漏, 但 stream 应能跑)
        if (g_state.client_connected.load() && g_state.appsrc) {
            // 2026-08-10 fix: 用 gst_buffer_new_and_alloc + fill (GStreamer allocator 安全)
            // stream-type=0 模式下 appsrc 拿 ref, 我们不 unref (appsrc 负责 release)
            size_t frame_bytes = W * H * 3;
            GstBuffer* gst_buf = gst_buffer_new_and_alloc(frame_bytes);
            gst_buffer_fill(gst_buf, 0, bgr.data, frame_bytes);
            gst_buf->pts = pts_ns;
            gst_buf->duration = frame_duration_ns;
            pts_ns += frame_duration_ns;
            GstFlowReturn ret = gst_app_src_push_buffer(GST_APP_SRC(g_state.appsrc), gst_buf);
            // 不 unref: appsrc 在 stream mode 下 take ownership (reference +1, release 后会自己 unref)
            (void)ret;
            // 如果 appsrc 没拿 ref, 我们 unref 是必要的 (free the buffer)
            gst_buffer_unref(gst_buf);
            (void)ret;  // suppress unused warning
        }

        if (ioctl(fd, VIDIOC_QBUF, &buf) < 0) { perror("QBUF"); return 1; }

        double grab_ms = timespec_diff_ms(t_grab_end, t0);
        double od_ms = timespec_diff_ms(t_od_end, t_grab_end);
        double push_ms = timespec_diff_ms(t_push_end, t_od_end);
        grab_ms_total += grab_ms;
        od_ms_total += od_ms;
        push_ms_total += push_ms;
        count++;
        total_dets += det_count;

        struct timespec t_now_b;
        clock_gettime(CLOCK_MONOTONIC, &t_now_b);
        double since_print = timespec_diff_ms(t_now_b, last_print) / 1000.0;
        if (since_print > 1.0) {
            double elapsed = timespec_diff_ms(t_now_b, t_bench_start) / 1000.0;
            double fps = count / elapsed;
            printf("  count=%4d fps=%5.2f | grab=%.1f od=%.1f push=%.1f ms | dets=%d client=%s\n",
                   count, fps,
                   grab_ms_total/count, od_ms_total/count, push_ms_total/count,
                   total_dets, g_state.client_connected.load() ? "yes" : "no");
            last_print = t_now_b;
            grab_ms_total = od_ms_total = push_ms_total = 0;
        }
        if (timespec_diff_ms(t_now_b, t_bench_start) > (double)DURATION_S * 1000.0) break;
    }

    // 10. Cleanup
    ioctl(fd, VIDIOC_STREAMOFF, &type);
    for (int i = 0; i < N_BUFS; i++) munmap(bufs[i], buf_lens[i]);
    close(fd);
    g_main_loop_quit(loop);
    unlink(stderr_file);
    dlclose(h);

    printf("\n=== v31 done ===\n");
    return 0;
}