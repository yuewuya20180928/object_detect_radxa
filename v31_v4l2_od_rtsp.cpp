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
#include <opencv2/imgcodecs.hpp>  // 2026-08-13 debug: cv::imencode for /tmp/v31_last.jpg
#include <thread>
#include <mutex>
#include <condition_variable>
#include <deque>
#include <memory>
#include <utility>
#include <cmath>  // 2026-08-14: expf for yolov5 decode
#include <algorithm>  // 2026-08-14: max_element for yolov5 decode + NMS
#include <vector>  // 2026-08-14: detection boxes

// 2026-08-14: yolov5 decode constants
// 棋盘阵 yolov5s anchors (P3/P4/P5 stride 8/16/32)
static const int YOLO_GRID_SIZES[3] = {80, 40, 20};
static const int YOLO_STRIDES[3] = {8, 16, 32};
static const int YOLO_NUM_ANCHORS = 3;
// COCO 80 个 anchors (P3/P4/P5)
static const float YOLO_ANCHORS[3][6] = {
    {10.0f, 13.0f, 16.0f, 30.0f, 33.0f, 23.0f},      // stride 8: 80x80 grid
    {30.0f, 61.0f, 62.0f, 45.0f, 59.0f, 119.0f},     // stride 16: 40x40 grid
    {116.0f, 90.0f, 156.0f, 198.0f, 373.0f, 326.0f}  // stride 32: 20x20 grid
};
static const int YOLO_NUM_CLASSES = 80;
static const char* YOLO_CLASSES[80] = {
    "person", "bicycle", "car", "motorbike", "aeroplane", "bus", "train", "truck", "boat", "traffic light",
    "fire hydrant", "stop sign", "parking meter", "bench", "bird", "cat", "dog", "horse", "sheep", "cow",
    "elephant", "bear", "zebra", "giraffe", "backpack", "umbrella", "handbag", "tie", "suitcase", "frisbee",
    "skis", "snowboard", "sports ball", "kite", "baseball bat", "baseball glove", "skateboard", "surfboard", "tennis racket", "bottle",
    "wine glass", "cup", "fork", "knife", "spoon", "bowl", "banana", "apple", "sandwich", "orange",
    "broccoli", "carrot", "hot dog", "pizza", "donut", "cake", "chair", "sofa", "pottedplant", "bed",
    "diningtable", "toilet", "tvmonitor", "laptop", "mouse", "remote", "keyboard", "cell phone", "microwave", "oven",
    "toaster", "sink", "refrigerator", "book", "clock", "vase", "scissors", "teddy bear", "hair drier", "toothbrush"
};
// 2026-08-14: conf/iou 阈值 改全局变量, 从 main() argv 传入
static float YOLO_CONF_THRESH = 0.55f;  // 2026-08-14 00:30: 调高过滤假噪声
static float YOLO_IOU_THRESH = 0.60f;
static int OD_SKIP = 1;  // 每 N 帧做一次 OD, 默认 1 (每帧)
static const int YOLO_INPUT_W = 640;
static const int YOLO_INPUT_H = 640;

// 2026-09-06 23:25 RCA: 易混淆类 top-K logit dump (验证水杯→cup/bottle/remote/vase 跨帧跳变根因)
// 用 printf(stdout) → journald (不能用 fprintf(stderr),会被 dup2 + TRUNC)
static const int TOPK_TARGET_CLASSES[] = {41, 39, 65, 75, 60};
static const char* TOPK_TARGET_NAMES[] = {"cup", "bottle", "remote", "vase", "table"};
static const int TOPK_TARGET_NUM = sizeof(TOPK_TARGET_CLASSES) / sizeof(TOPK_TARGET_CLASSES[0]);

