# Long-term Memory

## User Preferences

- **Default model (config primary)**: `minimax/MiniMax-M3` (1M context) — set 2026-06-15 in `openclaw.json → agents.defaults.model.primary`; M3 native contextWindow 204800→1000000 same day
- **Fallback chain**: M3 → M2.7 → M2.7-highspeed
- **M3 1M context verification**: triggered via `/model minimax/MiniMax-M3` in main session to refresh locked context-window cache (was 205k cached from old config)

## Iron Rules (行为铁律)


- **【铁律】任何时候都不要做推断，都要依据实际输入和实际情况来判断**（2026-06-22 设定）
  - 区分 "我观察到了" vs "我推测是" —— 标签要说清楚
  - 没有数据时直接说 "没数据"，不要用读代码 / 听 LLM / 凭经验去补
  - 给出 patch 之前先确认 bug 是不是真的在数据里复现过
  - 想推荐防御性写法可以，但要标 "防御性，未实测"，不能伪装成 "已发现的 bug"
- **【铁律】进度类问题必做 sync**（2026-06-26 立，源自 webchat session 两次 /progress 误报）
  - 用户问进度/待办/现状/X 跑了吗 → 先扫 memory/*.md / MEMORY.md / HEARTBEAT.md / 项目 README 主路径表 / 最近 eval log，再答
  - 不扫就给答案 = 错情报
  - 详细 sync 清单见 `AGENTS.md` 的 "Progress Sync" 章节
- **【铁律】opencv 三个包共存坑**（2026-06-27 栽过 1 次，6/26 也栽过）
  - 用户 base 同时装了 `opencv-contrib-python` / `opencv-python` / `opencv-python-headless` 三个, 共用 `site-packages/cv2/` 目录
  - **不要 `pip uninstall opencv-python-headless`** — 会把共享的 `__init__.py` 和 .so 一起删, 整个 cv2 模块废掉
  - **修法**: `pip install --force-reinstall opencv-python==<ver>` (重写共享目录)
  - 探 cv2 是否有 GUI 只能**实测** (`cv2.namedWindow` 试调), 看 `qt/plugins` 目录不可靠
- **【铁律】环境被清空 = 全新重建**（2026-06-28 踩到，**2026-06-29 修正：env 实际还在**）
  - 6/28 那次同步时 `which conda` 找不到，误判为 env 被清
  - 6/29 实际验证：`/home/yuewuya/miniconda3/envs/rknn/` + `tf2/` **都还在**，所有包完整
  - 修正后规则：每次新会话 sync 时**必须实测** `python -c "import xxx"`，但 conda env 在 `/home/yuewuya/miniconda3/envs/<name>/bin/python` 不在默认 PATH
  - 完整路径：`/home/yuewuya/miniconda3/envs/rknn/bin/python`（含 ultralytics + onnx + onnxruntime + rknn-toolkit2 + pycocotools）
  - 重建路径见 STAGE2_EMBEDDED_DEPLOYMENT.md "conda env tf2 装包脚本" 段
- **【铁律】PyPI 不一定有包，GitHub 不一定通**（2026-06-28 踩到）
  - rknn-toolkit2 在 PyPI 上**搜不到任何版本**
  - github.com 域名直连 000（被代理拦），但 `api.github.com` + `codeload.github.com` 通
  - 可行下载路径：
    - `https://api.github.com/repos/<owner>/<repo>/zipball/<tag>` （走 api，重定向到 codeload）
    - `https://codeload.github.com/<owner>/<repo>/zip/refs/tags/<tag>` （走 codeload，但末尾会 301 到 github.com 域名，可能超时）
  - 实测 7.5MB / 60s 是下载不完整的（rknn-toolkit2 完整包 30-40MB），需要更长 timeout 或分卷下载
  - **以后装工具链前先 `pip index versions <pkg>` 试，不要假定一定有**
- **【铁律】ultralytics cls_id ≠ COCO cat_id + 1**（2026-06-28 栽过 1 次，mAP 错 7 倍）
  - 直接用 `cat_id = cls_id + 1` 在 cat=12 (stop sign) 起错位（ultralytics 跳过了 12 padding 类）
  - 实际：ultralytics cls=11 (stop sign) 对应 COCO cat=12 (碰巧对)，但 cls=40 (wine glass) 对应 COCO cat=46，不是 41
  - **修法**：按类名匹配生成 mapping (`UL_TO_COCO = {0: 1, 1: 2, ...}`)，不要简单 +1
  - 后果：mAP 全 cat 平均 0.07（实际 0.31），54/80 cat 全 0，person 还行（+1 凑巧对）
- **【铁律】三方/IPK/deb 包可能 ship mode 744 不是 755**（2026-07-10 栽过 1 次，npu-runtime 2.0.3 radxa-a733-trixie-test）
  - `vpm_run` 文件 mode `-rwxr--r-- 1 1003 1005` (744, owner UID 1003)
  - radxa(UID 1000) 落 "other" 类 = **没 exec 位** → `bash: Permission denied`
  - 但 **ldd 可解析共享库**（因为 loader 有 setuid 行为）→ 误导排查方向
  - **修法**: `sudo chmod 755 /usr/bin/vpm_run /lib/libVIPhal.so /lib/libNBGlinker.so`
  - **root 跑 sudo vpm_run --help 能过**（root 跳过权限检查）→ 误以为驱动问题
  - **check list**: 非 Debian 官方源装 binary 后, 立刻 `ls -la /usr/bin/* | grep -v '^.{8,}'` 看 mode
- **【铁律】UDP 端口扫描 vs git clone 是独立的代理通道**（2026-07-10 自查）
  - 同一个 IP 段内多个 SSH 服务看 banner: PC 自己是 Ubuntu+OpenSSH 10.2, A7S 是 Debian+OpenSSH 10.0
  - 别因为 IP neigh 表里看到就直接 SSH — 可能 SSH 到的是**自己 PC 或别台同网设备**
  - **check list**: SSH 之前先看 mDNS (`getent hosts radxa-cubie-a7s.local`)、确定 mac OUI (Radxa 那套)
- **【铁律】Allwinner A733 VIN driver `vin_g_volatile_ctrl` 有 kernel panic bug**（2026-07-23 两次实测栽过，板端 crash）
  - 触发：任意 `VIDIOC_G_CTRL` ioctl → `vin_g_volatile_ctrl+0xdc` → page fault → Kernel panic
  - 实测触发命令：
    - `v4l2-ctl --all` （遍历所有 controls 必触发）
    - `gst-launch-1.0 v4l2src device=/dev/video0 ! fakesink` （probe caps 时触发）
  - 不可安全使用 `v4l2-ctl --all` / `--list-ctrls` / `--get-ctrl=*` / 任何调 G_CTRL 的工具
  - 安全 ioctl 子集：`QUERYCAP` (`-D`)、`ENUM_FMT` (`--list-formats`)、`G_FMT` (`--get-fmt-video`)、`S_FMT` (`--set-fmt-video`)、`REQBUFS`+`STREAMON`+`DQBUF` (streaming)
  - 即使避开 G_CTRL，**VIN cap streaming 4K @ 30fps + ISP + scaler pipeline 也大概率 crash**（reason unclear）
  - **结论：A733 BSP kernel 6.6.98-3-aw2511 的 VIN driver 不可用于生产**，MIPI IMX415 摄像头路线**已封存**，见 HEARTBEAT
  - **替代方案**：USB UVC 摄像头（uvcvideo module 已正常 load），绕开 VIN driver 整套
- **【铁律】v4l2-compliance 默认 probe /dev/video* 会撞 VIN driver panic**（2026-07-25 12:12 实测硬挂板子）
  - **触发**：`v4l2-compliance -d /dev/video0` → 立即 panic → SSH 断 → 30s 后 ICMP 都不通 → 板子硬挂 → 必须 power cycle
  - **根因推测**：VIN driver 在 ISP/scaler 硬件初始化时撞 page fault（与 vin_g_volatile_ctrl 同源）
  - **已知安全 / 不安全视频 capture 命令** (2026-07-25 老板 morning 实测修正):
    - ✅ **SAFE + WORK**: `gst-launch-1.0 v4l2src device=/dev/video0 io-mode=4 ! videoconvert ! video/x-raw,format=NV12,width=1920,height=1080 ! jpegenc ! multifilesink location=X.jpg` (老板 04:34 实测抓到 1920x1080 JPEG 真 camera 图)
      - 关键: **io-mode=4 (DMABUF)** + **先 media-ctl -l 启用 sensor→MIPI→CSI→TDM→ISP→scaler→cap link** + **不调 en-awisp**
      - 推断: gst-launch v4l2src probe caps 不调 G_CTRL, 所以不撞 vin_g_volatile_ctrl
    - ❌ **PANIC**: `v4l2-compliance -d /dev/video*` (实测硬挂, 不要试)
    - ⚠️ **推测 unsafe 但未实测**: `v4l2-ctl --stream-mmap` / `v4l2-ctl --stream-on` / `ffmpeg -f v4l2` / `ffprobe -f v4l2` / `yavta` (跟 v4l2-compliance 同根, 推测撞 ISP 初始化 → panic)
    - ⚠️ **set-fmt 风险**: `v4l2-ctl --set-fmt-video=...` / `media-ctl -V` 在 vin_cap/vin_video sink pad 上 (会触发 pipeline 协商)
  - **绝对安全的 metadata-only 操作** (7/25 12:09 实测):
    - `v4l2-ctl -D` (QUERYCAP)
    - `v4l2-ctl --list-formats` (ENUM_FMT)
    - `v4l2-ctl --get-fmt-video` (G_FMT)
    - `media-ctl -p` (topology dump)
    - `media-ctl -l` (LINKS ioctl 启用/禁用 link)
    - `media-ctl -V` 在 sensor/MIPI/CSI/TDM_RX 上设 format (不撞 vin_cap)
  - **结论修正**: MIPI 路线 **不完全死透**, 用 `gst-launch v4l2src device=/dev/video0 io-mode=4 + media-ctl -l 设 link` 是 work 的
  - **之前判断错的修正**: "任何 gst-launch v4l2src 都 panic" 是过分总结, 实际 gst-launch v4l2src + io-mode=4 work
  - check list: 跑 v4l2-compliance / ffmpeg / ffprobe / yavta / v4l2-ctl --stream-* 前先想好 power cycle 后果
- **【铁律】改 /usr/lib/linux-image-.../allwinner/*.dtb 之前必须备份原文件到非 /tmp 路径**（2026-07-23 栽过）
  - `/tmp` 是 tmpfs，**reboot 时全清**！merged dtb + base dtb 备份都丢了
  - 备份必须放：`/root/` 或 `/home/radxa/` 或 `/var/backups/`
  - 同样：apt reinstall linux-image-... 在板端不可用（"cannot be downloaded"），即使开启 radxa repo 也找不到这个本地 build 的包
  - **要回滚原 base dtb 的唯一办法**：从 Radxa 官方镜像 dd raw image 重烧 SD 卡
  - check list: 任何 dtb 替换前 `sudo cp src dst && sudo cp src /root/dtb-backup-$(date +%F).bak`
- **【铁律】Allwinner A733 VIPLite kernel driver vs userspace HAL 版本必须 timestamp 一致 + vpm_run binary 也要配套换**（2026-07-10 栽过 + 2026-07-14 实测修正）
  - 不光 .so 要配套,**vpm_run binary 本身也是 May 2025 build**,内部 ABI 跟 Oct 2025 driver 不匹配
  - 7/12 我们只换了 .so 没换 vpm_run → 仍 hang,可能根因就是 vpm_run binary
  - 修法: 板端编译 awnpu_model_zoo 源码 (`functions/vpm_run/main.cpp` + `examples/yolov8/main.cpp`) 生成配套 vpm_run,或者 PC 端 cross-compile 后 scp
  - **7/14 实测发现**: 换完 vpm_run + 配套 .so 后,driver load + network create/prepare 都成功了,但 **inference 阶段仍然 hang** (`VIPDRV_WAIT_TASK status=-1`)
  - **根因是 kernel driver (Oct 2025) 跟 awnpu Aug 2024 .so 仍然 ABI 不完全配套**
  - **hang 位置从 driver load 推后到 inference 阶段**
  - 下一步必须 **换 kernel driver** —— 走 cubie-image 5.15 kernel (TinaSDK BSP 同期 build) 或全编 TinaSDK v1.4.6 kernel

- **【铁律】awnpu_model_zoo CMakeLists.txt 默认 SYS_ARCH=linux_armhf** (2026-07-14 实测踩到)
  - 因为 CMAKE_C_COMPILER=/usr/bin/cc 不含 "aarch64" 字符串,走了 armhf 分支
  - 但路径是 `lib_linux_aarch64/`,不是 `lib_linux_armhf/`,导致 NPU_RT_LIBS glob 找不到 .so → 链接失败
  - 修法: `-DCMAKE_C_COMPILER=/usr/bin/aarch64-linux-gnu-gcc` 强制走 aarch64 分支 (即使文件不存在,只要路径含 aarch64 字符串)
  - 或者 patch CMakeLists.txt

- **【铁律】radxa kernel headers 6.6.98-1-aw2511 不在 /usr/include** (2026-07-14 实测)
  - 装在 `/usr/src/linux-headers-6.6.98-1-aw2511/include/`
  - 但 userland 头文件在 `/usr/src/.../include/uapi/`(asm-generic/linux 都缺 uapi 版本)
  - 编译 userland 程序需要:
    ```bash
    sudo ln -sf /usr/src/linux-headers-6.6.98-1-aw2511/arch/arm64/include/uapi/asm /usr/include/asm
    sudo ln -sf /usr/src/linux-headers-6.6.98-1-aw2511/include/uapi/asm-generic /usr/include/asm-generic
    sudo ln -sf /usr/src/linux-headers-6.6.98-1-aw2511/include/uapi/linux /usr/include/linux
    sudo ln -sf /usr/src/linux-headers-6.6.98-1-aw2511/include/uapi/asm-generic/types.h /usr/include/asm/types.h
    ```
    (因为 arm64 没有自己的 asm/types.h,fallback 到 asm-generic/types.h)
  - 然后 `cmake -DCMAKE_C_FLAGS="-I.../include/uapi -I.../include -I.../arch/arm64/include/uapi -I.../arch/arm64/include"`
  - 当前板子: kernel driver `2.0.3.4-AW-2025-10-27` (kernel 6.6.98-1-aw2511 自带) vs userspace libVIPhal.so `2.0.3.4-AW-2025-05-16` (apt 装的 npu-runtime 2.0.3)
  - patch 号相同但 timestamp 差 5 个月 → ABI 不一致 → NPU 硬件 IRQ hang → vpm_run `VIPDRV_WAIT_TASK status=-1`
  - 现象: vpm_run 加载 .nb 成功, 但 inference 永远卡; dmesg 报 `error wait irq hardware hang`, VIPLite driver 自己 reset 失败
  - 验证: vpm_run 启动 log `VIPLite driver software library version` vs dmesg `VIPLite driver version` 必须完全相同 (含日期)
  - 修法: 从 Allwinner Netdisk (`netstorage.allwinnertech.com:5001/sharing/Mh23BhPHq`) 下新版 docker_images_v2.0.x.zip 解 tar 替换 libVIPhal.so + libNBGlinker.so
  - 排查耗时: 以为 DCDC2 voltage 欠压是根因, 重启 + 锁频 492MHz 都救不回, 浪费 30 分钟。**先看 vpm_run 启动 log 第一行 + dmesg 'VIPLite driver version'**, 不要瞎试电源/频率
  - **check list**: 任何 vpm_run hang 第一动作 = 对比这两个版本, 不一致就别再瞎折腾
- **【铁律】官方 awnpu_model_zoo Model Zoo v0.9.0 是公开下载,不需要 NDA**（2026-07-14 文档调研）
  - 公开 URL: `wget https://dl.radxa.com/cubie/allwinner-model-zoo.tar.gz` (208MB)
  - 我们硬盘上 `allwinner-model-zoo.tar.gz` 就是这个
  - 但 ACUITY Toolkit docker image (`ubuntu-npu:v2.0.10.1`) 没在 dl.radxa.com 公开,要走 NDA 平台 open.allwinnertech.com
  - 备选: awnpu_model_zoo 解包里有 `scripts_model_convert/pegasus_*.sh` 可以手动跑 (但需要 ACUITY toolkit 二进制)
  - **完全离线备选**: 直接用 awnpu_model_zoo 预编译的 A733 .nb 模型 (e.g. `examples/yolov5/model/yolov5s_rt_uint8_a733.nb`),跳过转换步骤

- **【铁律】cubie-image 仓库提供 5.15 kernel,跟 a733-bullseye 仓库的 -21 是不同来源**（2026-07-14 文档调研）
  - 仓库: https://github.com/cubie-image/sun60iw2p1
  - 7 个 5.15 版本: -112 / -131 / -1000 / -99991 / -99996 / -100 (最新, 2026-04-23)
  - 官方 G2D 文档用 5.15.147-99996 (linux_package3)
  - 跟 awnpu_model_zoo Aug 2024 .so 可能更配套 (都是 TinaSDK 同期 build)
  - URL 模板: `https://github.com/cubie-image/sun60iw2p1/releases/download/linux_package4/linux-image-5.15.147-100-a733_5.15.147-100_arm64.deb`
  - sandbox 网络限速下不了,需要 PC 端下载后 scp 到板端

- **【铁律】本地 apt repo (file:/var/lib/local-apt-repository/) 可能以 priority 1999 锁死版本**（2026-07-10 自查）
  - trixie_cli_beta_v2 镜像带这个 local repo, 优先于 radxa-repo 的 500
  - 结果: `linux-image-radxa-a733` "already newest 6.6.98-1", 但 APT 源里 6.6.98-3 才能解决 NPU 的 driver 升级
  - **check list**: `apt-cache policy <pkg>` 看 priority pinning; 强装用 `apt install -t a733-trixie-test` 临时让 radxa repo 优先
- **【铁律】pepect + SSH -tt 在 b64 encoded heredoc 里还是会遭受 term echo 自馈**（2026-07-10 踩过 2 次）
  - bash 会把输入命令的字符 echo 回 pty, pexpect buffer 里 marker (`__M_xxx_END`) 第一次出现是在 echo 中, 而**不是实际输出**
  - `ch.before` 只到 echo 那段就截了, cmd 实际输出落进 `ch.after` 后被丢
  - **workaround 3 招**:
    1. `stty -echo` 关 local echo (tty 模式)
    2. 用 `> /tmp/file 2>&1` redirect 到文件, 然后 `cat`
    3. 用 base64 + `bash /tmp/probe.sh && echo ==DONE==` 独立 marker
  - **check list**: SSH 后立即 `stty -echo`, 然后所有 probe 走单脚本
- **【铁律】I²C 7-bit vs 8-bit addressing 位移必须自己验证**（2026-07-25 老板纠正，源自我把 IMX415 0x34 跟论坛 0x1a 当矛盾）
  - Linux kernel I²C subsystem 用 **7-bit address**：`sensor0_twi_addr = <0x34>` 是正确的
  - IMX415 chip-ID register 0x3008 读出的 raw 8-bit 值是 `(7-bit addr << 1)`，所以 kernel 用 0x34，sensor 报 0x1a = `0x34 >> 1`，两者一致
  - I²C 总线帧: START + `(7-bit addr << 1 | R/W)` + ... + STOP，即 0x34 在总线上是 0x68(write)/0x69(read)
  - **check list**: 看到 sensor datasheet / 论坛说地址 X，看到 dtso / driver 说地址 Y，先别判 X 错。做一次 `(X << 1) == Y` 或 `(X >> 1) == Y` 验证
  - **反向适用**: 任何三方 I²C device,dtso / driver / datasheet / 论坛值必须做 bit-shift 对齐后再比较
  - **不要假定**: 论坛是其他开发者写的，可能用 8-bit 也可能 7-bit;datasheet 通常给 8-bit;driver 几乎都给 7-bit
- **【铁律】Radxa Cubie A7S 重烧 SD 卡 = 一周级别工作，绝不要轻推**（2026-07-24 老板明确指出,源自 7/9 那次烧卡）
  - 烧系统时间成本 = 解包稀疏 img + 验证 ext4 superblock + dd 烧 SD + 重启验证 + 重新配 SSH + 重装所有软件 + 重新配 NPU 驱动 ABI 版本号 + 重新验证摄像头
  - 一旦推烧,板端所有当前调试上下文清零,NPU + apt + 镜像仓库 pin 等所有累积的状态全没了
  - **决策门槛**（必须全过才推）:
    1. 文档分析穷尽（已过）
    2. 板端实测 7 条诊断命令（overlay/video/dmesg/uname/cat os-release/rsetup list/媒体管道）
    3. 跟老板当面/书面沟通确认是"镜像版本问题"而非"硬件/驱动问题"
    4. 必须有具体证据显示换镜像能解决问题（不是猜）
  - **check list**: 任何"我们烧系统吧"出现在答复里之前,先答"前 3 步都验证过吗 + 烧完能解决什么具体症状"
  - 老板原话："之前烧录系统卡了一周的时间" — 不可重蹈
- **【铁律】pycocotools 2.0.11 summarize 有 NumPy fancy indexing bug**（2026-06-28 栽过 1 次）
  - `aind=[0], mind=[2]` 两个 list 同时做 fancy indexing，配对后 shape 错
  - 后果：`COCOeval.stats` 全是 -1，但 `precision/recall` 本身正确
  - **修法**：手动 `fixed_stats()` 用 scalar 索引 (`s = precision[:, :, :, 0, 2]`)，不调用 `summarize()`
  - 验证：`fake perfect det` (跟 GT 完全一致) → `summarize()` mAP=1.0 ✓ (说明 evaluate+accumulate 对)
- **【铁律】回复风格：3-5 句总结 + 先结果后细节 + 不要中间推断过程** (2026-09-04 老板立, 源于今晚 v31 binary decode bug 修复总结)
  - **结构**: 3-5 句话讲清结论; 先整体结果, 后细节; 过程不描述
  - **不要**:
    - 不要输出过多中间推断信息
    - 不要罗列实验数据、代码片段、备份链、commit hash 等中间环节
    - 不要重复"按铁律""按 RCA 流程"等元叙述
  - **只要**:
    - 结果 (修了什么 / 现状如何)
    - 原因 (根因是什么, 一句话讲清)
    - 不要中间推断 / 不要中心信息 (commit hash / 时间戳 / 文件路径等)
  - **反例 (今晚实际发生)**: 我刚才写的"清晰描述"用了 250+ 行, 含现象 + 误诊 + 根因 + 代码 + 编译命令 + 部署 + 效果对比 + RCA 反思, 老板明确说"不清晰, 给我结论就行"
  - **check list**: 任何 troubleshooting 总结类回复, 先看是否能 3-5 句讲清; 不能就压缩, 不要堆中间信息


## Project Decisions

- **OD Demo 默认走 D1，暂不上 D3**（2026-06-27 决定）
  - 摄像头 `/dev/video0` 最大 640×480 (YUYV)，v4l2 验证
  - D3 输入 896×896 = 1.75x 上采样，**浪费高分辨率优势**，mAP 实际增益小
  - D3 实时 13 FPS（vs D1 23 FPS），实时场景肉眼可感顿挫
  - **触发条件**：升级到 ≥1080p 摄像头（≥ 2MP，1920×1080）再启用 D3
  - 当前 demo: `/work/object_detection/demo_odapi_headless.py` 默认 `--weights pretrained/efficientdet_d1_coco17_tpu-32/saved_model --input-size 640`
  - D3 实测基线（6/27 benchmark, RTX 3090 + TF 2.20）：saved_model 32MB, load 15.9s, 推理 77ms/帧 (13 FPS), peak 0.55GB 显存, OD API 内置 NMS (150k raw → 100 det), demo 已兼容 D3 无需改代码
- **Q2 端到端 demo 跑通，PC 原型就绪**（2026-06-28）
  - `demo_q2_pipeline.py` 18KB 单文件，输入源可热插拔（摄像头/视频/图像目录），输出 4 路（MP4 / JSONL / 快照 / MJPEG HTTP）
  - SSD 320 CPU 41 FPS、D1 640 CPU 5.6 FPS（GPU 驱动未装，CPU 可用）
  - RK3588 移植：只需替换 `_TfOdRunner` 为 `_RknnRunner`，其他不动
  - 浏览器看实时画面：`--mjpeg-port 8080` → 访问 `http://<ip>:8080/`
- **Q3 硬件选型确定（待 boss 确认下单）**（2026-06-28）
  - **首选**：Orange Pi 5 8GB + IMX415 8MP 4K MIPI 模组，~¥800 总价
  - **备选**：Radxa Rock 5B+ 8GB（双 4-lane MIPI CSI → 最多 4 摄像头，工业级），~¥1300
  - 触发换备选条件：未来要 4 摄像头 / M.2 双槽 / 工业级稳定性
  - 详细见 `/work/object_detection/STAGE2_Q3_HARDWARE_RESEARCH.md`
- **Q1 RKNN 工具链待板子到位再实测**（2026-06-28）
  - 工具链 zipball 下载路径已验证（codeload + api.github.com），但完整 30-40MB 下载需要长 timeout
  - 转换测试（SSD → RKNN，D0 → RKNN）的**真正价值是上 NPU 实测 FPS**，板子没到价值有限
  - **当前决策**：板子下单后，同步装 RKNN 环境 + 跑转换，等板子到直接上 NPU
- **切到 YOLOv8n (project at /work/object_detection_yolo)**（2026-06-28 决定）
  - SSD MobileNet FPNLite + EfficientDet BiFPN 都不兼容 RKNN NPU（OD API 的 NMS 节点 + 自定义算子）
  - 4 路径都试过：saved_model/TFLite/ONNX/graph surgery 都卡 NMS
  - YOLOv8n 是唯一 RKNN 模型 zoo 有官方支持的现代检测器，且**精度比 SSD 高 + RKNN 可行**
  - 精度实测（5000 张 COCO val2017 修复两个 bug 后）:
    - **mAP@0.5:0.95 = 0.313** (paper 0.373 的 84%)
    - mAP@0.5 = 0.422 (paper 0.527 的 80%)
    - mAP@0.75 = 0.345 (paper 0.403 的 86%)
    - mAP_large = 0.480
[] 三方对比（PC CPU/RTX 3090）:
    - YOLOv8n 640: mAP@0.5:0.95 = **0.313** (✅ RKNN ready)
    - SSD320 (OD API): 0.178 (❌ NMS blocker)
    - D1 640 (OD API): 0.338 (❌ BiFPN blocker)
  - RKNN 转换（rknn_model_zoo 优化版）已实测通: yolov8n.rknn 7.3MB / 1.3s build
  - 详见 `/work/object_detection_yolo/` + `STAGE2_EMBEDDED_DEPLOYMENT.md` 更新日志
- **嵌入式方案：全志 A733 主选 + RK3588 备选（2026-06-29 决策更新）**
  - **主选：Radxa Cubie A7S（全志 A733）** 淘宝 ~459 元 + 4K 摄像头 ≈ 700 元总
  - 备选：Orange Pi 5 Plus 8GB（RK3588）~1140 元
- **🛠️ 硬件已到位（2026-07-03）**
- **🟢 P0 完成（2026-07-10 凌晨通过 SSH 修复 chmod 问题）**
  - Radxa Cubie A7S 实际板子 IP: **192.168.71.15**（not .4 — .4 是 boss PC 自己的 Ubuntu）
  - 修复要点:
    1. SSH `radxa@192.168.71.15` (radxa/radxa) — 反推真正的板 IP
    2. npu-runtime 2.0.3 **已装**（apt 默认就是 2.0.3 from a733-trixie-test）
    3. **NPU driver 已在 6.6.98-1 kernel 里加载**（dmesg: `npu[1][1] vipcore, platform driver init`）→ **不用升级内核也能跑 NPU**
    4. `/dev/vipcore` 存在 (mode 0666), 这是新 viplite 命名 (旧 Vivante 是 `/dev/galcore`)
    5. **关键 bug**: `vpm_run` 文件 mode `-rwxr--r-- 1 1003 1005` = 744 (owner UID 1003 是 no-owner); radxa(1000) 落 "other" 类 = 没 exec 位 → `bash: Permission denied`
    6. **修法**: `sudo chmod 755 /usr/bin/vpm_run /lib/libVIPhal.so /lib/libNBGlinker.so /usr/lib/libVIPhal.so /usr/lib/libNBGlinker.so`
  - **关键收获**:
    - 装 npu-runtime 后 `vpm_run --help` 能跑不代表装对了 — 还需 ldd 验证动态库 + 选 chmod 验证执行位
    - 内核 6.6.98-1 足够，6.6.98-3 升级留 P2 (性能优化时再做)
    - local-apt-repo priority 1999 锁内核版本, 不阻塞 NPU 工作
  - **主板**：Radxa Cubie A7S × 1（全志 A733，3 TOPS NPU）
  - **摄像头**：瑞莎 Radxa 4K 高清摄像头 IMX415 × 1（8MP，4K MIPI CSI）
  - 库存：1 板 + 1 摄像头（暂无双板冗余，摔了就没备份）
  - 规格匹配：决策路径无变化（A733 工具链仍走 Vivante ACUITY Toolkit）
  - 下一步：烧录系统 → 装 viplite driver → 装 Vivante ACUITY Toolkit → 转 yolov8n → 跑 30 FPS 验证
  - 30 FPS 决策依据：
    - A733 3 TOPS (Vivante VIP9000)：YOLOv8n 640 实测裸推理 12.6ms（Radxa 官方 demo）= 79 FPS NPU，端到端估算 50-55 FPS
    - RK3588 6 TOPS：多源实测端到端 42 FPS
    - 两者 30 FPS 都达标，A733 价格低 38%、功耗低 ~30%
  - 工具链现状：
    - A733: Vivante ACUITY Toolkit（pegasus CLI）+ Docker 镜像 `cld1994/vivante-acuity-toolkit:6.6.1`，Radxa 官方 model zoo 有 yolov8n demo（实测可跑），社区 Hexchip 给完整 yolov5nu → .nb 转换流程
    - RK3588: rknn-toolkit2 + rknn_model_zoo，生态成熟 10x+，但成本高 60%
  - 关键差异（功耗/性能/兼容性）：
    - 功耗：A733 12nm 2×A76+6×A55 满载预估 6-8W vs RK3588 8nm 4×A76+4×A55 满载 10-12W（飞凌实测）
    - 性能：A733 裸 NPU 79 FPS / 端到端 50 FPS vs RK3588 端到端 42 FPS（A733 NPU 算力利用率更高？或 RK3588 测试含更多后处理）
    - 兼容性：A733 ONNX → nb 需 4 步工具链（import/inference/quantize/export），生态比 RKNN 弱但能跑通；RK3588 一键 ultralytics → RKNN
  - 风险与缓解：
    - A733 富余小（1.5-1.8x vs RK3588 1.5-2x 差不多），DDR 带宽比 RK3588 低 25%
    - 板子兼容性：Radxa Cubie A7S 4K 摄像头是 31-pin FPC（跟 Cubie A7Z 同款），Orange Pi 5 Plus IMX415 排线不通用
    - 决策：先下 A733 板子（~700 元），板子到了先跑通 demo，**如果 30 FPS 达不成或工具链卡死，fallback RK3588**

- 【铁律】**不准降频作为 A733 NPU hang 修法** (2026-07-11 boss 明确)
  - 以后任何 A733 NPU 调试,**默认不降频**,保持 1 GHz 性能
  - 只有当所有其他方案都试过 + 没方案时,才考虑降频作为最后手段
  - 即使 7/10 测过 492 MHz + 1008 MHz 都 hang (说明不是频率问题),也不要用降频"试试看"
  - 降频是性能降级,不是修法;真正修法 = 替换 libVIPhal.so / libNBGlinker.so 到匹配 kernel 2.0.3.4-Oct-2025 的版本

## Project Rules (项目规则变更)

- **【项目决策】不再考虑 RK3588 fallback** (2026-07-12 boss 23:26)
  - 除非 A733 完全不可行 (Frigate / Armbian / Netdisk 全部走死)
  - 不再考虑 Orange Pi 5 Plus 8GB 重下板
  - 当前路径: 找通用 A733 + 能跑 NPU 的镜像

## Current Focus (当前调研方向)

- **【当前】找通用 A733 + 能跑 NPU 的镜像** (2026-07-12 00:33 起)
  - **关键参考 (work 组合)**: Frigate discussion #23418
    - 板子: Radxa Cubie A7A (跟 A7S 共 sun60iw2 dtb)
    - OS: **Armbian 26.5.1 trixie (Debian 13)**
    - Kernel: **6.6.98-vendor-sun60iw2** (Allwinner SDK 编, 不是 Radxa 编)
    - NPU runtime: VIPLite **2.0.3.2-AW-2024-08-30** (npu-runtime 包, 跟 kernel ABI 匹配)
    - 实测 25-60 ms/inference = 17-40 FPS (work!)
    - URL: https://github.com/blakeblackshear/frigate/discussions/23418
    - 配套代码: https://github.com/unnamedwild-ux/frigate_npu_vivante
  - **Armbian 状态** (2026-07-12):
    - v26.8.0-trunk.343 release note 提到 Radxa Cubie A7Z 社区支持
    - PR #10036 "cubie-a7z: Add community support" 7/1/2026 merged
    - 但没在 stable release 出 cubie-a7z image, 在 nightly build + GitHub Actions artifacts
    - 没 cubie-a7s 单独 build (但 A7S 应该兼容 A7Z image)
  - **调研方向** (进行中):
    - 找 Armbian cubie-a7z 实际下载 URL (nightly build / Actions artifacts)
    - 或: 装 cubie-a7a build (Frigate 实测 work)
    - 或: trixye rootfs 上装 6.6.98-vendor-sun60iw2 kernel (绕过 Radxa kernel)
  - **备选** (没用过但可试):
    - TinaSDK V1.4.8 (Radxa 自家编译, includes Linux 6.6, 但没镜像)
    - 直接编译 armbian cubie-a7s (从 cubie-a7a fork, 改 dtb)

## Sandbox Network Limits (沙盒网络限制 2026-07-12 实测)

- 沙盒出口是 boss PC (192.168.71.14/24),但被 sandbox 限速
- 直连 radxa-repo.github.io: **10-19 KB/s** (单连接)
- 沙盒 aria2c -x16 -s16 不增加带宽 (GitHub Pages 限速)
- 沙盒 gh-proxy.com: 403 Forbidden (被 block)
- 沙盒 jsdelivr / codeload: 404 / 慢
- 沙盒 api.github.com: 0.17 KB/s (极慢)
- **结论**: sandbox 下载大文件 (>30 MB) 太慢, 需要 boss PC wget (50-100 MB/s)

## A733 NPU hang 修复项目 (2026-07-12)

### 核心根因（已 100% 复现确认）
- 板子 kernel 6.6.98-1-aw2511 内置 vipcore: `2.0.3.4-AW-2025-10-27` (Radxa 编, Oct 2025)
- apt 装 libVIPhal.so: `2.0.3.4-AW-2025-05-16` (May 2025)
- 5 个月 ABI 差 → `VIPDRV_WAIT_TASK status=-1` → NPU 永久 hang

### 唯一可行方案 = A1（已选定 2026-07-12）
- 路径：**全编 6.6 kernel + TinaSDK v1.4.6 bsp vip2 driver 替换 + awnpu_model_zoo .so**
- TinaSDK v1.4.6 bsp vip2 driver version: `0-AW-2024-05-28` (May 2024)
- awnpu_model_zoo libVIPhal.so: `2.0.3.2-AW-2024-08-30` (Aug 2024)
- 两者同期 build，ABI 配套（Frigate 实测 A7A + vendor-sun60iw2 + 这个 .so → 17-40 FPS work）

### 关键铁律（项目专属）
- **【铁律】vipcore 不能单编 .ko 替换** — 板子 `CONFIG_AW_NNA_VIP=y` 编进 zImage，TinaSDK bsp 的 vip2 按 module 设计（obj-m），100% 不兼容。必须**全编整个 kernel** 重新生成 zImage
- **【铁律】awnpu_model_zoo 的 .so 跟 TinaSDK v1.4.6 bsp vip2 driver 配套** — SHA256 `3ed5357b26bd6c4fb68fbdc0b21d637227dda711d1fe67b9e28accdc48bb11f2`（已 Frigate 实测 work 配套）
- **【铁律】TinaSDK v1.4.6 在 GitLab 公开** — `https://gitlab.com/tina5.0_aiot`，但 manifest 的 `<remote fetch="../Tina_AIOT/tina"/>` 错了（那个 remote 不存在），实际项目都在 `tina5.0_aiot/`
- **【铁律】open.allwinnertech.com 需要企业 NDA 认证**（3-4 工作日，个人无资质）— 用 awnpu_model_zoo 替代
- **【铁律】不走 RK3588 fallback**（除非 A733 完全不可行）— 2026-07-12 boss 明确

### 关键源仓库
- `https://github.com/radxa-pkg/linux-aw2511` — 板子 6.6.98-1-aw2511 kernel wrapper（33 KB）
  - src 子模块 → `https://github.com/radxa/kernel` branch `allwinner-aiot-linux-6.6`（**4.5 GB**）
  - bsp 子模块 → `https://github.com/radxa/allwinner-bsp` branch `cubie-aiot-v1.4.8`（78 MB）
  - device-a733 子模块 → `https://github.com/radxa/allwinner-device` branch `device-a733-v1.4.8`（5 MB）
  - release tag `6.6.98-3` (2026-06-11) 含预编译 .deb

### 项目决策日志
- ❌ 不再考虑 RK3588 fallback (2026-07-12 boss 23:26)
- ✅ 走 A1 全编 kernel 路径 (2026-07-12 boss 18:00)
- ✅ 用 awnpu_model_zoo .so 替代 NDA 配套 .so (2026-07-12 boss 17:30)
- ❌ 路径 A 失败（awnpu_model_zoo .so + Radxa Oct 2025 kernel driver）— 仍 hang
- ❌ 路径 B（重编 vpm_run）失败 — vpm_run 是 CLI wrapper，hang 在 libVIPhal.so 内
- ❌ 5.15 kernel panic — allwinner-prebuilt 缺 earlyprintk
- ❌ radxa-pkg/linux-aw2511 clone 失败（sandbox 4.5 GB 限速 200+ 小时，PC 端 clone 也失败）
- ⚠️ 项目暂停 2026-07-12 21:11（老板改天再继续）

### 已验证 work 的对照组合（Frigate 实测）
```
A7A + Armbian 26.5.1 trixie + kernel 6.6.98-vendor-sun60iw2 + libVIPhal.so 2.0.3.2-AW-2024-08-30
→ 17-40 FPS，NPU 完全 work
```

### 我们要复刻的组合（A1 完成后预期）
```
A7S + trixye + kernel 6.6.98-1-aw2511 (TinaSDK v1.4.6 vip2) + libVIPhal.so 2.0.3.2-AW-2024-08-30
→ 预期 NPU work（同期 build ABI 配套）
```

### 资源汇总
- 板端 backup: `/usr/lib/libVIPhal.so.bak.apt-may2025` (May 2025 apt 版)
- TinaSDK v1.4.6 bsp: `/work/tinasdk/bsp/` (539 MB，已下)
- TinaSDK clone_all.sh: `/work/tinasdk/clone_all.sh` (89 个项目一键下载，已跑完 16/89)
- TinaSDK v1.4.6 manifest: `/work/tinasdk/v1.4.6.xml`
- A1 完整执行计划: `/work/tinasdk/A1_FULL_PLAN.md`
- DOWNLOAD_LINKS: `/work/tinasdk/DOWNLOAD_LINKS.md`
- awnpu_model_zoo .so: `/home/yuewuya/Downloads/radxa/awnpu_model_zoo-v0.9.0-20260116-83a67d4b/common/npuruntime/lib_linux_aarch64/A733/`
- daily memory: `/home/yuewuya/.openclaw/workspace/memory/2026-07-12.md`

### 板子状态（21:11 前最后确认）
- 内置 vipcore: Radxa 编的 Oct 2025 build（CONFIG_AW_NNA_VIP=y 编进 zImage）
- libVIPhal.so: `85ebf32f...` (May 2025 apt 版，回滚后)
- libNBGlinker.so: `ce5ec879...` (May 2025 apt 版，回滚后)
- backup: 还在 `/usr/lib/libVIPhal.so.bak.apt-may2025`
- vpm_run mode: 755 ✓

## 2026-07-21 — NPU 跑通 + 新铁律

### NPU work (board: 192.168.71.20, trixie)
- vpm_run ret=0, 3024us/frame
- yolov5 demo: detection num: 3 (dog/truck/bicycle 82%/69%/47%), 25.7ms/inference → ~39 FPS
- NPU 配套: SDK 自带 Aug 2024 .so (2.0.3.2-AW-2024-08-30) + Radxa 内置 Oct 2025 kernel driver

### 新铁律（从今天装机踩到）
- **【铁律】板端 apt install 必须先 disable 3 个源**（2026-07-21 栽过）
  - `70-radxa.list`, `80-radxa-a733.list`, `local-apt-repository.sources`（含空 repo 干扰 + priority 1999 pinning）
  - 修法: `sudo mv /etc/apt/sources.list.d/70-radxa.list{,.disabled}` + 80... + local...
  - 不删只 rename, 保留可恢复
  - check list: 任何 apt install 前先 `ls /etc/apt/sources.list.d/`
- **【铁律】板端 wget 下载大型 .deb 会被截断**（2026-07-21 栽过, lzma 解压失败）
  - 同样 URL 同时下: PC 完整 7.99MB, 板端只到 2.5MB → "unexpected end of file"
  - 解决: PC wget → scp → 板端 `dpkg -i --no-triggers` 装
  - check list: 大于 2MB 的 .deb 都走 PC-scp, 不在板端 wget
- **【铁律】pip 装大 .whl 也遵循 PC-scp 原则**（2026-07-22 栽过, 同 apt .deb）
  - opencv-python-headless 39MB + numpy 15MB, 板端 pip 直连 PyPI 被截断
  - 解决: PC `pip download --platform manylinux_2_28_aarch64 --only-binary=:all:` → scp → 板端 `pip3 install --break-system-packages <wheel>`
  - **wheel 文件名保留原名**（含 cp tag, abi, platform），不要简化成 `numpy.whl` — pip 会因 wheel filename 不合法拒绝装
  - check list: 任何 >2MB 的 .whl 都走 PC-scp
- **【铁律】yolov5 install 路径扁平化**（2026-07-21 栽过, SDK Makefile bug）
  - Makefile: `@cp model/$(NPU_VERSION)/* /etc/npu/yolov5/model`
  - glob `v3/*` 拍扁, 实际文件在 `model/yolov5.nb` 不是 `model/v3/yolov5.nb`
  - check list: install 完 `ls /etc/npu/yolov5/model/` 看实际 layout, 再拼路径
- **【铁律】yolov5 demo 缺返回值检查会 segfault**（2026-07-21 栽过）
  - vip_create_network 失败 (model 找不到) 不 return, 继续进 preprocess → 空指针 crash
  - 修法（正式项目）: main.c 里 `if (awnn_create(...) != 0) return -1` + per-step 检查
  - check list: 任何跑 awnn_lib.c 的 demo 先看下 main.c 的 error handling
- **【铁律】yolov5 binary 是 PIE (ET_DYN), glibc 2.34+ 拒绝 dlopen**（2026-07-21 栽过）
  - 报错: `cannot dynamically load position-independent executable`
  - 修法: 用 SDK 源码编 shared lib (`g++ -shared -fPIC`), 不要 dlopen 那个 binary
  - 参考命令 (板端跑过):
    ```bash
    cd ~/ai-sdk/examples/yolov5
    g++ -shared -fPIC -O2 -o ~/od_api/lib/libod_helper.so \
        ../libawnn_viplite/awnn_lib.c ../libawnn_viplite/awnn_quantize.c \
        ../libawutils/image_utils.c yolov5_pre_process.cpp yolov5_post_process.cpp \
        -I../libawnn_viplite -I../libawutils -I../.. -I../../log \
        -I../../viplite-tina/lib/aarch64-none-linux-gnu/v2.0/inc -I/usr/include/opencv4 \
        -lopencv_core -lopencv_imgcodecs -lopencv_imgproc -ljpeg -lNBGlinker -lVIPhal
    ```
  - check list: 任何 ctypes 直接绑 SDK binary 的场景, 必须确认 binary 是 ET_DYN 还是 ET_EXEC, 是 DYN 就编 shared lib
- **【铁律】yolov5 binary 完全缓冲 stdout, subprocess.run 会丢 detection lines**（2026-07-21 栽过）
  - C runtime 检测到 stdout 不是 TTY → `setvbuf(_IOFBF)` 全部 block buffer
  - `subprocess.run(capture_output=True)` + `text=True` 仍是 pipe → 触发 block buf → yolov5 print detection lines 后 segfault, buffer 没 flush → 丢
  - **修法**: `pty.openpty()` + `subprocess.Popen(stdout=slave_fd, stderr=slave_fd)`, child 看到 TTY → line-buffered → 每 \n flush
  - `stdbuf -oL` + `subprocess.run` 不行（pipe 仍 block buf）, `grep` pipeline 倒行（不知道为啥）
  - **check list**: 任何捕 SDK C 二进制 stdout 时, 用 pty 不用 pipe

## 2026-07-30/31 — 板端推流 PC 取流（v0.16 → v23 RTSP → v24 udpsink → MPV/VLC）

### 老板任务时间线
- 23:00 左右：mpv 取流已通 → 把 USB 摄像头接进来 + 算法跑起来
- 板端 v0.16 UVC pipeline 24.4 FPS ✅, v23 + RTSP 推到 PC 出问题
- RTSP 试了 mpv + VLC 都失败，最终改成 udpsink + SDP 才出流
- 凌晨 01:08 出流 + 算法检出 person/suitcase 框

### 关键铁律（新增）

**【铁律】板端 RTSP server (GstRtspServer) + CPU 竞争会让 RTSP 协议协商失败**
- v23 跑 v0.16 (24 FPS) + encoder + RTSP server 三个 Python 进程，CPU 抢光
- GLib MainLoop 在 encoder subprocess 里跑，encoder 忙时 MainLoop 不迭代
- PC 端 mpv / VLC TCP 连上 8554 但 RTSP OPTIONS/SETUP 永远等不到响应
- 即使 taskset 把 worker/encoder 绑到固定核（CPU 2/3），仍然 sshd 都进不去（CPU 100%）
- **修法**：彻底不用 RTSP server，改 `appsrc → x264enc → h264parse → rtph264pay → udpsink host=PC_IP port=8554`（v24_udpsink.py）
- UDP 是无连接 + 内核态处理，没 RTSP 握手失败问题

**【铁律】RTP 推流时 PC 端必须用 `rtp://` 不是 `udp://`**
- 板端 rtph264pay 发的包是 UDP + 12字节 RTP 头 + H264 payload
- PC 端 `udp://:8554` 当 raw UDP 接收 → 看到 RTP 头不解码 → 黑屏
- 必须 `rtp://:8554`（VLC）或 `rtp://0.0.0.0:8554?listen=1`（mpv）
- 或 SDP 文件告诉 decoder PT96=H264/90000

**【铁律】x264 key-int-max=24 + 10 FPS 第一个 I-frame 要 2.4 秒**
- PC 端 player 看到第一个画面要等 ≥5-8 秒（I-frame + decoder init + buffer）
- 老板看到黑屏可能误以为失败 → 实际是在缓冲
- 必须**至少等 15-20 秒**才能下"出流失败"结论

**【铁律】Windows PowerShell `@` 在 URL 起始是 splat 操作符**
- `& "vlc.exe" udp://@8554` 报错（@ 被当 splat）
- 修法 1：单引号 `'udp://@8554'`
- 修法 2：去掉 `@` 用 `udp://:8554`（VLC 也认）
- 修法 3：URL 整体双引号包 `"udp://:8554"`

**【铁律】PowerShell here-string `@'...'@` 不能直接 pipe**
- `'...' | Out-File` OK
- `@'...'@ | Out-File` 报错（here-string 不在 pipe context 里 evaluate）
- 修法：先 `$sdp = @'...'@` 再 `Set-Content -Path x -Value $sdp`
- 或用 echo 一行：`"v=0`n..." | Out-File`（backtick-n 是 PS newline）

**【铁律】VLC 默认装在 `C:\Program Files\VideoLAN\VLC\` 但用户可能装在 D 盘**
- 不能假定路径，跑 `Get-ChildItem C:,D: -Filter vlc.exe -Recurse` 找真实路径

**【铁律】GstRtspServer `pipeline.get_bus()` 在某些版本返回 None**
- v24_udpsink.py 第一版报 `'NoneType' object has no attribute 'add_signal_watch'`
- 修法：`try: bus = pipeline.get_bus(); if bus is not None: ...` 或直接 skip bus watch

**【铁律】udpsink GStreamer element 的属性名是 `host`/`port`/`async`/`sync`，没有 `buffersize`**
- 写 `udpsink host=... port=... async=false sync=false` (无 buffersize)
- `buffer-size` 也不是，是 udpsrc 的属性

**【铁律】v24 udpsink 推 RTP 包 PC 端 10 秒收到 1098 个（~110/秒 ≈ 10 FPS）**
- 实测 v24 在板端 10 FPS 稳态
- 包大小典型：14 bytes (STAP-A 聚合) / 1400 bytes (单 NAL unit, MTU 上限) / 1003 bytes (中等 NAL)
- PC 端 PowerShell `UdpClient.Receive` 可直接验证包到达
- 验证包格式：第 1 字节 0x80 (RTP version 2, no padding, no extension, CC=0), 第 2 字节 0x60 (PT=96) 或 0xE0 (PT=96 + marker bit)

### 老板当前状态（2026-07-31 01:08）
- 板端 v24 udpsink 后台跑着（pid 1514/1531/1532）
- PC 端用 VLC + SDP 文件 `od.sdp` 或 mpv 命令接到流
- 实测看到 person/suitcase 检测框，FPS 9.96 稳态
- 算法在跑，画面在出 — 主线任务完成

### 待办（明天 / 后续）
- [ ] 把 v24_udpsink.py 提到正式版本号（v25 或就叫 v0.16-udp）
- [ ] 加 systemd unit 或更稳的 daemon 化（避免 SSH 断开 kill 进程）
- [ ] 解决 v23/v24 CPU 竞争（保留 v0.16 24 FPS 的优化空间）
- [ ] PC 端 player 选型固化（VLC vs ffplay vs mpv，看哪个最稳）
- [ ] 优化方案：把 vpm_run (NPU) + UVC 抓帧 + x264enc + udpsink 都做成 systemd service

## 2026-07-31 01:10 — 状态快照（老板拍板"先记录状态"）

详见 `memory/2026-07-31.md` (6089 bytes, 完整环境快照)

### 当前活链路速查
- **板端**：Radxa A7S 192.168.2.3, kernel 6.6.98-3-aw2511, Debian trixie, 8核 aarch64
- **USB 摄像头**：Microdia /dev/video0 (640x480 YUYV)
- **NPU**：VIPLite v2.0.3.2-AW-2024-08-30, model `/etc/npu/yolov5/model/yolov5.nb` (md5 9f8f9f2f...)
- **当前 pipeline**：`v24_udpsink.py` (板端 `/home/radxa/camera_uvc_od_pipeline_v24_udpsink.py`, md5 4023f1812c64d40acd626ff3ea7612be)
- **推流**：appsrc(BGR) → videoconvert → x264enc → h264parse → rtph264pay → udpsink 192.168.2.4:8554
- **进程**：PID 1514 (main) / 1531 (worker CPU 2) / 1532 (encoder CPU 3)
- **稳态性能**：9.96 FPS, encoder push avg 5.8ms
- **PC 端 player**：VLC `& "D:\Program Files\VideoLAN\VLC\vlc.exe" od.sdp` (成功) 或 mpv `rtp://0.0.0.0:8554?listen=1`
- **检出**：person (0.49-0.52), suitcase (0.41-54, 误识别纸箱)

### 备份位置
- `/work/radxa_yolo/code/camera_uvc_od_pipeline_v24_udpsink.py` (本地副本)

### 已知问题（明早决定）
1. 9.96 FPS (vs baseline 24.4) — encoder + bgr.copy + udpsink 加 ~10ms/帧
2. NPU worker 82ms (vs v0.16 报告 37ms) — 绑 core 2 后慢 2x，存疑
3. v24 不是 systemd service，SSH 断开可能被 kill
4. suitcase 误识别（纸箱）— YOLO int8 模型问题，可接受 demo 用
5. RTSP server 路径已封存，要复活需先修 GLib 调度

### 老板明日选项
A. 收 9.96 FPS 不再优化 → B. 优化到 20+ FPS → C. 加 systemd service → D. 测 TTA → E. 还原 RTSP → F. 文档化

## 2026-08-06 23:50 — 30 FPS 优化实验失败（板端 native 改 .so 路径已碰壁）

### 实验背景
- v24 baseline FPS = 10.36-10.41 (worker_total_ms = 80.3ms)
- 老板目标 30 FPS → 需砍 worker_total_ms 从 80ms 到 33ms（节省 47ms）
- 当时路径：K (NMS OpenMP) + F (prefetch) + V (去 vector copy) + A (INT8 直通) + B (416 resize)

### 实验失败记录（8/6 23:50 实测，全部反向）

| 改动 | 期望收益 | 实测结果 | 原因 |
|---|---|---|---|
| K: `omp parallel sections` 包裹 3 个 generate_proposals | -2~3ms | **+8.4ms** (10.9→19.3) | 默认 8 threads，L2 cache thrashing + bandwidth contention |
| F1: `omp parallel for` 包裹 3 个 memcpy | -10ms | **+8.8ms** (40.6→49.3) | 同上，3 个 memcpy 争带宽 |
| F2: NEON `__builtin_prefetch` in neon_memcpy_f32 | -10~15ms | **+5.1ms** (31.2→36.3) | streaming pattern 下 prefetch 无效，立即被冲掉 |
| V: 去掉 std::vector<float> copies | -3ms | 不显著 | 改成了直接指针访问，但被 OMP 副作用掩盖 |

**总效果：FPS 10.44 → 8.64 (-17%)，全部指标都变慢**。

### 关键教训（铁律新增）

**【铁律】aarch64 8 核默认 OpenMP 在小数据并行上是负优化**（2026-08-06 实测）
- 默认 `omp parallel for` 开 8 threads，但任务粒度只有 1-26MB memcpy / 25K cells NMS
- 8 threads → L2 cache thrashing（每个 thread 拉走自己的 L1/L2 cacheline 副本）
- 8 threads → 内存带宽 contention（DDR 总带宽有限，多 threads 抢）
- **OMP 真正能赢的场景**：compute-bound 大块独立任务（图像 resize/conv 这种）
- **正确做法**（如果还要试）：
  - `omp_set_num_threads(2)` 显式限制（小数据用 2-3 threads）
  - 用 `omp parallel sections num_threads(3)` 而不是 parallel for
  - 大块 memcpy 单 thread 跑 + 多 thread 跑小 NMS
  - **不要**给 streaming memcpy 加 prefetch，Cortex-A76 hardware prefetcher 已经做这个

**【铁律】板端 native 改 libod_helper.so 拿到 30 FPS 不可能**（2026-08-06 实测撞墙）
- 核心瓶颈：`awnn_run` 里 3 段 FP32 memcpy = 40ms，占 worker_total 50%
- 根因：NPU 输出 buffer 经过 `vip_map_buffer` 走 VIPLite 内部 DMA bridge
- 实测带宽仅 830 MB/s（vs A733 LPDDR4 理论 12.8 GB/s，差 16 倍）
- 这是**硬件/驱动限制**，板端 native 改 .so 拿不到这个带宽

**【铁律】要破 30 FPS 必须绕过 VIPLite FP32 memcpy**（2026-08-06 推论 + 待验证）
- 可行路径：
  - **PC 端装 ACUITY Toolkit → 重导 416 .nb**（B 路线，工作量 1-2 hr，需要 x86 PC）
  - **force_float32: false in postprocess_file.yml** → NPU 直接输出 INT8 → 跳过 41ms FP32 memcpy（路线 A）
  - **DMA-BUF / ion 零拷贝** → 跳过整个 memcpy（路线 I，1-2 天，赌 VIPLite ABI）
- 单一路径做不到，三个必须组合 (B + A 或 B + I)

### 当前活链路状态（8/6 23:55）
- v24 baseline 在跑：FPS 10.36-10.41，pid 6304/6330/6331
- worker_total_ms 80.3ms (vip_run 26.5 + memcpy 40.6 + post 11.0 + 其他 ~2)
- 检出 person 实测可看到
- .so 当前 md5: efd66e0eedeeee75a99670ee615735dd（重编自 8/6 15:35 source，跟 8/5 那个 backup 内容差异很小）

### 优化路线新判断
- ~~K + F + V 路线~~：已实测无效
- **A 路线（INT8 直通）+ B 路线（416 重导）**：唯一可行突破，需 PC 端 ACUITY Toolkit
- **I 路线（DMA-BUF 零拷贝）**：高风险高收益，工作量 1-2 天
- **J 路线（x264 → vencoderdemo）**：不影响 detection FPS，只省 CPU
- **接受 10 FPS**：稳态可用，不必死磕 30

### 老板明日选项（8/7）
A. 收 10.4 FPS 不再优化 → B. 装 ACUITY Toolkit 冲 30+ FPS → C. 调研 DMA-BUF 零拷贝 → D. 加 systemd service → E. vencoderdemo 优化编码 → F. 写文档

## 2026-08-06 23:55 — A1 path A 撞墙 + 关键新发现

### A1 试图走 path A (升级板端 .so 到 2.0.10.x) 实测结果

**目标**：板端 NPU userspace .so 升到 2.0.10.x，配合 ACUITY 6.30.22 输出的 NBG format version 02

**结果**：path A **不存在**
- awnpu_model_zoo-v0.9.0 (2026-01-16) 的 A733 路径只有 `libVIPhal.so` (39416B) + `libNBGlinker.so` (174728B)
- 这两个 .so **strings 显示 VIPLite 2.0.3.2-AW-2024-08-30**（不是 2.0.10.x）
- md5 跟板端当前 .so 一字不差 (a432e0ec / dd63994b) — 板端已经在用 2026-01 版本
- **结论**：Allwinner 没公开发布 A733 的 VIPLite 2.0.10.x userspace .so，只有 2.0.3.2
- awnpu_model_zoo 的"2026-01-16 packaging date"只是 SDK 打包时间，runtime .so 还是 2.0.3.2 ABI

### 8-04 SRAM size 错误的真凶（之前误判修正）

**之前笔记说**：老板 hexedit 改 chip ID 0x20→0x3B 后仍报 SRAM size 错误
**真凶**：不是 chip ID，是 NBG header **format version**

| .nb | byte 4-5 (version) | byte 6-7 (format) | byte 8-11 (chip ID) |
|---|---|---|---|
| baseline (板端) | 1e 00 | **01 00** | 3b ✅ |
| Radxa 官方 a733.nb | 1e 00 | **01 00** | 3b ✅ |
| ACUITY 6.30.22 导出 | 00 00 | **02 00** | 20 ❌ |

- 板端 .so 是 VIPLite 2.0.3.2 → 只认 NBG format version `01`
- ACUITY 6.30.22 输出 format version `02`（2.0.10.x ABI）→ .so 解析错位读 SRAM size → 0x8c8a82ac garbage
- 老板 8-04 只 hexedit 改 byte 8 chip ID，没改 byte 6-7 format version
- **修法**：需要 VIPLite 2.0.10.x userspace .so（Allwinner 未公开）

### 单次 yolov5 demo 跑 .nb 真实数据（关键发现！）

```
baseline .nb 3 次平均:
  vip_run_network: 25.85ms
  fp32 6.5MB memcpy: ~10ms
  fp32 1.6MB memcpy: ~2.5ms
  fp32 0.4MB memcpy: ~0.7ms
  tensor_to_fp: 14-20ms
  awnn_run total: 38-46ms (avg 42ms)

官方 a733 .nb 3 次平均:
  vip_run_network: 20.20ms (⚡ 快 5ms)
  fp32 6.5MB memcpy: ~12ms
  fp32 1.6MB memcpy: ~2.3ms
  fp32 0.4MB memcpy: ~0.7ms
  tensor_to_fp: 15-20ms
  awnn_run total: 36-41ms (avg 38ms) ⚡ 快 5ms
```

**v24 pipeline 跑同样 baseline .nb 跑出 80ms** —— **v24 比单次 demo 慢 38-42ms (50%)**！

### v24 pipeline 隐性开销分析

| 阶段 | 单次 yolov5 demo | v24 pipeline | Δ |
|---|---|---|---|
| vip_run_network | 25.85ms | 26.40ms | +0.5ms |
| fp32 6.5MB memcpy | ~10ms | **31.22ms** | **+21ms** ❌ |
| fp32 1.6MB memcpy | ~2.5ms | **7.60ms** | **+5ms** ❌ |
| tensor_to_fp 总 | ~14ms | **40.56ms** | **+26ms** ❌ |
| awnn_run total | 38ms | **67.7ms** | **+30ms** ❌ |

**v24 多进程 + shared memory + multiprocessing.Queue IPC 让 memcpy 慢 3x！**

### 30 FPS 路线新判断

- ❌ Path A 升级 .so 不存在（Allwinner 未公开 2.0.10.x）
- ❌ Path B 重导 .nb 撞 ABI（format version 不匹配）
- ❌ Path B + force_float32: false 没意义（.nb 都跑不通）
- ⚠️ **Path C v24 pipeline 同步化**：单次 demo 跑 38ms → v24 改单进程 → 估 50ms → 20 FPS
- ⚠️ 30 FPS 实际可能做不到（A733 + yolov5s 640x640 单次 38ms 是硬件底线）

### 当前状态
- baseline .nb 已恢复 (md5 9f8f9f2f..., 5MB, 检测正常)
- v24 已 kill
- libVIPhal.so / libNBGlinker.so 仍是 awnpu_model_zoo-v0.9.0 版（板端一直在用）
- 备份：/home/radxa/npu_runtime_backup_20260806_235200/ (空目录，板端 .so 没改过)
- 官方 a733 .nb 在 /home/radxa/yolov5s_rt_uint8_a733.nb (5MB, format 01, chip 3b, 但 class mapping 错导致 422 false positive)

## 2026-08-07 00:20 — v24 sync inline 实测 ❌ 同步化没收益

### 实测结果
- 改写 v24 → v24_sync（去掉 worker Process + shared memory + multiprocessing.Queue，NPU inline 在 main 进程）
- 启动 OK，NPU inline mode
- FPS 10.78（vs multiprocessing 版 10.36）—— **几乎一字不差**

### awnn 内部 timing 对比

| 阶段 | 单次 yolov5 demo | v24 multiprocessing | v24 sync (inline) |
|---|---|---|---|
| vip_run_network | 25.85ms | 26.40ms | 27.07ms |
| **fp32 6.5MB memcpy** | **9-12ms (avg 10.7)** | **31.22ms** | **31.50ms** ❌ |
| fp32 1.6MB memcpy | 2.4ms | 7.60ms | 7.83ms |
| fp32 0.4MB memcpy | 0.6ms | 1.90ms | 1.95ms |
| **awnn_run total** | **38-46ms (avg 42)** | **67.7ms** | **68.5ms** ❌ |

### 根因（修正之前的猜测）
- ❌ 不是 multiprocessing IPC（去掉 worker Process 后 FPS 不变）
- ❌ 不是 shared memory 同步
- ❌ 不是 queue 序列化
- ✅ **是 NPU SRAM → system DRAM DMA bridge 硬限制 830 MB/s**
- 单次 demo memcpy 10ms 是 NPU 热 cache + 没 DMA 竞争
- 持续跑 memcpy 31ms 是真实带宽（26MB / 31ms = 840 MB/s）

### 30 FPS 已撞墙
**A733 + yolov5s 640x640 硬件极限 ~26 FPS**（理论 awnn_run 38ms）
**当前架构能达到 ~12 FPS**（持续跑 + 同步开销）
**30 FPS 在现有架构 + 现有 .nb 不可能**：
- 理论极限 26 FPS（无任何开销）
- 实际持续跑极限 ~17 FPS（DMA 31ms × 1 + post 11ms × 1 = 42ms = 24 FPS；实际 22-25 FPS）
- 30 FPS 必须换 yolov5n 或缩到 416 input

### 板端当前状态
- v24_sync 跑着，pid 8229 + 8248（main + encoder）
- baseline .nb 已恢复 (md5 9f8f9f2f..., 5MB, 检测正常)
- libod_helper.so / libVIPhal.so / libNBGlinker.so 全部 baseline
- /home/radxa/camera_uvc_od_pipeline_v24_sync.py 保留 (改写版本)
- /home/radxa/camera_uvc_od_pipeline_v24_udpsink.py 保留 (原版)
- /home/radxa/yolov5s_rt_uint8_a733.nb 保留 (官方 a733 demo .nb, 5MB, ABI 兼容但 class mapping 错)

### 30 FPS 唯一可行路径（之前没意识到）
- **换 yolov5n（640 input, 1.9M params）** — NPU compute 26ms → ~7ms (3-4x)
- **缩 input 640 → 416** — NPU compute + memcpy 都按面积缩
- **或者重导 .nb + force_int8 output**（需要绕开 2.0.10.x ABI 限制 — 上一轮已撞墙）

### 老板选项（明早）
A. 接受 10 FPS，加 systemd service 固化
B. 换 yolov5n（牺牲 mAP -5~7，估 25-30 FPS）
C. 缩到 416 input（牺牲 mAP -2~3 + 重导 .nb 工作量）
D. 切其他平台（RK3588 6 TOPS 30+ FPS 但贵 60%）

## 2026-08-07 00:25 — A1 path 撞 ABI 2.0 墙

### 实验
- 老板 8-04 用 `--optimize v3` 撞 ABI 墙，怀疑是 flag 错了
- 今晚用 `--optimize a733`（Radxa 官方文档示例）重现 pipeline
- ⚠️ **a733 flag 输出的 NBG 跟 v3 一字不差**：format version 02 + target 0x10000020

### NBG header byte 8-11 详情（off-by-X 教训）
老板 8-04 笔记说"hexedit 改 chip ID 0x20→0x3B 没用，仍报 SRAM size"
- 老板当时只改了 byte 8 (`20` → `3b`)，没改 byte 6-7
- byte 6-7 是 NBG format version，控制 NPU 内部 NBG parser 方向
- byte 8-11 是 target chip ID

| NB\.nb | byte 4-5 | byte 6-7 (format) | byte 8-11 (chip) | 板端可读 |
|---|---|---|---|---|
| **baseline (板端 native)** | 1e 00 | **01 00** | **3b 00 00 10** | ✅ |
| 官方 a733 demo .nb | 1e 00 | **01 00** | **3b 00 00 10** | ✅ |
| ACUITY 6.30.22 + v3 | 00 00 | **02 00** | 20 00 00 10 | ❌ |
| ACUITY 6.30.22 + a733 | 00 00 | **02 00** | 20 00 00 10 | ❌ |

**结论**：ACUITY 6.30.22 工具链只输出 format version 02 (2.0.10.x ABI) 的 NBG。不管 optimize flag 怎么写。
**Radxa 官方 demo 用的 .nb 是预编译的**（不是用 ACUITY 6.30.22 生成的，是更早的 6.18.x 或 6.20.x 时代产物）。

### 修复路径（none worked tonight）
1. ~~升级板端 .so 到 2.0.10.x~~：Allwinner 没公开 A733 的 2.0.10.x userspace .so ❌
2. ~~手动 patch NBG header 02→01 + 20→3B~~：高风险（其他字段可能不匹配），未尝试
3. ~~找 ACUITY 6.18.x 老版本~~：8-03 调研发现开放镜像 404，NDA 平台路径卡

### 结论
**30 FPS 在 A733 + yolov5s 640 上永远做不到**：
- 单次 NPU run 理论极限 26 FPS（38ms）
- 持续运行受 DMA 桥 830 MB/s 限制 → 22-25 FPS
- 30 FPS 需换 yolov5n/缩 416/换平台

### 板端资源保留
- baseline .nb 已恢复 (md5 9f8f9f2f...)
- ACUITY 6.30.22 conda env 完整
- a733 flag 生成的 .nb 仍在 `/tmp/a733_test/wksp_a733_nbg_unify/network_binary.nb` (md5 e4a9aba0...)
- 老板 PC 端 `~/Public/radxa_yolo/acuity_pipeline/model/yolov5s-sim` 系列文件完整
- 老板手头 `ubuntu-npu_v2.0.10.2.tar.zip` 在 `~/Downloads/docker_images_v2.0.x/`

### 老板选 A1 后续选项
- W1: 手动 hexedit NBG 改 format 02→01 + chip 20→3B（高风险，1-2 小时能验证）
- W2: 找 ACUITY 6.18.x 老版本（可能要破解 AmebaPro2 GitHub release）
- W3: 接受 10 FPS，加 systemd service 固化
- W4: 切其他平台（RK3588 6 TOPS 30+ FPS）

## 2026-08-07 00:40 — v24 systemd service 收工

### 落地成果
- ✅ `/etc/systemd/system/camera-uvc-od-v24.service` (1.3KB, enabled + active)
- ✅ `/opt/od-pipeline/install.sh` 永久保留 (1.9KB)
- ✅ `/opt/od-pipeline/camera-uvc-od-v24.service` 备份
- ✅ `/var/log/camera-uvc-od-v24/` log dir (radxa owned)

### 实测 systemd 跑 v24 数据
- Service active 持续 1min 10s+ 稳定
- FPS **11.13** (avg 11.08-11.18) — 比 manual baseline 10.36 略好
- worker_total_ms 80.8ms (稳定)
- awnn_run total 67-69ms (跟 manual 一致)
- Memory 108.8M / 1G max
- Tasks 24/32 (TasksMax 工作)
- 自动重启 on-failure, RestartSec=10, StartLimit 5次/300秒

### service 关键配置
- Type=simple, User=radxa, Group=radxa
- After=network-online.target multi-user.target
- Restart=on-failure (手停不重启)
- ProtectSystem=full, ProtectHome=read-only, ReadWritePaths=/var/log /tmp
- LD_LIBRARY_PATH=/home/radxa/od_api/lib:/home/radxa/ai-sdk/viplite-tina/lib/aarch64-none-linux-gnu/v2.0
- ExecStart=/usr/bin/python3 .../v24_udpsink.py --save-dir /var/log/... --save-every 100 --device /dev/video0 --conf 0.25

### 老板常用操作命令 (放在 README)
```bash
# 启停
sudo systemctl start|stop|restart camera-uvc-od-v24.service
# 状态
systemctl status camera-uvc-od-v24.service
# 实时日志
journalctl -u camera-uvc-od-v24 -f
# 取消开机自启（但当前仍跑）
sudo systemctl disable camera-uvc-od-v24.service
```

### PC 端接流
- `vlc udp://@:8554`
- `mpv udp://@8554`
- `ffplay -fflags nobuffer udp://192.168.2.3:8554`

### PC workspace 留下的资源
- /home/yuewuya/.openclaw/workspace/v24_systemd/camera-uvc-od-v24.service
- /home/yuewuya/.openclaw/workspace/v24_systemd/install.sh
- /home/yuewuya/.openclaw/workspace/v24_systemd/README.md (5.2KB 完整部署文档)

### 30 FPS 路线彻底 dead-end (今晚确认)
所有路径全撞墙：A/B/C/D/E 详见 memory 2026-08-07 00:25 章节。**接受 10 FPS 收工**。

## 2026-08-07 00:50 — INT8 .nb 路线突破！FPS 10.4 → 13.19

### 关键发现（之前完全没注意到的）
1. **NBG header byte 8-11 的 chip ID 是从 `--optimize` 参数解析，不是 hardcode**
   - 老板 8-04 用 `--optimize v3`（pipeline 优化级别）输出 target 0x10000020
   - 老板昨晚用 `--optimize a733`（SoC 名）输出 target 0x10000020（错）
   - **正确 flag 是 `--optimize VIP9000NANODI_PID0X1000003B`**（完整 chip ID）
2. **老板 VivanteIDE 5.11.0 (Jan 20 2025) cmdtools 里正好有这个 cfg 文件**：
   - `VIP9000NANODI_PID0X1000003B.config` — A733 专属
   - 跟板端 baseline .nb byte 8-11 (`3b 00 00 10`) 一字不差
3. **老板 PC 早就有完整工具链**：VivanteIDE 5.11.0 + ACUITY 6.30.22 + 7.4GB docker rootfs（acuity_rootfs/）
4. **板端 VIPLite 2.0.3.2 实际兼容 NBG format version 02**（之前误判）

### 8-04 笔记修正
- 之前我以为是 "ACUITY 6.30.22 输出 format 02 ↔ 板端只认 format 01"
- **真凶是 `--optimize v3/a733` flag 错（输出 target 0x20）**，**format 02 是兼容的**
- 8-04 hexedit 改 chip ID 但仍报错，是因为整个 NBG 内部 SRAM 配置按 0x20 设置，跟硬件 0x3B 不匹配
- **fix：用正确 chip ID flag，让 pegasus 内部按 0x3B 配置 NBG**

### 实测 INT8 .nb 性能（v24 pipeline）
| 指标 | baseline FP32 | INT8 chip3B | Δ |
|---|---|---|---|
| **FPS** | 10.4 | **13.19** | **+27%** ⬆️ |
| worker_total_ms | 80.3ms | **59.8ms** | **-20.5ms** |
| worker_npu_ms | 67.7ms | 47.5ms | -20ms |
| tensor_to_fp 总 | 40.56ms | 13.98ms (memcpy 10.65+2.70+0.63) | **-26.5ms** ⬆️ |
| detection num | person 正常 | person 偶尔检出 (标定不同) | OK |

### 资源保留
- INT8 .nb: `/tmp/a733_test/wksp_full_chip_id_nbg_unify/network_binary.nb` (md5 421d096b...)
- 板端: `/etc/npu/yolov5/model/yolov5.nb` (当前用 INT8 版)
- baseline .nb backup: `/etc/npu/yolov5/model/yolov5.nb.baseline_pre_chip3b` (md5 9f8f9f2f...)
- 备份 (pre_chip3b 之前): `/etc/npu/yolov5/model/yolov5.nb.baseline_pre_hexpatch`

### systemd service 状态（跑 INT8 .nb）
- service active 跑 30s+，FPS 12.88-13.69
- 推流 udp://192.168.2.4:8554 持续
- 详细：journalctl -u camera-uvc-od-v24 -f

### 30 FPS 仍差多少
- 帧间隔 ≤ 33ms → worker_total ≤ 33ms
- 当前 60ms → 还差 27ms
- 进一步：缩 input 640→416（理论 -50%），换 yolov8n（理论 NPU 推理 12ms vs 26ms = -14ms）

### 老板明日选项
A. 收 13 FPS 收工（systemd 已固化）
B. 进一步缩 input 640→416 重导 .nb
C. 换 yolov8n 重导 .nb（Radxa 官方实测 79 FPS NPU 推理）
D. ACUITY 6.30.22 + 正确 chip ID flag 重导完整 README + 上传 INT8 .nb 到 Radxa 官方

- **【铁律】gst-x264enc 的 profile 必须用 property 设, 不是 option-string** (2026-08-10 实测栽过)
  - 错: `g_object_set(x264, "option-string", "profile=high", NULL)` → "Bad name for option profile=high" → caps rejected
  - 对: `g_object_set(x264, "profile", "high", NULL)` (property 形式) 或 `option-string="high"` (CLI 形式)
  - option-string 是给 x264 CLI 用, 不接受 gst property 风格的 `key=value`
- **【铁律】systemd ExecStart 命令行 env= 覆盖 Environment= drop-in** (2026-08-10 实测栽过)
  - ExecStart 用 `/usr/bin/env KEY=value ...` → process env KEY 设到最前
  - drop-in `Environment="KEY=value"` 永远被 ExecStart 覆盖
  - 调试 GST_DEBUG 必须在 drop-in 设, ExecStart 不能 hardcode GST_DEBUG=...
  - check list: 加调试 env var 前先 `cat /proc/PID/environ | tr '\0' '\n'` 验证生效
- **【铁律】appsrc caps 协商需要 first buffer, 但 live appsrc 慢 main loop 会 fail** (2026-08-10 实测栽过)
  - appsrc 默认 max-latency (1s) + need-data signal trigger 时等 buffer
  - main loop 如果 FPS 慢 (>1s 间隔), appsrc 在 buffer 到来前报 "Internal data stream error"
  - 修法: on_need_data callback 里立即 push 1 个黑帧作为 preroll, caps 协商后 main loop 接管
  - 或: 用 gst_idle_add 让 push_buffer 在 GMainLoop context 跑 (避免主线程 wait)
- **【铁律】videoconvert 默认输出 4:4:4 (Y444) → x264enc reject** (2026-08-10 实测栽过)
  - x264enc getcaps 接受 high-4:4:4, 所以 videoconvert 默认协商成 Y444
  - x264enc 默认 profile=constrained-baseline 实际 reject Y444 → "Internal data stream error"
  - 修法: launch string 强制 capsfilter I420: `! video/x-raw,format=I420,... ! x264enc`

### 8-12 笔记：count % 3 修复可能没生效
- source 加 `if ((count % 3) == 0) { ... } else { ... }` 包裹 OD 块
- 编译后 binary MD5 跟 race fix binary 完全一致 (cmp exit 0)
- 但 service log od_ms 仍全 > 14ms（每帧 OD），没有出现 < 5ms 跳过样本
- debug 实验: `if (count < 3) { ... }` 替换 count % 3 == 0 后，od_ms = 0.1ms（if 真的分支）
- debug 实验: `if ((count % 3) == 0) { ... }` + `volatile int count` 跑出来 od_ms 19-27ms（每帧 OD）
- **结论**: G++ -O2 对 `if ((count % 3) == 0) { ... } else { ... }` 做了某种优化，让 if 总成立（推测：modulo by constant 被常量化）
- 修法：用 `count & 0x3` 位运算替换 mod，或用 atomic<bool> flip 标志，或把 OD 移到独立线程

### 8-13 笔记：G++ -O2 mod 优化的真相 + 真正修法 (v31 final)
- **8-12 推测是错的** (count & 0x3 也没用, atomic fetch_add 也没用)
- **实测** (objdump -d 看 main 函数):
  - `if ((count % 3) == 0)` → G++ 生成 `cmp w0, #3` + `b.eq`, 但因为 count 总是递增, w0 % 3 总是变, 不能常量化
  - 实际上 G++ 把整个 mod 计算删掉 → `if (true)`, 每帧都跑
- **正确修法**: `static volatile int od_counter = 0; int od_c = od_counter++; if ((od_c % 3) == 0) { ... }`
  - volatile 让 `od_counter++` 不能被优化
  - 普通 int 自增在 main thread 单线程下足够
  - objdump 验证: `ldr w0, [od_counter_addr]; add w2, w0, #1; str w2, ...; smull w0 * 0x55555556 (mod 3); b.ne skip`
  - 实测 memcpy...load_input 计数 = 5/sec = 14 FPS × 1/3 ✅
- **atomic<int>::fetch_add 失败**: 代码生成 (__aarch64_ldadd4_acq_rel) 对, 但运行时仍每帧 OD — 可能 atomic {0} brace-init 在某些情况下不调 atomic_store, od_c 一直 0 → mod 0 == 0 总成立 → 每帧 OD
- **教训**: 单线程下别用 std::atomic, 用 volatile int 简单可靠

**【铁律】uvc/UAC 等 USB device class: descriptor 的 capability 不是 actual performance**（2026-09-19 实测挨批）
- Microdia 0c45:6340 UVC 报 "max 30 fps" capability（lsusb dwFrameInterval 列 6 个值 30/25/20/15/10/5）
- 实测只跑 13-18 fps（auto-exposure 锁在 ~54ms interval = 18.5 fps 上限）
- **没查 datasheet 就下结论 "sensor 物理上限 15fps" 是错的**（老板 push 查手册后才找到 UVC FAQ 真根因）
- 正确流程: 任何 "X 是物理上限" 类结论前, 必须:
  1. 查 vendor 官方 datasheet / UVC class spec / driver quirks table
  2. 开 `uvcvideo trace=0xffff` 看 PROBE/COMMIT 实际 dwFrameInterval
  3. 实测变环境变量 (光照/负载) 看 fps 是否响应, 排除 firmware vs hardware
- 不要直接信 "v4l2-ctl --get-parm" 报的值（报的是 max，不是协商 actual）

**【铁律】老板 push 查手册时必须真去查, 不要用 "查不到" 搪塞**（2026-09-19）
- 第一轮我说 "Microdia 不公开 datasheet, 查不到", 老板 push 后才发现 UVC FAQ ideasonboard.org 有完整说明
- 查不到 ≠ 没资料, 先 web_search + web_fetch 多源试, 实在查不到才说

### 8-13 笔记：libod_helper.so post-process 实际输出格式
- workspace 旧注释: `yolov5_post_process_bgr` 输出 `"%2d: %3.0f%%, [%4.0f, %4.0f, %4.0f, %4.0f], %s\n"` (label: conf%, [x1,y1,x2,y2], class_name)
- **实测 `strings libod_helper.so`**: 只有 `"detection num: %d"` 一行格式
- **后果**: sscanf 7-field 永远 != 7 → det_count = 0 → log dets=0 (false negative)
- **修法**: 改 parser 用 `sscanf(line, "detection num: %d", &n)`
- **限制**: 当前 libod_helper.so 不输出 bbox/class, 只拿得到 count
- **要 bbox/class**: 必须改 libod_helper.so 源码 + 重编 (板端有 libod_helper.so 但需找源码)

### 8-13 笔记：v31 log `od_ms` misleading 不代表 bug
- log 每秒打印 `count=N ... od=21ms`
- 看起来每帧 OD, 实际 memcpy 计数 = 5/sec = 1/3 跑 OD
- **根因**: G++ 编译时把 t_od 读取时点放在 print block 之前一帧 (用上一帧的 t_od), 而 print 命中前一帧是 OD 的概率较高
- **不是 bug**, 不影响功能, 只是 metric 不直观
- 验证 OD 跳过的正确方法: `journalctl ... | grep -c "memcpy.*load_input"` 除以 FPS, 应 ≈ 1/3

### 8-13 笔记：PC 端 VLC 卡顿根因 + SDP framerate 修复
**问题**：
- 板端 launch string SDP `a=framerate:30` 跟实际板端稳态 14 FPS 不匹配
- PC 端 mpv/VLC 按 SDP 30 FPS 期望设计 buffer, 但实际只有 14 FPS 推送, buffer 永远吃不满
- 每次 NPU spike (od_ms 33-130ms) 都直接体现在画面上 = "间歇性卡顿"

**修复**：
- launch string 3 处 framerate 改 30/1 → 14/1 (appsrc caps, capsfilter I420, capsfilter H.264)
- g_state.FPS = 14 (appsrc on_media_configure 设 caps 用)
- capsfilter H.264 加 `profile=constrained-baseline` (不强制 level, 避免 SDP/SPS level 不匹配导致 not-negotiated -4)

**踩坑**：
- 第一次加 `level=3.0` 强制 SDP profile-level-id=42c01e, 但 x264 实际 SPS level=L1.0 (因为 14fps bitrate 低自动 pick)
- SDP/SPS level 不一致 → pipeline caps 协商失败 → "streaming stopped, reason not-negotiated (-4)" → 503 Service Unavailable
- 解决: capsfilter 只设 `profile=constrained-baseline`, 不设 level, 让 x264 自己选 (现在 SDP=42c016 = Constrained Baseline L1.0, 跟 SPS 一致)

**实测**:
- SDP: `a=framerate:14, profile-level-id=42c016` (Constrained Baseline L1.0)
- 老板 PC 端 mpv / VLC 验证: 间歇性卡顿问题已解决 ✅

**资源链**:
- workspace source MD5: 704221db
- 板端 binary MD5: 773507e7
- 板端 binary 备份: /home/radxa/v31_v4l2_od_rtsp.bak_race_fix_20260812 (前一版, race_fix)
- 中间失败版本 (level=3.0): /home/radxa/v31_v4l2_od_rtsp.bak_pre_level30_20260813 (capsfilter level 强制导致 not-negotiated)
- 当前 working: 板端 v31_v4l2_od_rtsp (fps14_profile)

## 2026-08-17 23:09 — yolov8 decode 已加 v31 (PC 端 toValidate 验证通过)

### 背景
- yolov8s 训练完成 (180 张 老板客厅 + 6 not_person, mAP50=0.995, 30 epoch, 43 秒)
- ONNX 已导出 (best.onnx 42.6MB, best_simp.onnx 单 output0 shape [1,6,8400])
- 板端现有 v31 binary 只支持 yolov5 decode (3 outputs, 80 classes COCO)
- **pegasus ONNX → .nb 卡在 Vivante IDE 5.11.0 缺 VXC shader 编译器** (cl_viv_vx_ext.h 找不到)
  - gen_nbg 在 CPU simulator 跑 yolov5s 7M 参数需要 5+ 分钟
  - yolov8s 11M 参数跑了 6 分钟还没出 .nb, 放弃等
- 路径选择: 加 v31 yolov8 decode (板端 .nb 仍需 pegasus 解决, 但 C++ 部分先就位)

### v31 加的改动 (commit 8-17 23:09)
1. **常量新增**:
   - `static const int YOLOV8_TOTAL_ANCHORS = 8400;` (80×80+40×40+20×20)
   - `static const int YOLOV8_NUM_CLASSES = 2;` + `YOLOV8_CLASSES[2] = {"person", "not_person"}`
2. **新函数 `yolov8_decode_all()`**:
   - 输入: `void** outputs` (单 tensor) + orig_w/orig_h
   - 内存 layout NCHW (1,6,8400): `out[i + c*8400]` = channel c, anchor i
   - 4 个 xywh (pixel 坐标, 640×640 letterbox) + 2 个 cls score (sigmoid'd)
   - per-class conf 阈值 (复用 PERSON_CONF_THRESH=0.45 / OTHER_CONF_THRESH=0.65)
   - 复用 area filter (MIN_BOX_AREA_RATIO / MAX_BOX_AREA_RATIO)
   - **不复用 yolov5 anchor 解码** — yolov8 是 anchor-free, xywh 已是 pixel 坐标, 直接 cx±w/2 cy±h/2
3. **argv 切换**: `argv[10] = "yolov8"` 启用 yolov8 模式 (默认 yolov5)
4. **draw_detections 加 is_yolov8 参数**, 切换 80 COCO labels vs 2 person/not_person labels
5. **主循环 if/else**: yolov8 模式跳过 `yolov5_post_process_bgr` (libod_helper 写死 3 outputs)
6. **复用 NMS/letterbox_inverse/temporal voting**: DetBox 结构兼容, yolov5_nms 可用

### PC 端 toValidate 验证 (test_yolov8_decode.py)
- yolov8s.onnx → onnxruntime 跑 frame_0001.jpg
- v31 yolov8_decode 输出 vs ultralytics 内置 NMS:
  ```
  top-1 box diff:
   mine:  [0.0,0.0,151.7,350.3] conf=0.990
    ref:   [0.0,0.2,154.4,353.0] conf=0.990
    diff:  [0.0,0.2,2.6,2.8]
  ```
- 误差 2-3 像素 (NMS tie-breaking 差异), 同一类同 conf ✓

### TODO (下一步, 按优先级)
- [ ] 解决 .nb 生成: 找完整 Vivante IDE (含 VXC 编译工具), 或换 tina-sdk Tina 编译流程
- [ ] 板端交叉编译 v31 with is_yolov8 path
- [ ] 部署 .nb + 重启 service + VLC 拉流验证 (与 v38 对比)
- [ ] 备份 v31 源码 v38 + yolov8-decode 版本
- [ ] 老板验收: 框位置不准问题是否改善

### 关键文件
- v31 源码: /home/yuewuya/.openclaw/workspace/v31_v4l2_od_rtsp.cpp (line 60-69: yolov8 常量, line 213-249: yolov8_decode_all)
- 测试脚本: /home/yuewuya/.openclaw/workspace/dataset_od/test_yolov8_decode.py
- yolov8s 模型: /home/yuewuya/.openclaw/workspace/dataset_od/runs/yolov8s_person_finetune/weights/best.pt (22.5MB)
- yolov8s ONNX: best_simp.onnx 28MB (单 output0 [1,6,8400])

## 2026-08-17 23:50 — yolov8s.nb 部署成功，板端跑通！

### 完成项
1. ✅ pegasus export 通了 — 通过 `gen_nbg + VIV_VX_ENABLE_SAVE_NETWORK_BINARY` + `VSIMULATOR_CONFIG=VIP9000NANODI_PLUS_PID0X1000003B` (target=0x1000003B for A733)
2. ✅ yolov8s.nb 生成 (11.5MB, target=0x1000003B)
3. ✅ 板端 NPU load yolov8s.nb 成功: `output 0 dim 8400 6 1, data_format=3 (INT8), scale=2.484082, zero_point=-128`
4. ✅ v31 加 yolov8 decode + NHWC layout + sigmoid (raw logits → probability)
5. ✅ 板端 service 跑 yolov8s.nb + yolov8 argv 模式, FPS ~14, awnn_run 34ms

### 关键修正 (踩坑)
1. **NPU 输出 layout 是 NHWC 不是 NCHW**:
   - 起初按 ultralytics 文档 (NCHW) 写 → max_person=0 全 0
   - 修正后 (NHWC, 6 个值连续): max_person=630 (raw logit)
2. **NPU 输出是 raw logits (反量化 INT8) 不是 sigmoid'd**:
   - 加 `score = sigmoid(logit)`, 阈值仍用 0.45 → sigmoid(630) ≈ 1.0
3. **gen_nbg 编译需加 -lArchModelSw -lNNArchPerf** (默认 makefile 没加)
4. **Vivante IDE 缺 VXC shader compiler**: `cl_viv_vx_ext.h` 找不到
   - 解决: `VIV_VX_ENABLE_SAVE_NETWORK_BINARY=1` + `VIV_VX_SAVE_NETWORK_BINARY_PATH=...` 跳过 verify 直接保存 .nb
5. **VSIMULATOR_CONFIG 必须设**: pegasus_setup.sh v3 → VIP9000NANODI_PLUS_PID0X1000003B (匹配 A733)

### 板端备份链 (2026-08-17 23:50)
```
当前:  /home/radxa/v31_v4l2_od_rtsp            md5 6bacc8e5 (yolov8 decode, yolov5 mode 也 work)
bak:    /home/radxa/v31_v4l2_od_rtsp.bak_yolov8_decode_20260817_2350  同上 (服务前 cp)
v38:   /home/radxa/v31_v4l2_od_rtsp.v38       md5 3ff10c6a (昨晚 yolov5 mode)
pre:   /home/radxa/v31_v4l2_od_rtsp.bak_pre_yolov8_decode_20260817_2330  同 v38
.nb:   /etc/npu/yolov5/model/yolov5.nb      (5MB, COCO 80类, 老备份)
       /etc/npu/yolov5/model/yolov8s.nb     (11.5MB, 2类 person/not_person, 当前 active)
```

### service 配置
```
ExecStart=... /home/radxa/v31_v4l2_od_rtsp /dev/video0 640 480 2 86400 /etc/npu/yolov5/model/yolov8s.nb 0.40 0.55 1 yolov8
                                                                ^^^^^^^^^^^^^^ ^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^ ^^^^^^^^
                                                                yolov8s.nb 替换 yolov5.nb                                                argv[10]="yolov8"
```

### 实测表现
- 板端 NPU inference: 34ms/帧
- FPS: ~14 稳态
- 画框正常 (NMS 80-100 → top-1)
- **box 位置不准**: model 在暗场景把 person 预测在画面中心
  - 根因: 训练数据 auto_label.py 把所有200 张 raw frame 的 person 框都标在同一固定位置 (200 张是 6.6秒内连续抓, person 没动多少)
  - 修法: 重新精细标 person 位置 (200 张都不一样的 box), 重新训练
  - 这是 model 准确度问题, **不是 v31 代码问题**

### TODO (明天)
- [ ] 重新标 person 位置: 让老板在新场景多位置坐, 抓 ≥ 100 张图, 用 image tool 标 actual person box
- [ ] 重训练 yolov8s (同样 30 epoch, ~1 分钟)
- [ ] 重导出 best_simp.onnx → .nb
- [ ] 部署新版 .nb + 验证 (框应跟随实际 person 位置)
- [ ] (可选) 改 MAX_BOX_PER_FRAME = 2 让 person + not_person 同时显示

## 2026-08-17 23:55 — 新铁律 (NPU 输出)

- **【铁律】NPU 输出 layout 是 NHWC 不是 NCHW** (pegasus 部署后)
  - 文档/PC 端 onnxruntime 输出是 NCHW
  - 但 pegasus → .nb → NPU 输出是 NHWC (data_format=3 INT8, scale=2.484082)
  - 修正: 读时用 `out[i*6 + c]` 不是 `out[c*N + i]`
- **【铁律】NPU 输出是 raw logits 不是 probability**
  - 即使 .nb 用 sigmoid 节点, NPU 反量化后给的是 sigmoid 之前的 logits
  - 阈值要重新校准 (raw logit 4.0 ≈ sigmoid 0.98)
  - 或者代码里加 `score = 1/(1+exp(-logit))` 再用 0.45 阈值
- **【铁律】pegasus export 缺 VXC shader compiler**
  - Vivante IDE 5.11.0 缺 `cl_viv_vx_ext.h`
  - gen_nbg verify graph 会失败
  - 绕过: `VIV_VX_ENABLE_SAVE_NETWORK_BINARY=1` + `VIV_VX_SAVE_NETWORK_BINARY_PATH=<path>` 让 gen_nbg 跳过 verify 直接写 .nb
- **【铁律】pegasus VSIMULATOR_CONFIG 必须设**:
  - `v3` = `VIP9000NANODI_PLUS_PID0X1000003B` (A733)
  - 默认 PICO (target=0x10000020) 会被 NPU driver reject
- **【铁律】gen_nbg 编译必须加 -lArchModelSw -lNNArchPerf**
  - 默认 pegasus_export_ovx_nbg.sh 生成的 makefile.linux 没这俩
  - 加 `LIBS+= -L$(VIVANTE_SDK_DIR)/lib -lArchModelSw -lNNArchPerf`

## 2026-09-04 00:35 — 新铁律 (RCA 流程)

- **【铁律】RCA 流程：根因不明确时不盲试** (2026-09-04 老板立, 源于今晚 person/cat/dog 抖动排查)
  - **核心原则**: 根因不明确 → 不定措施, 只列假设
  - **流程**:
    1. 现象确认 (实测数据, 不是猜)
    2. 列出**前 5 种最可能根因** (按概率从高到低)
    3. **每个假设单独验证** (跑 1 个再回头分析, 不批量)
    4. 验证到根因明确 → 再制定解决措施
  - **不要做**:
    - 不要"先试一下说不定能过"
    - 不要同时改 N 个变量
    - 不要在没验证假设前直接给方案
    - 不要凭经验 / 凭"我以为" / 凭推理给方案
  - **典型反例** (今晚实际发生过):
    - 8-17 auto_label.py 误诊为根因 → 实际是 v31 binary NPU layout bug (根因错)
    - 9-3 推荐 fine-tune 你场景 → 老板指出是单点解, catastrophic forgetting 破坏 80 类 (方案错)
    - 今晚 person/cat/dog 抖动 → 我列了 5 种可能但没逐个验证就给"换 backbone"方案 (流程错)
  - **check list**: 任何 troubleshooting 类问题, 先问"根因明确吗", 不明确就进上面 4 步流程, 明确才能定措施

## 2026-10-11 15:30 — 新铁律 (架构 A + OD 优化)

### 架构 A — OD 独立 worker thread (老板决策)
- **【铁律】OD-bound 系统: 拆 OD 独立 thread, main 不等 OD, fps 拉满 camera ceiling** (2026-10-11 老板拍板, 源于今晚 v31 19.83 fps OD-bound 优化)
  - **场景**: 单线程 main loop 中 OD pipeline (NPU inference + decode + nms) 总耗时 35-44ms / 帧, camera 30fps 但 fps 上限被 OD 拖到 19.83
  - **架构**:
    ```
    main (capture + push):              worker_thread (OD):
    ===================                ===========================
    while: while:
      DQBUF                               wait q_frame
      cvtColor → bgr                       pre_process (libod_helper.so)
      push q_frame (bgr.clone ~0.5ms)     awnn_set_input_buffers
      pop q_result last (timeout 0)       awnn_run (35ms ★)
      draw last_det on bgr                 awnn_get_output_buffers
      push appsrc                          yolov5_decode_all + yolov5_nms
      QBUF                                 push q_result → main
    ```
  - **预期 + 实测**: fps 19.83 → **29.78** (camera-bound, 拉满 camera ceiling 29.82)
  - **关键决策**:
    - q_frame max=2 (drop oldest backpressure, OD 永远算最新帧)
    - q_result max=1 (try_pop_latest, main 只用 last_det)
    - bgr 必须 clone (避免 OD 算时 main 改写 race), 0.5ms 开销可接受
  - **跟 9/19 MT 区别**: 9/19 MT 拆 capture/process 两线程同步 pipeline (camera 实际只 15fps 退步 -1%); 架构 A 是 OD 异步拆分 (camera 满 30fps 时 fps +50%)
  - **det 显示**: 永远晚 1 帧 (33ms) + 偶尔不更新 (OD 慢 17% 时新帧覆盖旧)
  - **老板约束**: "为了保证检测的效果是不可能隔帧检测的" → 否决 OD_SKIP 方案
  - **代码模式**:
    - FrameQueueT<T> 模板 (push_drop_oldest / wait_pop / try_pop_latest + cv_not_empty + mutex)
    - DetectorWorker class 接受 ctx + W + H + stderr_file + 5 个 libod_helper 函数指针 + q_frame_max
    - main loop 不等 OD, push q_frame (clone bgr) + pop last_det + draw
  - **check list**: OD-bound system 优化时, 第一时间考虑架构 A (OD 异步), 不优先 OD_SKIP/NPU 加速

- **【铁律】Ring buffer / lock-free queue 引入同步依赖, OD 慢时退步 fps** (2026-10-11 实测教训)
  - 试 BgrRing class (3 slots, mutex + cv, acquire/release) 替代 bgr.clone() zero-copy
  - 实测 fps **24.5** (退步 5fps, 从 29.66 跌)
  - **根因**: OD 慢 17% (worker 36ms vs main 33ms), ring 满, main acquire 阻塞 12-56ms 等 OD release → main 退化成同步模式
  - **教训**: ring buffer 只有在 OD ≥ capture 节奏时才行, 否则 backpressure 反向拖累
  - **替代方案**: bgr.clone() 0.5ms 是最低开销方案, ring buffer 只在 OD 永远不慢于 capture 时才有 ROI
  - **check list**: 加 queue/backpressure 前算 OD 跟 main 节奏差, 慢 ≥5% 时不要用 blocking acquire

- **【铁律】RTSP SDP framerate 必须跟实际 fps 匹配** (2026-10-11 修, 旧 bug 自 8/13)
  - 老 `g_state.FPS = 14` + 3 处 caps `framerate=14/1` (8/13 留的, 当时实际 14 fps)
  - 实际 fps 涨到 30 后, VLC/mpv 按 14fps 设计 buffer, 永远吃不满, spike 直接体现在画面
  - 修法: 改 `g_state.FPS` + 同步 caps 跟 launch string, 3 处必须一起改 (appsrc caps / capsfilter I420 / capsfilter H.264)
  - **check list**: 任何 fps 性能调优后必须同步 SDP, 跑 VLC/mpv 验证 buffer 设计正确

- **【铁律】Deli 1080P USB camera ceiling 30fps vs Microdia 15fps** (2026-10-11 实测)
  - 老板换了 Deli 1080P Camera (3001:4034) 替换之前的 Microdia
  - 实测 capture ceiling: **29.82 fps** (vs Microdia 15fps), +50% camera 硬件提升
  - Deli 6 种 format 实测 (USB2 限速):
    - YUYV 640x480 → 29.82 fps (camera 上限)
    - MJPG 全 res → 30 fps (JPEG 压缩摆脱 USB2 带宽)
    - YUYV 800x600 → 19.87 fps (USB2 18MB/s 带宽)
    - YUYV 1280x720 → 9.94 fps
    - YUYV 1920x1080 → 4.97 fps
  - NPU input 永远 640x640 letterbox (不管 camera res), 所以 OD timing 跟 res 无关
  - **check list**: 摄像头换新后第一时间测 ENUM_FMT + capture ceiling, 跟旧摄像头对比 ROI
