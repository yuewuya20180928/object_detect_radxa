# v31_v4l2_od_rtsp — Radxa Cubie A7S Camera RTSP Pipeline

**All C++ V4L2 + NPU OD + x264enc + GStreamer RTSP server (架构 A: OD 独立 worker thread)**

## 🎯 项目状态

| 版本 | 状态 | 描述 |
|---|---|---|
| **v31** (master, 架构 A) | ✅ **生产可用** | OD 独立 worker thread + SDP framerate=30 |
| v31 (legacy, 单线程) | 🗄️ 备份 | md5 47549d27, fps 19.83 OD-bound |
| v31-odthread-v1 | 历史 | md5 6dbc87cd, 架构 A MVP, fps 29.66 |
| v31-odthread-v2 | 🗑️ 失败 | md5 50d7d46a, +BgrRing 失败, fps 24.5 |
| v30 | ⚠️ 已弃用 | 第一版全 C++，pre-roll 503 问题 |
| v28-v29 | 历史 | Python 主进程 + C++ worker 混合架构 |

## 🎬 端到端架构 (架构 A)

```
USB UVC 摄像头 (Deli 1080P 640x480 YUYV @ 30fps ceiling 29.82)
   ↓ V4L2 mmap grab
cv::cvtColor YUYV → BGR
   ↓ main loop push q_frame (bgr.clone ~0.5ms)
   ↓
   ┌─────────────────────────┐  ┌──────────────────────────┐
   │ main thread             │  │ OD worker thread         │
   │ DQBUF → cvtColor        │  │ pop q_frame              │
   │ → push q_frame (max=2,  │  │ → yolov5_pre_process_bgr │
   │   drop oldest)           │  │ → awnn_set_input_buffers │
   │ → pop q_result last     │  │ → awnn_run (NPU 36ms ★) │
   │ → draw last_det on bgr  │  │ → awnn_get_output        │
   │ → push appsrc           │  │ → yolov5_decode_all +    │
   │ → QBUF                  │  │   yolov5_nms + cyl_iou   │
   └─────────────────────────┘  │ → push q_result (max=1)  │
                                └──────────────────────────┘
GstBuffer → appsrc → videoconvert → capsfilter I420 → x264enc → h264parse
   ↓ capsfilter H.264 (profile=constrained-baseline, framerate=30/1)
rtph264pay → RTSP server (0.0.0.0:8554/test)
   ↓
PC client: VLC / mpv / ffplay (rtsp://192.168.2.5:8554/test)
```

**关键设计**:
- main 跟 OD worker 完全解耦, main 不等 OD (每帧 33ms, 拉满 30fps camera ceiling)
- OD worker 异步消费 q_frame, 每帧 ~36ms (NPU 27ms + memcpy 9ms)
- det 显示 = last_det (1 frame latency 33ms, 老板接受 "不能隔帧检测" 约束)
- q_frame max=2 drop oldest backpressure, q_result max=1 (main 只用 latest)

## 📊 实测性能 (架构 A 上线后, 10/11 实测)

| 指标 | 旧版 (单线程) | **架构 A** | 提升 |
|---|---|---|---|
| **fps** | 19.83 | **29.78** | **+50%** |
| **camera ceiling (无 OD)** | 15 (Microdia) / 29.82 (Deli) | 29.82 | — |
| **主线程 od time** | 36-44ms | **0.3-2ms** (draw only) | — |
| **RTSP client 拉流 fps** | 19.7 | **29.78 不掉** | — |
| **det 显示延迟** | 0 (同步) | 33ms (1 frame) | — |

### 整个流程 per-frame timing (架构 A, 660 samples avg)

| 主线程阶段 | avg | min | max |
|---|---|---|---|
| dq (DQBUF capture) | 31.15ms | 27.30 | 34.90 |
| cvt (YUYV→BGR) | ~0ms | — | — |
| od (push q_frame + pop det + draw) | 2.14ms | 0.90 | 3.90 |
| push (appsrc) | 0.19ms | 0 | 1.10 |
| qb (QBUF) | 0.05ms | 0 | 0.10 |
| **主线程合计** | **~33.5ms** | 28 | 39 |

| OD worker 阶段 | avg | max |
|---|---|---|
| vip_run_network (NPU 硬件) | 27.19ms | 29.58 |
| tensor_to_fp (INT8→fp32 memcpy) | 9.29ms | 21.85 |
| load_input_data | 0.35ms | 0.43 |
| **awnn_run total** | **36.51ms** | 49.73 |

**Camera ceiling 29.82 fps vs 实测 29.78 fps = 拉满**

## 🛠️ 硬件要求

- **板子**: Radxa Cubie A7S (Allwinner A733)
- **内核**: 6.6.98-3-aw2511 (Debian trixie)
- **NPU driver**: VIPLite v2.0.3.x
- **NPU 模型**: `/etc/npu/yolov5/model/yolov5.nb` (INT8 chip3B, COCO 80 类)
- **摄像头**: **Deli 1080P Camera (3001:4034) USB UVC** —— 替换之前的 Microdia (实测只 15fps)
- **库依赖**:
  - GStreamer 1.0 + gst-rtsp-server-1.0 + gst-app-1.0
  - OpenCV 4.x
  - libod_helper.so (自编译 Allwinner OD helper)

## 🔨 编译

**板端 aarch64**:

