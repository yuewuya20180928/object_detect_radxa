// v31_v4l2_od_rtsp.cpp — 简化版: V4L2 + OD + RTSP server
//
// 2026-08-09 00:30 修 preroll 503:
// - appsrc 接 need-data/enough-data signal (appsrc_wants_data 标志)
// - 不在 on_media_configure 设 client_connected=true (太早, appsrc 还没进 PLAYING → Internal data stream error)
// - main loop 仅在 appsrc_wants_data=true 时 push_buffer
// - 2026-08-08 10:15 重写 debug:
//   * 用 gst_buffer_new_and_alloc (GStreamer allocator, refcount 安全)
//   * caps 用 launch string 设置 (不在 on_media_configure)
//   * 不接 need-data/enough-data/unprepared 信号 (2026-08-09 推翻)
//   * 不设 max-buffers / max-bytes (let appsrc 自由 queue)
//   * profile=high (fix x264 baseline 4:4:4 不支持)
//   * block=false (不阻塞)
//   * set_shared(TRUE) (RTSP server 复用 media)

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
#include <atomic>
#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>

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

// 全局状态 (给 RTSP callback 用) — 2026-08-11 race fix (加 atomic, 保留 v9a 所有 critical fix)
struct AppState {
    std::atomic<GstElement*> appsrc{nullptr};   // P0 fix: 64-bit ptr 跨线程读写改 atomic
    std::atomic<int> client_count{0};           // P0 fix: RTSP client 计数 (替代 bool, 精确跟踪)
    std::atomic<bool> appsrc_wants_data{false}; // appsrc signal control
    int W, H, FPS;                              // 仅 setup 时设, 运行中不变
};
static AppState g_state;

// P0 fix: prepared/unprepared 配对 flag, 防止 on_media_unprepared 在 connect 后立刻误触发
static std::atomic<bool> media_prepared{false};

// 2026-08-09 appsrc signal handlers — 等 appsrc 真正准备好接收再推 buffer
static void on_need_data(GstAppSrc* src, guint length, gpointer user_data) {
    g_state.appsrc_wants_data.store(true);
    g_print("[rtsp-server] appsrc need-data (len=%u), enabling push\n", length);
    fflush(stdout);
}
static void on_enough_data(GstAppSrc* src, gpointer user_data) {
    g_state.appsrc_wants_data.store(false);
    g_print("[rtsp-server] appsrc enough-data, pausing push\n");
    fflush(stdout);
}
// 2026-08-09 preroll fix: caps 协商需要 first buffer, 但 main loop 14.5 FPS 太慢 (~70ms),
// appsrc 进入 PAUSED→PLAYING 时立即需要 buffer 否则报 "not-negotiated" / "Internal data stream error"
// → on_need_data trigger 时立即 push 一个黑帧 (640x480 BGR 全 0) 让 caps 协商过, 然后 main loop 接管
static bool g_preroll_pushed = false;

static void on_need_data_preroll(GstAppSrc* src, guint length, gpointer user_data) {
    on_need_data(src, length, user_data);
    if (!g_preroll_pushed) {
        size_t frame_bytes = g_state.W * g_state.H * 3;
        GstBuffer* buf = gst_buffer_new_and_alloc(frame_bytes);
        gst_buffer_memset(buf, 0, 0, frame_bytes);  // 全 0 = 黑色 BGR
        GstFlowReturn ret = gst_app_src_push_buffer(src, buf);
        g_preroll_pushed = true;
        g_print("[rtsp-server] preroll black frame pushed (ret=%d, bytes=%zu)\n", ret, frame_bytes);
        fflush(stdout);
    }
}

// 2026-08-11 新增: media prepared callback (set_shared(TRUE) 下 shared media 的 prepared 在首次 connect 时触发)
// 配合 on_media_unprepared 做配对, 防误触发
static void on_media_prepared(GstRTSPMedia* media, gpointer user_data) {
    media_prepared.store(true);
    g_print("[rtsp-server] media prepared, ready for client\n");
}

