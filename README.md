# v31_v4l2_od_rtsp — Radxa Cubie A7S Camera RTSP Pipeline

**All C++ single-process V4L2 + NPU OD + x264enc + GStreamer RTSP server**

## 🎯 项目状态

| 版本 | 状态 | 描述 |
|---|---|---|
| **v31** (master) | ✅ **生产可用** | race fix + SDP framerate=14 (跟实际匹配) |
| v30 | ⚠️ 已弃用 | 第一版全 C++，pre-roll 503 问题 |
| v28-v29 | 历史 | Python 主进程 + C++ worker 混合架构 |

## 🎬 端到端架构

```
USB UVC 摄像头 (640x480 YUYV @ 30fps)
   ↓ V4L2 mmap grab
cv::cvtColor YUYV → BGR
   ↓ dlopen libod_helper.so
yolov5_pre_process_bgr + awnn_run (NPU INT8) + yolov5_post_process_bgr
   ↓ (每 3 帧做 1 次 OD, 减少 NPU 负载)
GstBuffer → appsrc → videoconvert → capsfilter I420 → x264enc → h264parse
   ↓ capsfilter H.264 (profile=constrained-baseline, framerate=14/1)
rtph264pay → RTSP server (0.0.0.0:8554/test)
   ↓
PC client: VLC / mpv / ffplay (rtsp://192.168.2.3:8554/test)
```

## 📊 实测性能

- **FPS**: 14.22 cumulative (稳态 ~10 FPS，板端 NPU spike 偶尔 130ms)
- **Latency**: ~100ms 端到端（板端 push → PC 端 decode）
- **CPU/NPU**: aarch64 Allwinner A733, VIPLite NPU
- **分辨率**: 640x480 @ 14 fps
- **码率**: ~1000 kbps H.264 Constrained Baseline L1.0
- **多客户端**: 4 个 RTSP client 同跑已验证 PASS

## 🛠️ 硬件要求

- **板子**: Radxa Cubie A7S (Allwinner A733)
- **内核**: 6.6.98-3-aw2511 (Debian trixie)
- **NPU driver**: VIPLite v2.0.3.x
- **NPU 模型**: `/etc/npu/yolov5/model/yolov5.nb` (INT8 chip3B)
- **摄像头**: USB UVC (Microdia 0c45:6340)
- **库依赖**: 
  - GStreamer 1.0 + gst-rtsp-server-1.0
  - OpenCV 4.x
  - libod_helper.so (自编译 Allwinner OD helper)

## 🔨 编译

**板端 aarch64**:

```bash
cd /home/radxa
g++ -O2 -o v31_v4l2_od_rtsp v31_v4l2_od_rtsp.cpp \
    -I/usr/include/opencv4 \
    $(pkg-config --cflags gstreamer-1.0 gstreamer-rtsp-server-1.0 gstreamer-app-1.0) \
    $(pkg-config --libs gstreamer-1.0 gstreamer-rtsp-server-1.0 gstreamer-app-1.0) \
    -L/home/radxa/od_api/lib -lod_helper \
    -lopencv_core -lopencv_imgproc \
    -Wl,-rpath=/home/radxa/od_api/lib
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

**mpv (推荐)**:
```powershell
mpv "rtsp://192.168.2.3:8554/test" --rtsp-transport=tcp --cache=yes --cache-secs=5
```

**VLC**:
```powershell
vlc "rtsp://192.168.2.3:8554/test" --network-caching=3000 --rtsp-tcp
```

**ffplay**:
```powershell
ffplay "rtsp://192.168.2.3:8554/test" -rtsp_transport tcp -rtbufsize 1500000
```

## 📚 关键迭代历史

### v31 (master) — 2026-08-11 ~ 2026-08-13
- ✅ **8-11 race fix**: appsrc atomic state, max-bytes=10MB, preroll fix, media lifecycle
- ✅ **8-13 SDP framerate**: 30 → 14 跟实际匹配, profile=constrained-baseline
- ✅ 4-client 验证 PASS (1.4x 单 client 性能)
- ✅ PC 端 VLC/mpv 卡顿问题已解决

### v30 (历史) — 2026-08-08
- 第一版全 C++，pre-roll 503 问题（已弃用）

### v28-v29 (历史) — 2026-08-07
- Python 主进程 + C++ worker 混合架构

## 🐛 已知问题 / 观察项

- NPU spike 偶现 50-130ms（每 1.5min 一次），由 VIPLite driver mutex contention
- 内存未回 baseline (84M → 211M, glibc fragmentation，需长时间观察是否 leak)
- count % 3 修复实际生效（G++ -O2 未优化掉，od_ms 0.1 样本 ≈33%）

## 📂 文件说明

| 文件 | 说明 |
|---|---|
| `v31_v4l2_od_rtsp.cpp` | **当前生产代码 (master)** |
| `v31_v4l2_od_rtsp.cpp.bak_20260811_0014_before_race_fix` | race fix 前 snapshot |
| `v31_v4l2_od_rtsp.cpp.bak_debug_20260808` | debug 早期版本 |
| `v31_v4l2_od_rtsp.cpp.bak_fps14_20260813` | 8-13 fps14 修复前备份 |
| `v30_v4l2_od_rtsp.cpp` | v30 全 C++ 第一版（已弃用） |
| `v32_3thread_atomic.cpp` | 老板探索的 3 线程版本（未采用） |
| `v28_v4l2_bench.cpp` | USB grab benchmark |
| `v29_v4l2_od_push.cpp` | Python+C++ 混合版（历史） |
| `deploy/camera-rtsp-v31.service` | systemd unit |
| `deploy/debug.conf` | GST_DEBUG drop-in |

## 📝 License

(待老板决定)
