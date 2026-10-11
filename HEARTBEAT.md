# 2026-10-11 15:30 GMT+8 — OD 独立 thread 上线, fps 19.83 → 29.78 (拉满 camera ceiling)

## 当前状态 (2026-10-11 15:30 更新)
- 板端服务: systemd `camera-rtsp-v31.service` **failed** (撞 polkit lock), nohup **PID 4317** 跑生产 binary (cmdline 跟 service ExecStart 一致, RTSP 推流可用)
- 摄像头: **新换 Deli 1080P Camera (3001:4034)** —— 之前 Microdia 实测只跑 15fps, Deli 满 30fps
- model: `/etc/npu/yolov5/model/yolov5.nb` (5MB, COCO 80 类) — 不变
- binary (生产, **NEW**): `/home/radxa/v31_v4l2_od_rtsp` md5 `323b0d84` (架构 A + SDP fix, 82936 bytes)
- binary (备份): `/home/radxa/v31_v4l2_od_rtsp.bak_pre_odthread_v3_20261011_030219` md5 `47549d27` (老 production)
- binary (测试): `v31_v4l2_od_rtsp_odthread_v1` md5 `6dbc87cd` (架构 A 29.66fps), `_v2` md5 `50d7d46a` (架构 A+BgrRing 24.5fps 失败回滚), `_v3` md5 `323b0d84` (=生产)
- argv: `... /dev/video0 640 480 2 86400 /etc/npu/yolov5/model/yolov5.nb 0.20 0.45 1` — 不变
- RTSP: `rtsp://192.168.2.5:8554/test` ✅ LISTEN
- 客户端: mpv 拉流 ESTAB `192.168.2.5:8554 ← 192.168.2.2:57910` (Windows 端)
- **最新 fps** (15:30, RTSP 拉流期间): **29.78 fps** | dq=29 cvt=0 od=2 push=0 qb=0 ms | dets=0 client=1
- 跑了 16min+ (660 samples), avg=29.67, min=16.70 (启动), max=29.79 (稳态)

## 关键改动

### 1. 架构 A — OD 独立 worker thread
v31 main loop 拆 OD 流程到独立 worker thread:
```
main: DQBUF → cvtColor → push q_frame (bgr.clone 0.5ms) → pop last_det → push appsrc → QBUF
worker: pop q_frame → pre_process → awnn_run (35ms) → decode + nms → push q_result
```
main 不等 OD, OD 异步跑, det 显示用 last_det (1 frame latency 33ms)。
**fps 19.83 → 29.66 (MVP 测) → 29.78 (production 跑 16min+)**
- 代码改动: +130 行 (FrameTask + DetResult + FrameQueueT 模板 + DetectorWorker class)
- 新 cpp: `v31_v4l2_od_rtsp_odthread_v3.cpp` (39557 bytes), workspace 同步 `v31_v4l2_od_rtsp.cpp`

### 2. SDP framerate 14→30 修 (旧 bug)
- 老 `g_state.FPS = 14` + 3 处 caps `framerate=14/1` 是 8/13 留的 (当时实际 14 fps)
- 现在 fps 30, SDP 报 30, VLC/mpv buffer 设计跟实际匹配, 抗 30fps 抖动

### 3. BgrRing 试过 (失败, 回滚)
- 试 ring buffer (3 slots, mutex + cv) 替代 bgr.clone() zero-copy
- **fps 24.5** (退步 5fps), ring acquire 阻塞 12-56ms
- 根因: OD worker 慢 17% 时 ring 满, main acquire 阻塞等 OD release → main 退化成同步模式
- 回滚到 clone 方案 (0.5ms 比 ring 阻塞 30ms+ 好)
- **教训**: ring buffer 只有在 OD ≥ capture 节奏时才行

## 整个流程 per-frame timing (660 samples avg)

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

## service 下一步 (老板决定)
- service 需要 sudo 接管: `sudo systemctl reset-failed camera-rtsp-v31 && sudo systemctl start camera-rtsp-v31`, 然后 `kill 4317`
- v24 service (UDP 推流) 仍 failed, 老板没要求起
- 要再涨 fps: 换 USB3 camera 或更小 model (yolov5n 320, NPU 推理减半)