// media-unprepared callback: client 全部断开, media 准备释放, 停止 push
// 2026-08-11 修复: 用 media_prepared flag 防止误触发 (8-08 调试发现 unprepare 在 connect 后立即触发)
static void on_media_unprepared(GstRTSPMedia* media, gpointer user_data) {
    if (media_prepared.exchange(false)) {
        // 真断开: 所有 client 都走了
        g_state.client_count.store(0);
        g_state.appsrc.store(nullptr);
        g_state.appsrc_wants_data.store(false);
        g_print("[rtsp-server] all clients disconnected, push stopped\n");
    } else {
        // 误触发: 未 prepared 就 unprepared (RTSP server 状态机问题)
        g_print("[rtsp-server] spurious unprepare (not prepared), ignored\n");
    }
}

// media-configure callback: 当 RTSP client connect 时触发, 保存 appsrc 指针
// + 强制设置 caps / stream-type / x264 profile (gst_parse_launch 在 RTSP server 上下文不可靠)
static void on_media_configure(GstRTSPMediaFactory* factory, GstRTSPMedia* media, gpointer user_data) {
    GstElement* pipeline = gst_rtsp_media_get_element(media);
    GstElement* appsrc = gst_bin_get_by_name(GST_BIN(pipeline), "mysrc");

    // 强制设置 caps (launch string 的 caps 在 RTSP server 上下文不一定生效)
    GstCaps *caps = gst_caps_new_simple("video/x-raw",
        "format", G_TYPE_STRING, "BGR",
        "width", G_TYPE_INT, g_state.W,
        "height", G_TYPE_INT, g_state.H,
        "framerate", GST_TYPE_FRACTION, g_state.FPS, 1, NULL);
    g_object_set(appsrc, "caps", caps, NULL);
    gst_caps_unref(caps);

    // 2026-08-08 fix: 不强制 stream-type, 用默认 STREAM (0), do_seek 返回 TRUE
    // (random-access 模式下 appsrc 的 do_seek 没有 callback/handler 时返回 FALSE → preroll 503)
    // 2026-08-12 修复推流：appsrc 设 is-live=true 让其提供 clock + segment
    // (老注释 "不要设 is-live" 是错的, 这是 rtpsession "Don't have a clock" 的根因)
    g_object_set(appsrc, "stream-type", 0, NULL);
    g_object_set(appsrc, "is-live", TRUE, NULL);
    g_object_set(appsrc, "do-timestamp", TRUE, NULL);
    g_object_set(appsrc, "block", FALSE, NULL);
    g_object_set(appsrc, "format", GST_FORMAT_TIME, NULL);
    // 2026-08-11 修复: launch string 里 max-bytes=10000000 没生效 (appsrc 仍是 default 200000 bytes),
    // 1 个 BGR buffer (921600 bytes) 超过 max-bytes → 1 个 buffer 就 enough-data → pipeline 不消费 → 死锁
    // 显式用 g_object_set 设大点的 max-bytes 和 max-buffers
    g_object_set(appsrc, "max-bytes", (guint64)10000000, NULL);
    g_object_set(appsrc, "max-buffers", 10, NULL);

    // 验证 stream-type 实际值
    int stream_type = -1;
    g_object_get(appsrc, "stream-type", &stream_type, NULL);
    g_print("[rtsp-server] appsrc stream-type actual=%d (0=stream,1=seekable,2=random-access)\n", stream_type);

    // 2026-08-11 修复: x264enc 的 profile 是 caps property (不是 element property)
    // 已在 launch string 里用 capsfilter video/x-raw,profile=high 强制
    // g_object_set(x264, "profile", "high", NULL) 会报 "no property named 'profile'" 错误
    GstElement* x264 = gst_bin_get_by_name(GST_BIN(pipeline), "x264enc0");
    if (x264) {
        g_print("[rtsp-server] x264 element found (profile via capsfilter)\n");
    }

    // (h264-dump-debug 已关闭, launch string 不含 tee/appsink)

    // 2026-08-11 race fix: atomic store
    g_state.appsrc.store(appsrc);
    int count = g_state.client_count.fetch_add(1) + 1;
    g_state.appsrc_wants_data.store(false);  // 等 appsrc 触发 need-data signal 才置 true
    g_preroll_pushed = false;  // 每个 media instance 重新 preroll

    g_signal_connect(appsrc, "need-data", G_CALLBACK(on_need_data_preroll), NULL);
    g_signal_connect(appsrc, "enough-data", G_CALLBACK(on_enough_data), NULL);
    // 2026-08-11: 监听 media lifecycle, prepared/unprepared 配对
    g_signal_connect(media, "prepared", G_CALLBACK(on_media_prepared), NULL);
    g_signal_connect(media, "unprepared", G_CALLBACK(on_media_unprepared), NULL);

    g_print("[rtsp-server] client connected (count=%d), appsrc=%p\n", count, appsrc);
    fflush(stdout);
}

