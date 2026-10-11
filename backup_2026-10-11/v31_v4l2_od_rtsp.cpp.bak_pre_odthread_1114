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
// 2026-08-17: yolov8 decode constants (anchor-free)
// output0 单 tensor shape (1, 6, 8400), 内存 NCHW
//   channel 0..3: cx, cy, w, h (已含 grid*stride, 640x640 letterbox 坐标, float)
//   channel 4: cls0 score (sigmoid'd, person) [0, 1]
//   channel 5: cls1 score (sigmoid'd, not_person) [0, 1]
// 8400 = 80*80 + 40*40 + 20*20 (anchor-free, 1 anchor per grid cell)
static const int YOLOV8_NUM_CLASSES = 2;
static const char* YOLOV8_CLASSES[2] = {"person", "not_person"};
static const int YOLOV8_TOTAL_ANCHORS = 8400;
// 2026-08-14: conf/iou 阈值 改全局变量, 从 main() argv 传入
static float YOLO_CONF_THRESH = 0.55f;  // 2026-08-14 00:30: 调高过滤假噪声
static float YOLO_IOU_THRESH = 0.55f;  // 2026-08-14 v36.2: 改回 0.55 (IoU 0.30/0.45 实验导致漏检)
// 2026-08-14 22:50 v35 误识别过滤: per-class conf + edge/area filter
// 老板客厅场景实测: 真人 47% 在中心, 但衣架/衣服/杂物 41-52% 在边缘
// 单 conf 阈值没法区分 → 加 3 层过滤
// 2026-08-14 23:05 v35.1: 调回宽松 (v35 漏检太狠, 老板姿势微变就只剩 1 个 42% 袖子)
//   - person conf 0.40 → 0.35 (保弱 anchor)
//   - edge 5% → 3% (放宽边缘)
//   - area max 70% → 80% (保大框)
//   - 新增 top-3 box 限制 (不管 conf, 每帧最多画 3 个最强)
// 2026-08-14 23:50 v36: B+D 迭代
//   B. class 黑名单 - ban 室内常误识别类 (伞/包/领带/椅子/沙发/盆栽/餐桌/马桶/书)
//   D. 同 class NMS IoU 0.55 → 0.30 - 让 face+collar 重叠的 person 合并成 1 个
// 2026-08-15 v37: 老板反馈 v35.1 还有多余 person 框 (雨伞+墙 46%, 金属碗 40%)
//   - person conf 0.35 → 0.45 (杀 40-46% 误识别, 保留真人 42%+)
//   - top-K 3 → 1 (只画 1 个最强, 真人 conf 几乎总是最高, 不怕漏)
// 2026-08-15 v38: 加 5 帧 temporal voting
//   - 真人每帧都在 (同一个位置) → hit_count 累积, 通过
//   - 假阳性随机位置 → 1-2 帧 hit, 被过滤
//   - hit_count >= 3/5 才画
#define TEMPORAL_WINDOW 5      // 看最近 5 帧
#define TEMPORAL_VOTES 3       // 至少 3 帧命中才算稳定
#define TEMPORAL_MAX_DIST 80.0f  // 同框距离阈值 (中心 < 80px 算同一框)
#define TEMPORAL_TRACKS 2      // 同时跟踪 2 条轨迹 (真人 + 偶尔另一稳定目标)
static float PERSON_CONF_THRESH = 0.45f;  // person 类敏感, 保留真人 (含 45%+ 弱 anchor)
static float OTHER_CONF_THRESH = 0.65f;   // 其他类严格, 杀 cell phone/椅子误识别
static float MIN_BOX_AREA_RATIO = 0.008f; // < 0.8% 噪点 reject
static float MAX_BOX_AREA_RATIO = 0.80f;  // > 80% 全框 reject (墙/窗帘)
static float EDGE_MARGIN_RATIO = 0.03f;   // 3% 边缘内 reject
static int MAX_BOX_PER_FRAME = 1;         // 每帧最多画 1 个 (top-1 by conf)
// 2026-08-14 v36.2: 黑名单简化, 只 ban 56=chair (最常误识别), 其他全保留
// v36 10 个 ban class 实验失败 (0 框), 推测 NPU 内部算法 + 边界条件耦合导致 person anchor 被误杀
static const int BLACKLIST_CLASSES[] = {56};
static const int BLACKLIST_NUM = sizeof(BLACKLIST_CLASSES) / sizeof(BLACKLIST_CLASSES[0]);
static int OD_SKIP = 1;  // 每 N 帧做一次 OD, 默认 1 (每帧)
static const int YOLO_INPUT_W = 640;
static const int YOLO_INPUT_H = 640;

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
// 2026-08-14 22:50 v35: 加 per-class conf + edge/area filter
static void yolov5_decode_all(void** outputs, int orig_w, int orig_h, std::vector<DetBox>& out_dets) {
    out_dets.clear();
    for (int s = 0; s < 3; s++) {
        int gs = YOLO_GRID_SIZES[s];
        int stride = YOLO_STRIDES[s];
        float* out = (float*)outputs[s];
        for (int gy = 0; gy < gs; gy++) {
            for (int gx = 0; gx < gs; gx++) {
                for (int a = 0; a < YOLO_NUM_ANCHORS; a++) {
                    int idx = (gy * gs + gx) * YOLO_NUM_ANCHORS * 85 + a * 85;
                    float* anch = out + idx;
                    // obj sigmoid
                    float obj = 1.0f / (1.0f + expf(-anch[4]));
                    // 2026-08-14 v35: per-class conf 阈值 (person 敏感, 其他严格)
                    float obj_thresh = (anch[5+0] > anch[5+2]) ? PERSON_CONF_THRESH : OTHER_CONF_THRESH;
                    if (obj < obj_thresh) continue;
                    // 找最大 class prob
                    int best_cls = -1;
                    float best_cls_logit = -1e9f;
                    for (int c = 0; c < YOLO_NUM_CLASSES; c++) {
                        float v = anch[5 + c];
                        if (v > best_cls_logit) { best_cls_logit = v; best_cls = c; }
                    }
                    float cls_prob = 1.0f / (1.0f + expf(-best_cls_logit));
                    float score = obj * cls_prob;
                    // 2026-08-14 v36: blacklisted class 直接 skip (不浪费 NMS 资源)
                    bool blacklisted = false;
                    for (int b = 0; b < BLACKLIST_NUM; b++) {
                        if (best_cls == BLACKLIST_CLASSES[b]) { blacklisted = true; break; }
                    }
                    if (blacklisted) continue;
                    // 2026-08-14 v35: 实际 class 决定 conf 阈值
                    float cls_thresh = (best_cls == 0) ? PERSON_CONF_THRESH : OTHER_CONF_THRESH;
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
                    // 2026-08-14 v35: 面积过滤 (杀全框/噪点)
                    float box_w = box.x2 - box.x1;
                    float box_h = box.y2 - box.y1;
                    float area_ratio = (box_w * box_h) / (orig_w * orig_h);
                    if (area_ratio < MIN_BOX_AREA_RATIO || area_ratio > MAX_BOX_AREA_RATIO) {
                        continue;  // 太小的噪点 或 太大的全框
                    }
                    out_dets.push_back(box);
                }
            }
        }
    }
}