// 2026-09-06 23:48 RCA 修复: 圆柱形类 (cylindrical) 特殊处理
//   NPU INT8 obj head 量化损失 (sig max 0.46) 导致 score=obj*cls_prob 卡阈值边界
//   cylindrical 类 (cup/bottle/vase/wine glass) 改用 score=cls_prob + 阈 0.40
//   person + 其他 73 类 完全不动, 走原 obj*cls_prob + 阈 0.20 路径
static const int CYL_CLASSES[] = {39, 40, 41, 75};  // bottle, wine glass, cup, vase
static const int CYL_NUM = sizeof(CYL_CLASSES) / sizeof(CYL_CLASSES[0]);
static const float CYL_CONF_THRESH = 0.40f;  // cylindrical 类单独阈值 (cls_prob 维度)

struct DetBox {
    float x1, y1, x2, y2;
    float score;
    int class_id;
};

static float iou_xyxy(const DetBox& a, const DetBox& b) {
    float xx1 = std::max(a.x1, b.x1);
    float yy1 = std::max(a.y1, b.y1);
    float xx2 = std::min(a.x2, b.x2);
    float yy2 = std::min(a.y2, b.y2);
    float w = std::max(0.0f, xx2 - xx1);
    float h = std::max(0.0f, yy2 - yy1);
    float inter = w * h;
    float area_a = (a.x2 - a.x1) * (a.y2 - a.y1);
    float area_b = (b.x2 - b.x1) * (b.y2 - b.y1);
    return inter / (area_a + area_b - inter + 1e-6f);
}

// 2026-08-14: 从 NPU 3 个 output tensor 解码出所有 detection (640x640 letterbox 坐标)
// NPU output 布局 (NHWC): [grid_y, grid_x, anchor_idx, 85]
// 85 = [x_logit, y_logit, w_logit, h_logit, obj_logit, cls0_logit, ..., cls79_logit]
static void yolov5_decode_all(void** outputs, int orig_w, int orig_h, std::vector<DetBox>& out_dets) {
    out_dets.clear();
    for (int s = 0; s < 3; s++) {
        int gs = YOLO_GRID_SIZES[s];
        int stride = YOLO_STRIDES[s];
        float* out = (float*)outputs[s];
        for (int gy = 0; gy < gs; gy++) {
            for (int gx = 0; gx < gs; gx++) {
                for (int a = 0; a < YOLO_NUM_ANCHORS; a++) {
                    int idx = (a * gs * gs + gy * gs + gx) * 85;
                    float* anch = out + idx;
                    // obj sigmoid
                    float obj = 1.0f / (1.0f + expf(-anch[4]));
                    if (obj < YOLO_CONF_THRESH) continue;
                    // 找最大 class prob
                    int best_cls = -1;
                    float best_cls_logit = -1e9f;
                    for (int c = 0; c < YOLO_NUM_CLASSES; c++) {
                        float v = anch[5 + c];
                        if (v > best_cls_logit) { best_cls_logit = v; best_cls = c; }
                    }
                    float cls_prob = 1.0f / (1.0f + expf(-best_cls_logit));
                    // 2026-09-06 23:48 RCA 修复: cylindrical 类 (cup/bottle/vase/wine glass) 用 cls_prob 路径
                    //   NPU INT8 obj head 量化后 sig 最大 0.46, 跟 cls_prob 相乘后 score 卡阈值
                    //   cylindrical 类 cls_prob 强 (cup sig 0.72-0.91 跨帧稳定), 扔掉弱 obj
                    //   person + 其他类完全不动, 仍走 obj*cls_prob + 0.20 阈值
                    bool is_cyl = false;
                    for (int cc = 0; cc < CYL_NUM; cc++) {
                        if (best_cls == CYL_CLASSES[cc]) { is_cyl = true; break; }
                    }
                    float score;
                    float cls_thresh;
                    if (is_cyl) {
                        score = cls_prob;
                        cls_thresh = CYL_CONF_THRESH;  // 0.40
                    } else {
                        score = obj * cls_prob;
                        cls_thresh = YOLO_CONF_THRESH;  // 0.20
                    }
                    if (score < cls_thresh) continue;
                    // yolov5 decode box
                    float x_logit = anch[0];
                    float y_logit = anch[1];
                    float w_logit = anch[2];
                    float h_logit = anch[3];
                    float xc = (1.0f/(1.0f+expf(-x_logit))*2.0f - 0.5f + gx) * stride;
                    float yc = (1.0f/(1.0f+expf(-y_logit))*2.0f - 0.5f + gy) * stride;
                    float w = std::pow(1.0f/(1.0f+expf(-w_logit))*2.0f, 2) * YOLO_ANCHORS[s][a*2];
                    float h = std::pow(1.0f/(1.0f+expf(-h_logit))*2.0f, 2) * YOLO_ANCHORS[s][a*2+1];
                    DetBox box;
                    box.x1 = xc - w/2; box.y1 = yc - h/2;
                    box.x2 = xc + w/2; box.y2 = yc + h/2;
                    box.score = score; box.class_id = best_cls;
                    out_dets.push_back(box);
                }
            }
        }
    }
}