int main(int argc, char** argv) {
    const char* dev = argc > 1 ? argv[1] : "/dev/video0";
    int W = argc > 2 ? atoi(argv[2]) : 640;
    int H = argc > 3 ? atoi(argv[3]) : 480;
    int N_BUFS = argc > 4 ? atoi(argv[4]) : 4;
    int DURATION_S = argc > 5 ? atoi(argv[5]) : 15;
    const char* model_path = argc > 6 ? argv[6] : "/etc/npu/yolov5/model/yolov5.nb";

    printf("=== v31 ALL-C++ V4L2 + OD + RTSP (debug version) ===\n");
    printf("  dev=%s  res=%dx%d  n_bufs=%d  duration=%ds\n", dev, W, H, N_BUFS, DURATION_S);
    printf("  model=%s\n\n", model_path);
    // 2026-08-13 fix: SDP framerate 跟实际板端稳态输出匹配 (实际 ~14 FPS cumulative, 稳态 10 FPS).
    // 之前 30 FPS 误导 PC 端 mpv/VLC 按 30 FPS 设计 buffer, 实际 14 FPS 时 buffer 永远吃不满, 任何 spike 都直接体现在画面上.
    g_state.W = W; g_state.H = H; g_state.FPS = 14;

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
        fprintf(stderr, "dlsym failed: %s\n", dlerror()); return 1; }
    awnn_init();
    void* ctx = awnn_create(model_path);
    if (!ctx) { fprintf(stderr, "awnn_create failed\n"); return 1; }
    printf("NPU ready (model=%s)\n\n", model_path);

    // 2. Init GStreamer
    gst_init(&argc, &argv);

    // 3. RTSP server setup - launch string defines the pipeline (with caps)
    GstRTSPServer* server = gst_rtsp_server_new();
    gst_rtsp_server_set_address(server, "0.0.0.0");
    gst_rtsp_server_set_service(server, "8554");
    GstRTSPMountPoints* mounts = gst_rtsp_server_get_mount_points(server);
    GstRTSPMediaFactory* factory = gst_rtsp_media_factory_new();
    // 2026-08-11 h264-dump-debug: tee 到 filesink 抓 H264 annex-B 流到 /tmp/v31_dump.h264
    // (同步改 push loop count 计数 + dump 完成自动关掉)
    // 2026-08-13 fix: SDP framerate 改 14 (跟实际稳态匹配). 30 FPS 误导 PC 端.