// 2026-08-14: NMS (按 class 分组)
// 输入 out_dets (640x640 letterbox 坐标), 输出 nms_dets
// 2026-08-14 v36: 默认 IoU 阈值 0.55→0.30, 让 face+collar 重叠 person 合并
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

// 2026-08-17: yolov8 decode - 单 output0 tensor
// 2026-08-17 修正: NPU 输出 NHWC (1, 8400, 6) 不是 NCHW (1, 6, 8400)
//   NPU  dim 8400 6 1 => 内存顺序 out[i*6 + 0..5]
//     out[i*6 + 0] = anchor i 的 cx (pixel, 640x640 letterbox)
//     out[i*6 + 1] = anchor i 的 cy
//     out[i*6 + 2] = anchor i 的 w
//     out[i*6 + 3] = anchor i 的 h
//     out[i*6 + 4] = anchor i 的 cls0 score (person, sigmoid'd)
//     out[i*6 + 5] = anchor i 的 cls1 score (not_person, sigmoid'd)
// 8400 = 80*80 + 40*40 + 20*20 (anchor-free, 无 anchor box)
// PC 端 onnx 输出是 NCHW, 但 pegasus/od 部署后重排为 NHWC → NPU 输出为 NHWC
// 复用现有 per-class conf + area filter + NMS + letterbox_inverse + draw
static void yolov8_decode_all(void** outputs, int orig_w, int orig_h, std::vector<DetBox>& out_dets) {
    out_dets.clear();
    float* out = (float*)outputs[0];
    for (int i = 0; i < YOLOV8_TOTAL_ANCHORS; i++) {
        // NHWC layout: 6 个值连续存放
        float cx = out[i * 6 + 0];
        float cy = out[i * 6 + 1];
        float w  = out[i * 6 + 2];
        float h  = out[i * 6 + 3];
        float logit_cls0 = out[i * 6 + 4];  // person raw logit
        float logit_cls1 = out[i * 6 + 5];  // not_person raw logit
        // 2026-08-17: NPU 输出是 INT8 反量化后的 raw logits (不是 sigmoid'd probability)
        // logit 范围 -inf ~ +inf, sigmoid 后才是 0~1
        // sigmoid(x) = 1 / (1 + exp(-x))
        float score_cls0 = 1.0f / (1.0f + expf(-logit_cls0));
        float score_cls1 = 1.0f / (1.0f + expf(-logit_cls1));
        // 选最大 class
        int best_cls;
        float score;
        if (score_cls0 >= score_cls1) {
            best_cls = 0;
            score = score_cls0;
        } else {
            best_cls = 1;
            score = score_cls1;
        }
        // per-class conf 阈值 (复用全局变量)
        float thresh = (best_cls == 0) ? PERSON_CONF_THRESH : OTHER_CONF_THRESH;
        if (score < thresh) continue;
        // area filter (噪点 + 全框)
        float area_ratio = (w * h) / (float)(orig_w * orig_h);
        if (area_ratio < MIN_BOX_AREA_RATIO || area_ratio > MAX_BOX_AREA_RATIO) continue;
        DetBox box;
        box.x1 = cx - w * 0.5f;
        box.y1 = cy - h * 0.5f;
        box.x2 = cx + w * 0.5f;
        box.y2 = cy + h * 0.5f;
        box.score = score;
        box.class_id = best_cls;
        out_dets.push_back(box);
    }
}