// 2026-08-14: NMS (按 class 分组)
// 输入 out_dets (640x640 letterbox 坐标), 输出 nms_dets
static void yolov5_nms(std::vector<DetBox>& dets, std::vector<DetBox>& nms_dets) {
    nms_dets.clear();
    std::sort(dets.begin(), dets.end(), [](const DetBox& a, const DetBox& b) { return a.score > b.score; });
    for (auto& d : dets) {
        bool keep = true;
        for (auto& k : nms_dets) {
            if (d.class_id == k.class_id && iou_xyxy(d, k) > YOLO_IOU_THRESH) { keep = false; break; }
        }
        if (keep) nms_dets.push_back(d);
    }
}

// 2026-09-06 23:48 RCA 修复: cylindrical 类间 NMS (cup/bottle/vase/wine glass)
//   同位置 IoU > 0.5 的 cylindrical 多类 → 只留 score 最高的, 其余标 score=-1 杀掉
//   person + 其他类完全不受影响 (只处理 cylindrical 类)
//   解决: cup/bottle/vase/remote 同 anchor 重复检测 / 嵌套框问题
static void cyl_iou_nms(std::vector<DetBox>& dets) {
    for (size_t i = 0; i < dets.size(); i++) {
        bool i_is_cyl = false;
        for (int cc = 0; cc < CYL_NUM; cc++) {
            if (dets[i].class_id == CYL_CLASSES[cc]) { i_is_cyl = true; break; }
        }
        if (!i_is_cyl) continue;
        for (size_t j = 0; j < dets.size(); j++) {
            if (i == j) continue;
            bool j_is_cyl = false;
            for (int cc = 0; cc < CYL_NUM; cc++) {
                if (dets[j].class_id == CYL_CLASSES[cc]) { j_is_cyl = true; break; }
            }
            if (!j_is_cyl) continue;
            float iou = iou_xyxy(dets[i], dets[j]);
            if (iou > 0.5f && dets[i].score < dets[j].score) {
                dets[i].score = -1.0f;  // 标记拒绝
                break;
            }
        }
    }
}

// 2026-08-14: letterbox inverse - 从 640x640 还原到原始 640x480 BGR
// yolov5 letterbox: scale = min(model_w/img_w, model_h/img_h) = min(640/640, 640/480) = 1.0
// new_size = (640, 480), pad = (0, 80) (40 top + 40 bottom)
static void letterbox_inverse(int orig_w, int orig_h, DetBox& box) {
    float scale = std::min((float)YOLO_INPUT_W/orig_w, (float)YOLO_INPUT_H/orig_h);
    int new_w = (int)(orig_w * scale);
    int new_h = (int)(orig_h * scale);
    int pad_x = (YOLO_INPUT_W - new_w) / 2;
    int pad_y = (YOLO_INPUT_H - new_h) / 2;
    box.x1 = (box.x1 - pad_x) / scale;
    box.y1 = (box.y1 - pad_y) / scale;
    box.x2 = (box.x2 - pad_x) / scale;
    box.y2 = (box.y2 - pad_y) / scale;
    // clip
    box.x1 = std::max(0.0f, std::min((float)orig_w, box.x1));
    box.y1 = std::max(0.0f, std::min((float)orig_h, box.y1));
    box.x2 = std::max(0.0f, std::min((float)orig_w, box.x2));
    box.y2 = std::max(0.0f, std::min((float)orig_h, box.y2));
}