// 不在 capsfilter 强制 level (8-13 试过 level=3.0 但 x264 实际 SPS level 跟它不一致, pipeline not-negotiated -4 -> 503).
// 只设 profile (constrained-baseline), 让 x264 自己选合适的 level (跟 SPS 匹配).
    gst_rtsp_media_factory_set_launch(factory,
        "( appsrc name=mysrc caps=\"video/x-raw,format=BGR,width=640,height=480,framerate=14/1\" "
        "! videoconvert "
        "! capsfilter caps=\"video/x-raw,format=I420,width=640,height=480,framerate=14/1\" "
        "! x264enc ! h264parse "
        "! capsfilter caps=\"video/x-h264,framerate=14/1,profile=constrained-baseline\" "
        "! rtph264pay name=pay0 pt=96 config-interval=1 "
        ")");
    gst_rtsp_media_factory_set_shared(factory, TRUE);
    // 2026-08-11: 显式设 RTSP media latency = 200ms (避免 rtpsession "Can't determine running time" 警告)
    gst_rtsp_media_factory_set_latency(factory, 200);
    g_signal_connect(factory, "media-configure", G_CALLBACK(on_media_configure), NULL);
    gst_rtsp_mount_points_add_factory(mounts, "/test", factory);
    gst_rtsp_server_attach(server, NULL);
    g_object_unref(mounts);
    printf("RTSP server: rtsp://0.0.0.0:8554/test\n\n");

    // 4. GMainLoop for GStreamer (在单独 thread 跑, 不阻塞 V4L2 main loop)
    GMainLoop* loop = g_main_loop_new(NULL, FALSE);
    g_thread_new("gst-loop", (GThreadFunc)g_main_loop_run, loop);

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

    // 6. stderr redirect for OD post-process output
    char stderr_file[] = "/tmp/v31_postproc_XXXXXX.txt";
    int stderr_fd = mkstemp(stderr_file);
    close(stderr_fd);

    // 7. Main loop
    int count = 0;
    int total_dets = 0;
    long pushed_count = 0, push_skipped = 0, push_fail = 0;
    GstClockTime pts_ns = 0;
    guint64 frame_duration_ns = GST_SECOND / g_state.FPS;
    struct timespec t_bench_start, last_print;
    clock_gettime(CLOCK_MONOTONIC, &t_bench_start);
    last_print = t_bench_start;

    while (1) {
        struct timespec t_frame0, t_dq, t_cvt, t_od, t_push, t_qb;
        clock_gettime(CLOCK_MONOTONIC, &t_frame0);
        struct v4l2_buffer buf;
        memset(&buf, 0, sizeof(buf));
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;

        struct timespec t0;
        clock_gettime(CLOCK_MONOTONIC, &t0);

        if (ioctl(fd, VIDIOC_DQBUF, &buf) < 0) { perror("DQBUF"); return 1; }
        clock_gettime(CLOCK_MONOTONIC, &t_dq);
        cv::Mat yuyv_mat(H, W, CV_8UC2, bufs[buf.index]);
        cv::Mat bgr;
        cv::cvtColor(yuyv_mat, bgr, cv::COLOR_YUV2BGR_YUYV);

        // OD
        clock_gettime(CLOCK_MONOTONIC, &t_cvt);
        unsigned int in_size = 0;
        fflush(stderr);
        int saved = dup(STDERR_FILENO);
        int tmp_fd = open(stderr_file, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        dup2(tmp_fd, STDERR_FILENO);
        close(tmp_fd);
        // 2026-08-12 优化卡顿: NPU 推理中位数 32ms 是 FPS 卡顿主因。
        // 每 3 帧做一次 OD, 每帧都 push appsrc -> 板端 FPS 14 -> 23+,
        // spike 减少, PC 端 VLC 不再连续卡顿.
        int det_count = 0;
        if ((count % 3) == 0) {
            void* in_buf = yolov5_pre_process_bgr(bgr.data, W, H, &in_size);
            if (in_buf) {
                void* ib = in_buf;
                awnn_set_input_buffers(ctx, &ib);
                awnn_run(ctx);
                void** outputs = awnn_get_output_buffers(ctx);
                if (outputs) {
                    yolov5_post_process_bgr(outputs, W, H);
                    std::vector<std::tuple<int,int,int,int,float,std::string>> dets;
                    det_count = parse_detections_from_stderr(stderr_file, dets);
                }
            }
            fflush(stderr);
            dup2(saved, STDERR_FILENO);
            close(saved);
            if (in_buf) free(in_buf);
        } else {
            fflush(stderr);
            dup2(saved, STDERR_FILENO);
            close(saved);
        }
        clock_gettime(CLOCK_MONOTONIC, &t_od);

        // Push to appsrc: use gst_buffer_new_and_alloc + fill + always unref
        // 2026-08-09: 仅在 appsrc_wants_data 时推 (避免 preroll 503 + queue 爆炸)
        // 2026-08-11 race fix: atomic load appsrc, 用 client_count 替代 client_connected
        // 2026-08-11 fix: 主线程持续 push 会把 appsrc queue 撑满 → enough-data 刷屏 → pipeline 仍 PLAYING 但 buffer 堆积
        // 关键修复: 检查 appsrc 内部 queue level (current-level-buffers) < 2 才 push, 让 pipeline 消费节流同步
        if (g_state.client_count.load() > 0) {
            GstElement* appsrc = g_state.appsrc.load();
            if (appsrc) {
                guint64 level_bytes = 0, level_buffers = 0;
                g_object_get(appsrc,
                    "current-level-bytes", &level_bytes,
                    "current-level-buffers", &level_buffers,
                    NULL);
                if (count < 3 || count % 30 == 0) {
                    fprintf(stderr, "[debug] push: level_buffers=%lu level_bytes=%lu client=%d\n",
                            level_buffers, level_bytes, g_state.client_count.load());
                }
                if (level_buffers < 2) {  // appsrc queue < 2 个 buffer 才推
                    size_t frame_bytes = W * H * 3;
                    GstBuffer* gst_buf = gst_buffer_new_and_alloc(frame_bytes);
                    gst_buffer_fill(gst_buf, 0, bgr.data, frame_bytes);
                    gst_buf->pts = pts_ns;
                    gst_buf->duration = frame_duration_ns;
                    pts_ns += frame_duration_ns;

                    GstFlowReturn ret = gst_app_src_push_buffer(GST_APP_SRC(appsrc), gst_buf);
                    pushed_count++;
                    if (ret != GST_FLOW_OK) {
                        push_fail++;
                    }
                } else {
                    push_skipped++;
                }
            }
        }
        clock_gettime(CLOCK_MONOTONIC, &t_push);

        if (ioctl(fd, VIDIOC_QBUF, &buf) < 0) { perror("QBUF"); return 1; }
        clock_gettime(CLOCK_MONOTONIC, &t_qb);

        count++;
        total_dets += det_count;

        struct timespec t_now;
        clock_gettime(CLOCK_MONOTONIC, &t_now);
        double elapsed = timespec_diff_ms(t_now, t_bench_start) / 1000.0;
        double since_print = timespec_diff_ms(t_now, last_print) / 1000.0;
        if (since_print > 1.0) {
            double fps = count / elapsed;
            // 2026-08-10: per-frame timing breakdown to diagnose 14 vs 25 FPS
            double dq_ms = (t_dq.tv_sec - t_frame0.tv_sec) * 1000.0 + (t_dq.tv_nsec - t_frame0.tv_nsec) / 1e6;
            double cvt_ms = (t_cvt.tv_sec - t_dq.tv_sec) * 1000.0 + (t_cvt.tv_nsec - t_dq.tv_nsec) / 1e6;
            double od_ms = (t_od.tv_sec - t_cvt.tv_sec) * 1000.0 + (t_od.tv_nsec - t_cvt.tv_nsec) / 1e6;
            double push_ms = (t_push.tv_sec - t_od.tv_sec) * 1000.0 + (t_push.tv_nsec - t_od.tv_nsec) / 1e6;
            double qb_ms = (t_qb.tv_sec - t_push.tv_sec) * 1000.0 + (t_qb.tv_nsec - t_push.tv_nsec) / 1e6;
            printf("  count=%4d fps=%5.2f elapsed=%.1fs | dq=%.1f cvt=%.1f od=%.1f push=%.1f qb=%.1f ms | dets=%d client=%d\n",
                   count, fps, elapsed, dq_ms, cvt_ms, od_ms, push_ms, qb_ms, total_dets,
                   g_state.client_count.load());
            last_print = t_now;
        }
        if (elapsed > (double)DURATION_S * 1000.0) break;
    }

    // Cleanup
    ioctl(fd, VIDIOC_STREAMOFF, &type);
    for (int i = 0; i < N_BUFS; i++) munmap(bufs[i], buf_lens[i]);
    close(fd);
    g_main_loop_quit(loop);
    unlink(stderr_file);
    dlclose(h);

    printf("\n=== v31 done ===\n");
    return 0;
}