```bash
cd /home/radxa
g++ -O3 -std=c++14 -o v31_v4l2_od_rtsp v31_v4l2_od_rtsp.cpp \
    -I/usr/include/opencv4 \
    $(pkg-config --cflags --libs gstreamer-1.0 gstreamer-app-1.0 gstreamer-rtsp-server-1.0) \
    -lopencv_core -lopencv_imgproc -lopencv_imgcodecs \
    -ldl -lpthread \
    -Wno-unused-parameter -Wno-deprecated-declarations
```

## 🚀 部署

详见 `deploy/` 目录:

- `camera-rtsp-v31.service`: systemd unit
- `debug.conf`: GST_DEBUG drop-in

```bash
# 部署
scp v31_v4l2_od_rtsp radxa@<board>:~/
scp deploy/* radxa@<board>:/tmp/
ssh radxa@<board>
sudo cp /tmp/camera-rtsp-v31.service /etc/systemd/system/
sudo cp -r /tmp/camera-rtsp-v31.service.d /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable --now camera-rtsp-v31.service
```

## 🖥️ PC 端拉流

**mpv (Windows, 老板验证 work, 10/11)**:
```powershell
& "C:\Users\Administrator\Downloads\mpv-x86_64-20230723\mpv.exe" --rtsp-transport=tcp --cache=yes --cache-secs=3 --no-config rtsp://192.168.2.5:8554/test
```

**VLC**:
```bash
vlc --rtsp-tcp --network-caching=3000 rtsp://192.168.2.5:8554/test
```

**ffplay**:
```bash
ffplay -rtsp_transport tcp -fflags nobuffer rtsp://192.168.2.5:8554/test
```

## 📚 关键迭代历史

### v31 架构 A — 2026-10-11
- ✅ **架构 A (OD 独立 worker thread)**: main 不等 OD, fps 19.83 → **29.78** (+50%)
  - 代码改动 +130 行 (FrameTask + DetResult + FrameQueueT 模板 + DetectorWorker class)
  - main loop 改造: push q_frame (bgr.clone 0.5ms) + pop last_det + draw
- ✅ **SDP framerate 14→30 修 (旧 bug)**: VLC/mpv buffer 设计跟实际匹配
- ✅ **摄像头换 Deli 1080P**: camera ceiling 15fps → 29.82fps
- ✅ **BgrRing 试过 (回滚)**: ring acquire 阻塞 OD 慢时 main, fps 跌到 24.5

### v31 race fix — 2026-08-11
- appsrc atomic state, max-bytes=10MB, preroll fix, media lifecycle
- 4-client 验证 PASS (1.4x 单 client 性能)

### v31 SDP fix — 2026-08-13
- 老 SDP framerate=30 → 14 (当时实际 14 fps)
- profile=constrained-baseline, x264 level 协商修复
- 2026-10-11 架构 A 上线后 SDP framerate 14 → **30**

### v30 (历史) — 2026-08-08
- 第一版全 C++，pre-roll 503 问题（已弃用）

### v28-v29 (历史) — 2026-08-07
- Python 主进程 + C++ worker 混合架构

## 🐛 已知问题 / 观察项

- **service 启动撞 polkit**: 10/11 production 替换后 `sudo systemctl start` 要交互认证, 当前 nohup PID 4317 跑同 cmdline 接管
  - 老板接管: `sudo systemctl reset-failed camera-rtsp-v31 && sudo systemctl start camera-rtsp-v31` 后 `kill 4317`
- **Deli USB 摄像头是 DHCP**: 板端 IP 会换, 拉流前先 `nslookup radxa-cubie-a7s.local` 确认当前 IP
- NPU spike 偶现 50-130ms (每 1.5min 一次), VIPLite driver mutex contention (架构 A 不掉 fps, OD worker 自己 drop frame 即可)
- 内存未回 baseline (84M → 211M, glibc fragmentation, 需长时间观察是否 leak)

## 📂 文件说明

| 文件 | 说明 |
|---|---|
| `v31_v4l2_od_rtsp.cpp` | **当前生产代码 (架构 A)** |
| `v31_v4l2_od_rtsp_odthread.cpp` | v1 架构 A MVP cpp (已编入 `_v1` binary) |
| `v31_v4l2_od_rtsp_odthread_v2.cpp` | v2 +BgrRing (失败版 cpp) |
| `v31_v4l2_od_rtsp_odthread_v3.cpp` | v3 = v2 -BgrRing +SDP fix (= 生产 cpp) |
| `backup_2026-10-11/v31_v4l2_od_rtsp.cpp.bak_pre_odthread_*` | 老 production cpp 备份 |
| `v31_v4l2_od_rtsp.cpp.bak_*` | 历史 snapshot |
| `v30_v4l2_od_rtsp.cpp` | v30 全 C++ 第一版（已弃用） |
| `v32_3thread_atomic.cpp` | 老板探索的 3 线程版本（未采用） |
| `v28_v4l2_bench.cpp` | USB grab benchmark |
| `v29_v4l2_od_push.cpp` | Python+C++ 混合版（历史） |
| `deploy/camera-rtsp-v31.service` | systemd unit |
| `deploy/debug.conf` | GST_DEBUG drop-in |
| `memory/2026-10-11.md` | 10-11 详细日志 (架构 A 完整工作记录) |
| `HEARTBEAT.md` | 板端状态快照 (10-11 更新在最顶) |
| `MEMORY.md` | 长期记忆 + 铁律 |

## 📝 License

(待老板决定)