// 2026-08-14: 在 BGR 帧上画检测框 + 标签
static void draw_detections(cv::Mat& img, const std::vector<DetBox>& dets) {
    for (auto& d : dets) {
        cv::rectangle(img, cv::Point((int)d.x1, (int)d.y1), cv::Point((int)d.x2, (int)d.y2),
                      cv::Scalar(0, 255, 0), 2);
        char label[128];
        snprintf(label, sizeof(label), "%s %.0f%%", YOLO_CLASSES[d.class_id], d.score * 100);
        int baseline = 0;
        cv::Size text_size = cv::getTextSize(label, cv::FONT_HERSHEY_SIMPLEX, 0.5, 1, &baseline);
        // 背景黑条
        cv::rectangle(img,
                      cv::Point((int)d.x1, (int)d.y1 - text_size.height - 6),
                      cv::Point((int)d.x1 + text_size.width, (int)d.y1),
                      cv::Scalar(0, 0, 0), -1);
        cv::putText(img, label, cv::Point((int)d.x1, (int)d.y1 - 4),
                    cv::FONT_HERSHEY_SIMPLEX, 0.5, cv::Scalar(0, 255, 0), 1);
    }
}

#include <gst/gst.h>
#include <gst/app/gstappsrc.h>
#include <gst/video/video.h>
#include <gst/rtsp-server/rtsp-server.h>

// libod_helper.so C API


// =========================================================================
// 2026-10-11 优化 2: BgrRing zero-copy shared BGR buffer (替代 bgr.clone())
//   main 拿 free slot 写 cvtColor, push frame (只传 idx)
//   worker 算 OD 完 release slot, main 才能 reuse
//   ring size = 3 (main 1, OD 1, free 1, 不阻塞)
// =========================================================================
// libod_helper.so C API
typedef void* (*t_awnn_create)(const char*);
typedef void (*t_awnn_init)(void);
typedef void* (*t_yolov5_pre_process_bgr)(const unsigned char*, int, int, unsigned int*);
typedef int (*t_yolov5_post_process_bgr)(void**, int, int);
typedef void (*t_awnn_set_input_buffers)(void*, void**);
typedef void (*t_awnn_run)(void*);
typedef void** (*t_awnn_get_output_buffers)(void*);

// =========================================================================
// 2026-10-11 架构 A MVP: OD 独立 worker thread
// =========================================================================

struct FrameTask {
    std::shared_ptr<cv::Mat> bgr;     // bgr clone (避免 OD 算时 main 改写)
    uint64_t frame_id = 0;
    struct timespec capture_ts{};
};

struct DetResult {
    std::vector<DetBox> dets;
    uint64_t frame_id = 0;
};