# 2026-09-19 19:48 GMT+8 — 板端服务 9-6 16:20 后挂掉，9-19 19:48 重启恢复

## 当前状态 (2026-09-19 20:54 更新)
- board service: **active**, PID 4605 (v31 production binary, yolov5 mode)
- MT 测试期间 service 重启了一次（手杀 PID 2761 试新 binary），现在 PID 4605 恢复
- model: `/etc/npu/yolov5/model/yolov5.nb` (5MB, COCO **80 类**)
- binary (生产): `/home/radxa/v31_v4l2_od_rtsp` (md5 47549d27, 老版)
- binary (DRM 版，已编译): `/home/radxa/v31_v4l2_od_rtsp_drm_v1` (md5 e57eabf3, **等 USB-C 线到后切换**)
- argv: `... /dev/video0 640 480 2 86400 /etc/npu/yolov5/model/yolov5.nb 0.20 0.45 1`
- RTSP: `rtsp://192.168.2.5:8554/test` ✅ LISTEN
- 最新 FPS（20:30）: count=587 fps=**14.05** | dq=16.3 cvt=0.5 od=43.6 push=0.0 qb=0.0 ms | dets=1174
- IP 确认: `getent hosts radxa-cubie-a7s.local` → 192.168.2.5（没换）

## 多线程 MT 实验 (2026-09-19 20:54)
**结论：MT 架构正确，FPS 没涨（14 → 13.85）**

| 指标 | Production | MT | 差异 |
|---|---|---|---|
| FPS | 14.05 | 13.85 | -1% |
| capture fps | (同 process) | **16.96** | +20% (camera cap) |
| process fps | (同 capture) | 13.48 | -3% |
| `dq` (capture) | 30.9ms | **67ms** | camera 实际 15fps |
| process cycle | 71ms | 75ms | OD + decode + NMS + draw |

**根因**：
1. **UVC camera 实际只有 15fps**（v4l2-ctl 报 max 30fps 是 capability, 不是 actual）。`dq=67ms` 直接证据。
2. **process 是 OD-bound**（awnn_run 35-50ms + decode + NMS + draw 20ms = ~70ms），MT 解耦 capture/process 没用，process 还是串行跑 OD。

**结论修正**：之前 "MT → 22 fps" 的预估是错的（基于 camera 30fps 的假设）。Camera 实际 15fps，FPS 上限 ≈ 15-17fps。

**下一步（看老板决策）**：
- [ ] OD_SKIP=2 测: 每 2 帧跑一次 OD, 预期 17-20 fps, 但一半帧无 detection
- [ ] 减 OD 后处理: skip yolov5_decode_all / draw, 预期 +10-15% FPS
- [ ] camera 调到真 30fps: 查 UVC sensor + USB config, 可能需要重启

**MT 版本已编译好**: `/home/radxa/v31_v4l2_od_rtsp_mt_v1` (md5 b2fd4d5b, 82168 bytes), 备份 cpp `v31_v4l2_od_rtsp.cpp.bak_pre_mt_20260919_1246` + binary `v31_v4l2_od_rtsp.bak_pre_mt_20260919_1246`。架构本身以后需要时可以切（接 DRM、换 sensor 等都适用）。

## DRM/KMS HDMI 本地显示 进度（2026-09-19 20:25 启动）
**目标**：USB-C 转 HDMI 线 → 把 v31 检测后画面直接送 HDMI 屏（不走 weston）

**已完成**：
- ✅ v31 备份: `.bak_drm_pre_20260919_1220` (cpp + binary)
- ✅ DRM 模块写入 `v31_v4l2_od_rtsp_drm_v1.cpp` (+239 行 → 1075 行)
  - init_display: open /dev/dri/card0 → drmModeGetResources → 找 connected connector → 选 PREFERRED mode → alloc dumb buffer (XRGB8888) → mmap → drmModeAddFB2 → drmModeSetCrtc
  - display_frame: cv::resize → 信箱居中 → cv::cvtColor BGR→BGRA → 写 dumb_map (kernel 自动 scanout)
  - cleanup_display: SetCrtc off → RmFB → munmap → DestroyDumb → close