// 2026-08-14: letterbox inverse - 从 640x640 还原到原始 640x480 BGR
// yolov5 letterbox: scale = min(model_w/img_w, model_h/img_h) = min(640/640, 640/480) = 1.0
// new_size = (640, 480), pad = (0, 80) (40 top + 40 bottom)
// 2026-08-14 22:50 v35: 加 edge filter (杀墙/衣架/角落框)
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
    // 2026-08-14 v35: 边缘过滤 - 框任一边在 EDGE_MARGIN_RATIO 范围内 → reject (在 call site 判断, 这里只算)
    // 用 box 中心判断 (避免大框边缘接触但中心在中间的情况)
    float cx = (box.x1 + box.x2) / 2.0f;
    float cy = (box.y1 + box.y2) / 2.0f;
    float mx = EDGE_MARGIN_RATIO * orig_w;
    float my = EDGE_MARGIN_RATIO * orig_h;
    if (cx < mx || cx > orig_w - mx || cy < my || cy > orig_h - my) {
        box.score = -1.0f;  // 标记为 reject (call site 检查 score < 0)
    }
}

// 2026-08-14: 在 BGR 帧上画检测框 + 标签
// 2026-08-17: 加 is_yolov8 参数 切换 class 标签 (80 COCO vs 2 person/not_person)
static void draw_detections(cv::Mat& img, const std::vector<DetBox>& dets, bool is_yolov8) {
    for (auto& d : dets) {
        cv::rectangle(img, cv::Point((int)d.x1, (int)d.y1), cv::Point((int)d.x2, (int)d.y2),
                      cv::Scalar(0, 255, 0), 2);
        char label[128];
        const char* name = is_yolov8
            ? (d.class_id < YOLOV8_NUM_CLASSES ? YOLOV8_CLASSES[d.class_id] : "?")
            : (d.class_id < YOLO_NUM_CLASSES ? YOLO_CLASSES[d.class_id] : "?");
        snprintf(label, sizeof(label), "%s %.0f%%", name, d.score * 100);
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
    // 2026-08-17: argv 10 是模型类型: "yolov8" 或 "yolov5" (默认 yolov5)
    // yolov8 模式跳 yolov5_post_process_bgr (它写死处理 3 outputs), 改走 yolov8_decode
    bool is_yolov8 = false;
    if (argc > 10 && strcmp(argv[10], "yolov8") == 0) is_yolov8 = true;

    printf("=== v31 ALL-C++ V4L2 + OD + RTSP (debug version) ===\n");
    printf("  dev=%s  res=%dx%d  n_bufs=%d  duration=%ds\n", dev, W, H, N_BUFS, DURATION_S);
    printf("  model=%s\n", model_path);
    printf("  yolo: mode=%s conf=%.2f iou=%.2f od_skip=%d\n\n",
           is_yolov8 ? "yolov8" : "yolov5", YOLO_CONF_THRESH, YOLO_IOU_THRESH, OD_SKIP);
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

        // 2026-08-13 debug: 每 30 帧 (约 2s) 保存一帧
        // 1. /tmp/v31_last.yuv - 原始 YUYV (从 /dev/video0, 422 格式)
        //    播放: ffplay -f rawvideo -pix_fmt yuyv422 -video_size 640x480 /tmp/v31_last.yuv
        // 2. /tmp/v31_raw.jpg - 原始 BGR (未画框, 给参考)
        if ((count % 30) == 0) {
            FILE* fy = fopen("/tmp/v31_last.yuv", "wb");
            if (fy) { fwrite(bufs[buf.index], 1, W*H*2, fy); fclose(fy); }
            std::vector<uchar> jpeg_buf;
            cv::imencode(".jpg", bgr, jpeg_buf);
            FILE* fj = fopen("/tmp/v31_raw.jpg", "wb");
            if (fj) { fwrite(jpeg_buf.data(), 1, jpeg_buf.size(), fj); fclose(fj); }
        }

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
        static volatile int od_counter = 0;
        int od_c = od_counter++;
        int det_count = 0;
        // 2026-08-14: OD_SKIP 参数控制 OD 频率 (默认 1=每帧, 为画框稳定不闪烁)
        if ((od_c % OD_SKIP) == 0) {
            void* in_buf = yolov5_pre_process_bgr(bgr.data, W, H, &in_size);
            if (in_buf) {
                void* ib = in_buf;
                awnn_set_input_buffers(ctx, &ib);
                awnn_run(ctx);
                void** outputs = awnn_get_output_buffers(ctx);
                if (outputs) {
                    std::vector<DetBox> all_dets, nms_dets;
                    // 2026-08-17: yolov8 模式跳 yolov5_post_process_bgr (写死 3 outputs) + yolov5_decode_all
                    //   yolov5 模式: 调 libod_helper (会写 "detection num: N" 到 stderr) + 走 yolov5 decode
                    //   yolov8 模式: 不调 libod_helper, 直接用 yolov8_decode_all 解析单 output0
                    if (is_yolov8) {
                        yolov8_decode_all(outputs, W, H, all_dets);
                        yolov5_nms(all_dets, nms_dets);  // 复用现有 NMS (DetBox 兼容)
                        det_count = (int)nms_dets.size();
                    } else {
                    // 2026-08-14 fix: 新 yolov5.nb 是 640x640 input, grid [80, 40, 20]
                    // (之前 416x416 [52, 26, 13] 模型 obj head 损坏已替换)
                    // YOLOv5 output layout per anchor: [x, y, w, h, obj, cls0, cls1, ..., cls79] = 85
                    // 临时调试: 看 person/car logit 最大
                    if ((count % 30) == 1) {
                        float person_max = -1e9f, car_max = -1e9f;
                        for (int s = 0; s < 3; s++) {
                            int gs = (s==0)?80:(s==1)?40:20;
                            float* out = (float*)outputs[s];
                            for (int a = 0; a < gs*gs*3; a++) {
                                float* anch = out + a*85;
                                if (anch[5] > person_max) person_max = anch[5];  // cls 0 = person
                                if (anch[7] > car_max) car_max = anch[7];  // cls 2 = car
                            }
                        }
                        fprintf(stderr, "[NPU] person_logit=%.3f(%.0f%%) car_logit=%.3f(%.0f%%)\n",
                                person_max, 1.0f/(1.0f+expf(-person_max))*100,
                                car_max, 1.0f/(1.0f+expf(-car_max))*100);
                    }
                    yolov5_post_process_bgr(outputs, W, H);
                    fflush(stderr);  // 2026-08-13 fix: stderr 被 dup2 到 file 后变全缓冲, 必须 fflush 才能写到 stderr_file 让 parser 读到
                    std::vector<std::tuple<int,int,int,int,float,std::string>> dets;
                    det_count = parse_detections_from_stderr(stderr_file, dets);
                    // 2026-08-14: 自己 decode + NMS + letterbox_inverse + draw (libod_helper 只输出 count)
                    yolov5_decode_all(outputs, W, H, all_dets);
                    yolov5_nms(all_dets, nms_dets);
                    }  // end is_yolov8 else
                    // 2026-08-14 v35: edge filter 在 letterbox_inverse 里标记 score=-1
                    std::vector<DetBox> filtered_dets;
                    for (auto& d : nms_dets) {
                        letterbox_inverse(W, H, d);
                        if (d.score >= 0.0f) filtered_dets.push_back(d);  // score=-1 = reject
                    }
                    // 2026-08-14 23:05 v35.1: top-K box 限制 (按 conf 降序, 只画前 N 个最强)
                    // 避免误识别太多同时保留真人
                    std::sort(filtered_dets.begin(), filtered_dets.end(),
                              [](const DetBox& a, const DetBox& b) { return a.score > b.score; });
                    if ((int)filtered_dets.size() > MAX_BOX_PER_FRAME) {
                        filtered_dets.resize(MAX_BOX_PER_FRAME);
                    }
                    // 2026-08-15 v38: 5 帧 temporal voting 过滤
                    // 真人持续在同一位置 → hit_count >= 3 通过
                    // 假阳性随机 → hit_count < 3 过滤
                    if (!filtered_dets.empty()) {
                        const DetBox& cur = filtered_dets[0];
                        float cx = (cur.x1 + cur.x2) / 2.0f;
                        float cy = (cur.y1 + cur.y2) / 2.0f;
                        static struct { float cx, cy; int hit_count, last_seen_frame; } tracks[TEMPORAL_TRACKS] = {};
                        static int frame_idx = 0;
                        frame_idx++;
                        // 老化: 超 TEMPORAL_WINDOW 帧没出现 → 清零
                        for (int i = 0; i < TEMPORAL_TRACKS; i++) {
                            if (tracks[i].last_seen_frame > 0 &&
                                frame_idx - tracks[i].last_seen_frame > TEMPORAL_WINDOW) {
                                tracks[i].hit_count = 0;
                                tracks[i].last_seen_frame = 0;
                                tracks[i].cx = tracks[i].cy = 0;
                            }
                        }
                        // 匹配现有 track
                        int best = -1;
                        float best_d2 = TEMPORAL_MAX_DIST * TEMPORAL_MAX_DIST;
                        for (int i = 0; i < TEMPORAL_TRACKS; i++) {
                            if (tracks[i].last_seen_frame == 0) continue;
                            float dx = tracks[i].cx - cx;
                            float dy = tracks[i].cy - cy;
                            float d2 = dx*dx + dy*dy;
                            if (d2 < best_d2) { best_d2 = d2; best = i; }
                        }
                        if (best >= 0) {
                            // 更新平滑位置 (EMA 0.7)
                            tracks[best].cx = 0.7f * tracks[best].cx + 0.3f * cx;
                            tracks[best].cy = 0.7f * tracks[best].cy + 0.3f * cy;
                            tracks[best].last_seen_frame = frame_idx;
                            if (tracks[best].hit_count < TEMPORAL_WINDOW) tracks[best].hit_count++;
                        } else {
                            // 新 track (找空位)
                            for (int i = 0; i < TEMPORAL_TRACKS; i++) {
                                if (tracks[i].last_seen_frame == 0) {
                                    tracks[i].cx = cx; tracks[i].cy = cy;
                                    tracks[i].hit_count = 1;
                                    tracks[i].last_seen_frame = frame_idx;
                                    best = i;
                                    break;
                                }
                            }
                        }
                        // 检查当前 box 对应的 track 是否够票
                        bool passes = false;
                        if (best >= 0 && tracks[best].hit_count >= TEMPORAL_VOTES) {
                            passes = true;
                        }
                        if (!passes) filtered_dets.clear();  // 不够票, 不画
                        // DEBUG
                        if ((count % 30) == 1) {
                            fprintf(stderr, "[TEMP] frame=%d top1=(%.0f,%.0f) hits=%d/%d pass=%d\n",
                                    frame_idx, cx, cy,
                                    best >= 0 ? tracks[best].hit_count : 0,
                                    TEMPORAL_WINDOW, passes ? 1 : 0);
                        }
                    }
                    draw_detections(bgr, filtered_dets, is_yolov8);
                    // 2026-08-14 debug: 每 30 帧保存一帧带框版 (用于诊断/分享)
                    if ((count % 30) == 0) {
                        std::vector<uchar> jpeg_buf;
                        cv::imencode(".jpg", bgr, jpeg_buf);
                        FILE* fj = fopen("/tmp/v31_last.jpg", "wb");
                        if (fj) { fwrite(jpeg_buf.data(), 1, jpeg_buf.size(), fj); fclose(fj); }
                    }
                    // 2026-08-15 v38 DEBUG: 框位置不准调查 - 输出 top-3 box (letterbox 后)
                    // 在 OD 块内写到 stderr (会到 file), 移到块外才能看 journal
                    if ((count % 30) == 1 && !filtered_dets.empty()) {
                        std::vector<DetBox> tmp = filtered_dets;
                        std::sort(tmp.begin(), tmp.end(),
                                  [](const DetBox& a, const DetBox& b) { return a.score > b.score; });
                        printf("[BOXES] after_filter=%d top%d:", (int)tmp.size(),
                               std::min(3, (int)tmp.size()));
                        for (int i = 0; i < std::min(3, (int)tmp.size()); i++) {
                            printf(" #%d cls=%d sc=%.2f [%.0f,%.0f,%.0f,%.0f] center(%.0f,%.0f)", i,
                                   tmp[i].class_id, tmp[i].score,
                                   tmp[i].x1, tmp[i].y1, tmp[i].x2, tmp[i].y2,
                                   (tmp[i].x1+tmp[i].x2)/2, (tmp[i].y1+tmp[i].y2)/2);
                        }
                        printf("\n");
                    }
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