// 通用 thread-safe 队列, 支持 drop_oldest / wait_pop / try_pop_latest
template <typename T>
class FrameQueueT {
    std::mutex m_;
    std::condition_variable cv_not_empty_;
    std::deque<T> q_;
    size_t max_size_;
    std::atomic<bool> stop_{false};
public:
    FrameQueueT(size_t max_size=2) : max_size_(max_size) {}
    void stop() {
        std::lock_guard<std::mutex> lk(m_);
        stop_.store(true);
        cv_not_empty_.notify_all();
    }
    void push_drop_oldest(T t) {
        std::lock_guard<std::mutex> lk(m_);
        while (q_.size() >= max_size_) q_.pop_front();
        q_.push_back(std::move(t));
        cv_not_empty_.notify_one();
    }
    T wait_pop() {
        std::unique_lock<std::mutex> lk(m_);
        cv_not_empty_.wait(lk, [this]{return !q_.empty() || stop_.load();});
        if (stop_.load() && q_.empty()) return T{};
        T t = std::move(q_.front()); q_.pop_front();
        return t;
    }
    bool try_pop_latest(T& out) {
        std::lock_guard<std::mutex> lk(m_);
        if (q_.empty()) return false;
        out = std::move(q_.back());
        q_.clear();
        return true;
    }
    size_t size() { std::lock_guard<std::mutex> lk(m_); return q_.size(); }
};

// OD 独立 worker thread, 跑 pre + awnn_run + decode + nms
class DetectorWorker {
    FrameQueueT<FrameTask> q_frame_;
    FrameQueueT<DetResult> q_det_result_;
    std::thread thread_;
    std::atomic<bool> running_{false};
    void* ctx_;
    int W_, H_;
    const char* stderr_file_;
    t_yolov5_pre_process_bgr pre_fn_;
    t_yolov5_post_process_bgr post_fn_;
    t_awnn_set_input_buffers setin_fn_;
    t_awnn_run run_fn_;
    t_awnn_get_output_buffers getout_fn_;
public:
    DetectorWorker(void* ctx, int W, int H, const char* stderr_file,
                   t_yolov5_pre_process_bgr pre_fn,
                   t_yolov5_post_process_bgr post_fn,
                   t_awnn_set_input_buffers setin_fn,
                   t_awnn_run run_fn,
                   t_awnn_get_output_buffers getout_fn,
                   size_t q_frame_max=2)
        : q_frame_(q_frame_max), q_det_result_(1),
          ctx_(ctx), W_(W), H_(H), stderr_file_(stderr_file),
          pre_fn_(pre_fn), post_fn_(post_fn), setin_fn_(setin_fn),
          run_fn_(run_fn), getout_fn_(getout_fn) {}
    void start() {
        running_.store(true);
        thread_ = std::thread([this]{ run_loop(); });
    }
    void stop() {
        running_.store(false);
        q_frame_.stop();
        if (thread_.joinable()) thread_.join();
    }
    auto& q_frame() { return q_frame_; }
    auto& q_det_result() { return q_det_result_; }

    void run_loop() {
        while (running_.load()) {
            FrameTask task = q_frame_.wait_pop();
            if (!running_.load()) break;
            if (!task.bgr || task.bgr->empty()) continue;
            std::vector<DetBox> nms_dets;
            fflush(stderr);
            int saved = dup(STDERR_FILENO);
            int tmp_fd = open(stderr_file_, O_WRONLY | O_CREAT | O_TRUNC, 0644);
            dup2(tmp_fd, STDERR_FILENO);
            close(tmp_fd);
            unsigned int in_size = 0;
            void* in_buf = pre_fn_(task.bgr->data, W_, H_, &in_size);
            if (in_buf) {
                void* ib = in_buf;
                setin_fn_(ctx_, &ib);
                run_fn_(ctx_);
                void** outputs = getout_fn_(ctx_);
                if (outputs) {
                    post_fn_(outputs, W_, H_);
                    fflush(stderr);
                    std::vector<DetBox> all_dets;
                    yolov5_decode_all(outputs, W_, H_, all_dets);
                    yolov5_nms(all_dets, nms_dets);
                    cyl_iou_nms(nms_dets);
                    nms_dets.erase(std::remove_if(nms_dets.begin(), nms_dets.end(),
                        [](const DetBox& d) { return d.score < 0.0f; }), nms_dets.end());
                    for (auto& d : nms_dets) letterbox_inverse(W_, H_, d);
                }
            }
            fflush(stderr);
            dup2(saved, STDERR_FILENO);
            close(saved);
            if (in_buf) free(in_buf);
            DetResult res;
            res.dets = std::move(nms_dets);
            res.frame_id = task.frame_id;
            q_det_result_.push_drop_oldest(std::move(res));
        }
    }
};



