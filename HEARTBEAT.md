# 2026-09-04 00:28 GMT+8 — 路线 D + v31 binary decode fix + 80 类 person/cat/dog 抖动待修

## 当前状态
- board service: **active**, PID 4285 (v31 binary, yolov5 mode)
- model: `/etc/npu/yolov5/model/yolov5.nb` (md5 **9f8f9f2f**, 5MB, COCO **80 类**)
- binary: `/home/radxa/v31_v4l2_od_rtsp` (md5 **e30c8ad7**, decode layout fix 后)
- argv: `... /dev/video0 640 480 2 86400 /etc/npu/yolov5/model/yolov5.nb 0.20 0.45 1` (无 yolov8 flag → 走 yolov5 mode)
- RTSP: `rtsp://192.168.2.5:8554/test` ✅ LISTEN
- NPU: awnn_run **33-35ms / 帧**, output 85 通道 (4 box + 1 obj + 80 cls)
- FPS: **14.49** 稳态

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