- ✅ 集成 main loop: init 在 STREAMON 之后, display_frame 在 draw_detections 后每帧调, cleanup 在 STREAMOFF 后
- ✅ build 脚本: `/home/radxa/build_v31_drm.sh`（g++ -O3 -std=c++14 -Wall, 0 warning）
- ✅ 离线烟测 (无 DP sink): DRM init graceful fail (`[DRM] no connected connector found`), RTSP 跑 13 fps 不退化, OD 正常出 det (cls=0 person 0.50), 退出干净
- ✅ Production service 恢复到 14.05 fps

**待办（线到了做）**：
- [ ] 接 USB-C 转 HDMI 线 + HDMI 显示器
- [ ] 切 service ExecStart 到 `v31_v4l2_od_rtsp_drm_v1`
- [ ] 重启 service → 看 `[DRM] HDMI display enabled: 1920x1080 ...`
- [ ] 如果报 `SetCrtc initial failed`：sunxi-drm DP Alt Mode 不工作 → 改方案
- [ ] 如果报 `no connected connector found`：线没插好 / sink 没枚举到 → 重插线 / 检查 typec driver

## 今晚操作
1. 19:47 老板下令开启 → systemctl status 显示 inactive (dead), Job:122 (pending)
2. 19:48 systemctl start → "Interactive authentication required" 报错，但 service 实际起来了 (pid 913 elapsed=29s)
3. 19:49 FPS 14.0 稳态，2658 dets / 790 帧 (≈3.4 dets/帧)
4. 20:25 老板: 加 USB-C 转 HDMI 接屏功能
5. 20:19 moli: 写 DRM 模块到 `v31_v4l2_od_rtsp_drm_v1.cpp`（离线准备，线到了即测）
6. 20:25 moli: 离线烟测通过（DRM fail graceful / RTSP 不退化 / service 恢复）
4. systemctl status 仍报 "active since 2026-09-06 16:20:28 UTC"（systemd 状态缓存未刷新，误导）

## 9-3 关键进展
1. **路线 D 切换**: yolov8s_scene_ft_v2.nb (2 类) → yolov5.nb (80 类)
2. **camera 物理调整**: close-up face → 房间全景 (老板执行)
3. **v31 binary decode layout fix**: 关键 bug 修复，box 跟人走
4. **根因分析 (person/cat/dog 抖动)**: COCO 训练分布 + 640×640 浅层特征 → 方案 Z+X 治本
5. **Windows VLC 拉流验证**: 老板用 `vlc --rtsp-tcp --network-caching=3000` 在 Windows 拉流成功

## 板端 dump 文件 (debug 用)
- `/tmp/v31_raw.jpg`: 原始 BGR frame (无 overlay), 每 30 帧 (~2s) 更新
- `/tmp/v31_last.jpg`: 带 overlay 的 frame (yolov5 draw_detections 输出), 每 30 帧更新
- `/tmp/v31_last.yuv`: 原始 YUYV422, 640×480×2 = 614400 bytes

## 备份链
```
service 当前:  /etc/systemd/system/camera-rtsp-v31.service (yolov5.nb + yolov5 mode + conf=0.20)
service 备份:  .bak_routeD_20260903_153946 (conf=0.40)
              .bak_conf20_<ts> (conf=0.20 当前)
              .bak_decode_fix_<ts> (conf=0.40 二进制 fix 前)
binary 当前:   /home/radxa/v31_v4l2_od_rtsp (md5 e30c8ad7, decode fix)
binary 备份:   .bak_decode_fix_20260903_155527 (旧 c2a24c79, 修复前)
              .bak_decode_fix2_20260903_155638
cpp 备份:      v31_v4l2_od_rtsp.cpp.bak_decode_fix_20260903_155527
.nb 备份:      /etc/npu/yolov5/model/ 下完整保留 (yolov5.nb / yolov8s.nb / yolov8s_scene_ft_v1/v2/int8_v3)
```

## 下一步 (老板决定)
- [ ] (30 min) **方案 Z** = v31 binary 加 5 帧 temporal voting (修帧间跳)
- [ ] (半天) **方案 X** = 换 yolov8s 80 类 backbone + docker Vivante IDE 重出 .nb (修治本)
- [ ] (可选) 修 RTSP server SDP port=0 bug (Windows 防火墙是临时方案)
- [ ] (可选) 取消 fine-tune 路线 (老板 deep insight: 单点解, catastrophic forgetting 破坏 80 类)