static double timespec_diff_ms(const struct timespec& a, const struct timespec& b) {
    return (a.tv_sec - b.tv_sec) * 1000.0 + (a.tv_nsec - b.tv_nsec) / 1e6;
}

// 2026-08-13 fix: libod_helper.so 的 yolov5_post_process_bgr 实际输出格式
// 实测 (strings libod_helper.so | grep): 只有 "detection num: %d" 一行
// 之前注释写的 "0: 44%, [x1, y1, x2, y2], class_name" 是错的旧版本格式
// 这版只能拿 count, 不能拿 bbox/class (需要改 libod_helper.so 才能拿)
// 返回值: >= 0 = detection num; < 0 = parse 失败 (按 0 处理)
static int parse_detections_from_stderr(const char* stderr_file,
    std::vector<std::tuple<int,int,int,int,float,std::string>>& dets) {
    FILE* f = fopen(stderr_file, "r");
    if (!f) return 0;
    char line[512];
    int count = 0;
    while (fgets(line, sizeof(line), f)) {
        int n = -1;
        if (sscanf(line, "detection num: %d", &n) == 1 && n > 0) {
            count = n;  // 用 SDK 给的 count (已经过内置 NMS+threshold)
            // 注意: 当前 libod_helper.so 不输出 bbox/class, 这里只能拿到 count
            // 如果需要画框, 需要改 libod_helper.so 或自己写 post-process
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
    // 2026-08-14: argv 7-9 是 conf/iou/od_skip (可调参数, 运行时设)
    if (argc > 7) YOLO_CONF_THRESH = atof(argv[7]);
    if (argc > 8) YOLO_IOU_THRESH = atof(argv[8]);
    if (argc > 9) OD_SKIP = atoi(argv[9]);  // 每 N 帧做一次 OD, 1=每帧, 3=每 3 帧

    printf("=== v31 ALL-C++ V4L2 + OD + RTSP (debug version) ===\n");
    printf("  dev=%s  res=%dx%d  n_bufs=%d  duration=%ds\n", dev, W, H, N_BUFS, DURATION_S);
    printf("  model=%s\n", model_path);
    printf("  yolo: conf=%.2f iou=%.2f od_skip=%d\n\n", YOLO_CONF_THRESH, YOLO_IOU_THRESH, OD_SKIP);
    // 2026-08-13 fix: SDP framerate 跟实际板端稳态输出匹配 (实际 ~14 FPS cumulative, 稳态 10 FPS).
    // 之前 30 FPS 误导 PC 端 mpv/VLC 按 30 FPS 设计 buffer, 实际 14 FPS 时 buffer 永远吃不满, 任何 spike 都直接体现在画面上.
    g_state.W = W; g_state.H = H; g_state.FPS = 30;  // 2026-10-11: 架构 A 上线后 camera ceiling 30fps, SDP/caps 跟实际匹配

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
        "( appsrc name=mysrc caps=\"video/x-raw,format=BGR,width=640,height=480,framerate=30/1\" "
        "! videoconvert "
        "! capsfilter caps=\"video/x-raw,format=I420,width=640,height=480,framerate=30/1\" "
        "! x264enc ! h264parse "
        "! capsfilter caps=\"video/x-h264,framerate=30/1,profile=constrained-baseline\" "
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

    // === 架构 A: 启动 OD worker (clone bgr ~0.5ms, 接受小开销换 main 不等 OD) ===
    DetectorWorker worker(ctx, W, H, stderr_file,
                          yolov5_pre_process_bgr, yolov5_post_process_bgr,
                          awnn_set_input_buffers, awnn_run, awnn_get_output_buffers,
                          /*q_frame_max=*/2);
    worker.start();
    DetResult last_det;
    uint64_t frame_id = 0;

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
        // 2026-08-13 debug: 每 30 帧 (~2s) 保存 yuv 原始

        // === 架构 A: 异步 OD (clone bgr 0.5ms, 保证 main 不等 OD) ===
        clock_gettime(CLOCK_MONOTONIC, &t_cvt);
        cv::cvtColor(yuyv_mat, bgr, cv::COLOR_YUV2BGR_YUYV);
        // 1. clone bgr 推给 worker (避免 main 改写)
        FrameTask task;
        task.bgr = std::make_shared<cv::Mat>(bgr.clone());
        task.frame_id = frame_id++;
        worker.q_frame().push_drop_oldest(std::move(task));
        // 2. pop latest det from worker (non-blocking)
        DetResult new_det;
        if (worker.q_det_result().try_pop_latest(new_det)) {
            last_det = std::move(new_det);
        }
        // raw.jpg dump (改框前)
        if ((count % 30) == 0) {
            std::vector<uchar> jpeg_buf;
            cv::imencode(".jpg", bgr, jpeg_buf);
            FILE* fj = fopen("/tmp/v31_raw.jpg", "wb");
            if (fj) { fwrite(jpeg_buf.data(), 1, jpeg_buf.size(), fj); fclose(fj); }
        }
        // 3. draw last_det on current bgr (1 frame latency ~33ms)
        draw_detections(bgr, last_det.dets);
        // /tmp/v31_last.jpg dump 用 draw 完的 bgr (改框后)
        if ((count % 30) == 0) {
            std::vector<uchar> jpeg_buf;
            cv::imencode(".jpg", bgr, jpeg_buf);
            FILE* fj = fopen("/tmp/v31_last.jpg", "wb");
            if (fj) { fwrite(jpeg_buf.data(), 1, jpeg_buf.size(), fj); fclose(fj); }
        }
        clock_gettime(CLOCK_MONOTONIC, &t_od);
        // 2026-08-12 优化卡顿: NPU 推理中位数 32ms 是 FPS 卡顿主因。
        // 每 3 帧做一次 OD, 每帧都 push appsrc -> 板端 FPS 14 -> 23+,
        // spike 减少, PC 端 VLC 不再连续卡顿.
        //
        // 2026-08-13 fix: 之前用 `if ((count % 3) == 0)` 在 G++ -O2 下被优化掉
        // (modulo by constant 被常量化, if 总成立 → 每帧都跑 OD, od_ms 不降)
        // 实测 od=21ms 每帧 (期望 1/3 帧 = 7ms avg) -> bug 复现
        // 尝试 1: std::atomic<int> counter + fetch_add
        //   -> 代码生成对 (__aarch64_ldadd4 + 0x55555556 mod 3)
        //   -> 但运行时 od=21ms 每帧
        // 尝试 2: volatile int + 普通 +1
        //   -> volatile 让 fetch_add 不能被优化
        //   -> 普通 int 自增在 main thread 单线程下, 比 std::atomic 简单可靠
        //   -> 但运行时仍然 od=21ms 每帧 (debug print 在 postproc 里, 只看到 OD 帧)


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
        // total_dets += det_count; // 2026-10-11 架构 A: main 不再累计 det

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
    worker.stop();  // 2026-10-11 架构 A: 等 worker 算完最后一帧, 避免 race
    ioctl(fd, VIDIOC_STREAMOFF, &type);
    for (int i = 0; i < N_BUFS; i++) munmap(bufs[i], buf_lens[i]);
    close(fd);
    g_main_loop_quit(loop);
    unlink(stderr_file);
    dlclose(h);

    printf("\n=== v31 done ===\n");
    return 0;
}
