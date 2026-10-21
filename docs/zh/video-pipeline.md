# Pro AX630C 视频流水线

`ax630c.so` 从 `open_vin_capture` 读取 YUYV，优先把已出队的 capture DMA 缓冲区导入 VC8000E；导入失败才复制到编码器帧缓冲。Core 使用绑定编码器接口读取 H.264 或 H.265 AU。

## 编码格式

后端 ABI 和设备描述同时声明 `auto`、`h264`、`h265`。后端独立打开 `auto` 时选择 H.264；网页在自动模式建立连接时提交浏览器已验证的编码偏好，由 Core 在首个视频会话注册前选择 H.265 或 H.264，持久配置仍为 `auto`。H.265 使用 VC8000E 的 HEVC Main、8-bit 4:2:0 硬件编码路径，不在 CPU 上做视频软件编码。创建、重置、编码器查询及每个输出包都必须使用实际选中的格式，不能只开放配置选项。

H.264 沿用原来的 `ENC_RC_LEGACY` 寄存器程序；H.265 使用 `ENC_CODEC_HEVC` 与 `ENC_RC_FIXQP`，PPS 的初始 QP 与硬件帧 QP 保持一致。HEVC 参数集由 `third_party/vcenc-ewl/vcenc_hevc_header.h` 生成，来源和许可见该目录的 `UPSTREAM.md`。每个 H.265 IDR 前补齐 VPS/SPS/PPS（NAL 32/33/34），接 IDR NAL 19；普通 P 帧为 NAL 1。编码器不输出 B 帧，参数集声明无重排。尺寸按最小编码块对齐并用 conformance window 裁回实际画幅，level 按图像尺寸及 60 fps 上界选择。

切换格式时强制生成新的 IDR；实时画面、输出缩放和无信号占位共用选中的编码路径。浏览器能否播放还取决于当前传输协议下的 HEVC 解码能力；自动模式优先使用已通过 WebRTC／WebSocket 探测的 H.265，不支持时回落 H.264。有视频会话占用共享编码器时，后来的连接沿用当前编码，不能覆盖主会话或扩展的编码选择。

HDMI 桥片采集的 YUYV 为有限范围 BT.709；H.264 SPS 与 HEVC SPS 的 VUI 均显式声明 `video_full_range_flag=0`、色域／传递函数／矩阵 `1/1/1`，且不随输出缩放尺寸改成 BT.601。上游 HEVC 头的全范围 `5/1/5` 与实际像素不符，H.264 缺省色彩标记也依赖解码器猜测，都会引起两种编码的颜色、黑位和饱和度差异。修正码流标记，不对实时像素增加转换。无信号占位仍使用原有资源。

## 编码流色彩信息 ABI

后端声明 `ENCODER_COLOR_INFO`，通过表尾 `encoder_color_info` 返回实际编码流的
色深与范围。当前 VC8000E 的 H.264 Main 和 HEVC Main 都输出 8-bit、有限范围
BT.709；查询值与 SPS/VUI 生成器共用配置常量，不从 HDMI 输入位深或 EDID 推断。
首个带参数集的 IDR 成功发布后才缓存结果。初始化、编码器重置、帧缓冲重建和
解绑时清空缓存；无信号占位也经过同一编码路径。查询只读原子缓存，并验证
调用方结构长度，不访问硬件、取帧或等待编码。

## 编码到收帧

绑定 H.264/H.265 路径声明 `BOUND_ENCODED_TIME`，在 VCMD 编码完成等待返回后，
立即为该 AU 记录 `CLOCK_MONOTONIC` 纳秒时间，随 `packet.pts_ns` 交给 Core。
实时、缩放和无信号占位均使用同一测量边界，不使用按帧序号生成的合成 PTS。
测量涵盖随后参数集封装、复制、Core 排队、打包、加密及网络收帧；不代表硬件
中断的精确发生时刻。浏览器按实际 RTP 时间戳匹配完整帧收包时间，校时估算、
误差与缺失数据语义见 [`Core WebRTC 延迟规范`](../../../onekvm-core/docs/webrtc-low-latency.md#编码到收帧)。

## 本地 RGB 显示

`source_display` 通过 TDP 硬件从 YUYV 采集缓冲生成 640×360 RGB565，小屏通过 `video=display` 的共享内存环读取，不经过 JPEG 或 H.264 解码。有编码消费者时共享同一 VIN 帧，独立生成 RGB 缓存；仅有小屏消费者时直接出队 VIN 缓冲，跳过已完成的旧帧，转换最新画面后归还缓冲。Core 的 display-only 需求只保持采集绑定，不调用编码器读包，后端不能依赖编码路径产生 RGB。画面等比缩放、空余区域留黑；进入无信号模式时预先转换同一份已批准的占位源图，后续 RGB 请求可独立读取。

后端声明 `SOURCE_DISPLAY_CACHE_ONLY`；Core 在主编码消费者存在时传入
`DISPLAY_REQUEST_CACHE_ONLY`。该模式下缓存暂时没有新帧也只等待，不能自行出队、
丢弃 VIN 缓冲或执行停帧恢复。显示独占采集时继续保留直接出队和恢复路径。
主视频线程取得 source 地址不等待预览调用，避免 15 FPS 小屏的等待节奏串行化
主会话每帧的绑定检查。有编码消费者时，独立工作线程转换 RGB，最多借用一个 VIN
缓冲，不堆积预览任务；编码线程继续使用其余缓冲。只有编码和预览都完成后才归还
同一 DMA 缓冲。工作线程不获取 VIN 锁，不执行 QBUF；编码线程统一回收已完成的
预览借用。停流和模式重建在保持 VIN 锁的同时等候转换结束，再释放映射和 DMA 导入。

显示缓存使用独立锁及单调时钟条件变量；有编码会话时，读取缓存完全不获取 VIN 锁，避免排在主编码器等待采集之后。直接出队与模式切换、编码共用 VIN 锁，单次采集轮询最多 50 ms，其余等待释放 VIN 锁。只有最近两秒内有显示请求时才转换实时帧，最多 15 fps；关闭小屏串流后沿用 Core 的消费者需求机制释放采集与编码资源。显示等待最多一秒，返回 timeout 时客户端保留上一帧；重新连接仍请求 RGB，不永久回退编码流。

实时 RGB 使用 `VIDIOC_EXPBUF` 导出当前 VIN 缓冲，再通过 root-only
`/dev/onekvm-vproc`（动态 `/dev/videoX` 的稳定别名）的标准 V4L2 mem2mem
OUTPUT 队列提交 DMA-BUF；输入不经过 CPU 拷贝或逐像素转换。
TDP 同时完成缩放和有限范围 BT.709 到 RGB565 的转换。CSC 输入 Y offset 为 16，
去除有限范围黑位；缩放的 Q10 初始采样位置按 `(step - 1024) >> 1` 计算，
纵、横方向独立设置，放大时保留负相位，不能固定为 3:1 缩小的相位。
输出由驱动分配为可缓存 DMA pages，任务两端显式同步 DMA/CPU 所有权，
用户态在 CAPTURE 队列使用可复用的 MMAP 缓冲，DQBUF 后只读映射并复制
缩小的 RGB 图像到显示缓存。禁止把 coherent VIN
映射或不可缓存 RGB 映射当作普通内存逐像素访问。

TDP 驱动限制尺寸、步长、DMA 范围和任务等待时间，并串行提交硬件任务。
默认等待 100 ms；收到完成中断后仍须确认 AXI 写事务排空，再发布缓冲或调度
下一任务。异常时先关闭并排空 AXI，再复位 TDP 私有时钟域，不复位
共享 CDMA、VIN 或编码器。无法确认 DMA 停止时隔离设备，保留缓冲及供电到
重启，避免释放仍被硬件访问的内存。硬件失败时保留上一帧并限频记录错误，
实时画面不回退 CPU 转换。静态无信号素材沿用已有的 CPU 转换路径。
最后一项任务完成后空闲一秒，TDP 通过 runtime PM 关钟。
模块卸载先等待任务结束并禁用 runtime PM，只在设备仍 active 时关闭时钟；
不能在 remove 中留下额外的 PM 引用。平台设备跨模块重载保留该引用计数，
不平衡会使后续加载的驱动永久无法自动休眠。

`onekvm-video-ax630c-diag` 包提供 `ax630c-tdp-test`，使用标准 V4L2 分配、
导出和导入 DMA-BUF，检查 YUYV/NV12、BT.601/709、完整／有限范围、裁剪、
缩放、缓冲复用与带未完成任务的关闭。测试只在准备阶段填充输入色条，热循环
不复制输入；其不受帧率限制的进程 CPU 时间仍不能代替实际预览 CPU 测量。
测试的 CPU 填充输入使用 coherent MMAP：此内核的 vb2-dma-contig 导出缓冲
没有实现 DMA-BUF CPU access 同步，不能只靠 `DMA_BUF_IOCTL_SYNC` 刷新未入队
的 non-coherent 测试页。实际 VIN/VDEC 输入由硬件写入，并在生产者出队后借用。
回归测试还须超过输入附件缓存上限，反复用新 DMA-BUF 替换同一 vb2 槽；
驱动的 `buf_cleanup` 释放引用后必须清空指针，因为换槽时 vb2_buffer 本身仍会复用。

通用用户态实现位于 `onekvm-generic-v4l2/src/v4l2_transform.c`。它使用
`VIDIOC_S_FMT`、`VIDIOC_S_SELECTION`、OUTPUT DMABUF 和 CAPTURE MMAP，
不包含 AX630C 寄存器或私有 ioctl。每实例最多持有一个任务或输出帧，固定上限
的输入槽按 DMA-BUF 身份复用附件，输出只分配、映射一次。`receive(0)` 不等待；
关闭时先 STREAMOFF 完成 DMA，再释放输入引用。平台可用相同设备别名接入其他
标准 V4L2 转换驱动；目前实机验证范围为 105 Pro。

仅小屏采集连续 300 ms 没有 VIN 帧时，若桥片仍发布与当前采集一致的稳定尺寸，重建 DMA 队列并重新导入编码器缓冲。失锁或几何变化期间等待绑定流程处理，不反复重开旧尺寸。这与编码读取路径的停帧恢复对应，避免低分辨率切换后只有 RGB 消费者时永久停帧。

## 本地媒体解码

Pro 的 Hantro G1 使用主线 Hantro 驱动框架，AX630C 适配只负责设备识别、
时钟和私有 VDEC 复位。对用户态提供标准 V4L2 Stateless H.264 mem2mem：
OUTPUT 为 H264_SLICE，CAPTURE 为 NV12，通过 Media Request 提交标准
SPS/PPS、参考帧和解码参数。`/dev/onekvm-vdec`、`/dev/onekvm-vdec-media`
分别是对应 `/dev/videoX`、`/dev/mediaX` 的稳定别名；不固定动态编号，
也不提供私有寄存器 ioctl。

`ax630c.so` 提供通用的独立 [VDEC ABI](../../../onekvm-core/docs/vdec-backend-abi-v1.md)
复用 GStreamer `h264parse` 传递 SPS VUI 色彩信息，再由 `v4l2slh264dec`
驱动硬件解码和管理 DPB/B 帧重排。裸 appsrc 的 caps 没有色彩信息时，
SD 尺寸的 BT.709 视频会被默认按 BT.601 解释，因此不能省略上游解析器。输入使用
固定上限的缓冲池；输出借用驱动映射并显式声明支持 stride/crop metadata，
避免 appsink 要求紧密排列而触发额外整帧复制。PTS 和调用方 tag 随原始
codec frame 的 metadata 一起重排，不以可能重复的 PTS 查找身份。

解码核心输出仍为 NV12。VDEC ABI 同时提供 NV12 和 RGB565：选择 RGB565 时，
适配器将 GStreamer V4L2 capture 的 DMA-BUF 直接交给上述 V4L2 转换设备，
保留解码器的 stride、填充高度、可见 crop 和实际 BT.601/709／色彩范围。
转换完成前保留原始 GstSample，避免 DPB 提前复用；输出帧释放后才回收 RGB
缓冲。非阻塞接收可跨调用等待转换，不丢失 PTS/tag。小屏按 ABI 能力优先选择
RGB565，复用紧密排列的目标数组，合成器直接读取 RGB565，不再逐像素做 CSC。
此处 RGB 是硬件后处理输出，不代表 Hantro 解码核心原生输出 RGB。硬件转换失败
直接返回错误，不回退软件色彩转换。

支持标准缓存提示的队列使用 `V4L2_MEMORY_FLAG_NON_COHERENT` 申请可缓存
映射，CPU/DMA 所有权同步由 videobuf2 管理；不支持时沿用驱动原有映射。
API 表查询不打开设备。只有实际解码需求才创建会话，硬件错误直接返回，
不选择软件解码器，不复位 VIN/VENC/CDMA。此接口也为其他平台预留；
当前适配和实机验收范围仅为 105 Pro；ABI 的能力表、像素格式和帧生命周期不绑定 AX630C，其他平台可提供同一入口并由消费者按能力选择输出。

这些队列仍然是 DMA 缓冲区，V4L2 并不会因为用户态使用 MMAP 就退回 CPU
拷贝。Linux 6.18 的 `dma_alloc_noncontiguous()` 对非一致缓存路径不接受
`DMA_ATTR_NO_KERNEL_MAPPING`；AX630C Hantro 队列只保留
`DMA_ATTR_ALLOC_SINGLE_PAGES`，并交由 videobuf2 完成 CPU／设备所有权同步。
因此分配失败应作为驱动属性组合错误处理，不能通过切换软件解码器掩盖。

## 按需采集与空闲时钟

沿用 Core 的消费者需求判断：网页、VNC/RTSP、录制或其他实际取帧需求结束后解绑视频源。创建编码器只保存配置；绑定时才打开 `/dev/es_venc`、映射命令池并申请帧缓冲。解绑依次停止采集、释放 DMA 导入及编码缓冲、解除映射、关闭编码设备；再次绑定从新 IDR 开始。

`open_vin_capture` 在 STREAMON 前取得 runtime PM 引用，STREAMOFF 停止 SIF/WDMA/CSI 并排空中断和 AXI 请求。空闲 1 秒后关闭 ISP 内部 gates 及 VI/ISP_MM 公共时钟；AXI 仍忙时拒绝关钟。快速重开即使取消了 autosuspend，也要重新放行 AXI。恢复只开时钟并恢复请求，不重复全局 reset 扫描。CSI 停流继续关闭 D-PHY 和自身时钟；空闲状态查询只返回缓存计数，不读已关钟的寄存器。

VCMD 同时持有文件生命周期和命令引用，覆盖 ioctl、映射及最终关闭的 abort/寄存器访问，最后关闭后空闲 1 秒再关 VENC 时钟。关闭异常且 DMA 未确认停止时保留供电。诊断用 JENC/VDEC 驱动只在读取寄存器或改频率时唤醒，并在空闲 1 秒后关钟。所有模块保留 reset 状态，不逐帧切换时钟。

HDMI 输入经 LT86102UXE 分配到 HDMI OUT 和 LT6911UXC 采集桥。空闲策略只管理 SoC 采集/编码侧，不切换 `/proc/lt6911_info/{power,hdmi_power,loopout_power}`，保留桥片 EDID/HPD 和持续环出。不能用移除 `clk_ignore_unused` 代替驱动的资源管理。

## 无信号占位

无信号画面在后端编码，前端控制台不叠 Vue 占位图。画面资源来自 `onekvm-video-soph-mmf/src/no_signal_frames.inc`，同步复制到本目录的 `src/no_signal_frames.inc`；更新 MMF 已批准的 PNG 后必须重新同步该文件。`ax_no_signal.c` 把 packed NV21 解包并转为 VC8000E 输入所需的 YUYV，尺寸不匹配时等比留黑边。

首次绑定而 HDMI 未锁定时，后端分配默认 1920×1080 编码帧缓冲，把占位图写进其输入区，并以 5 fps 经正常 VC8000E 通道输出所选的 H.264 或 H.265。实时输入消失后，从首次检测到无信号起等待 3.2 秒，再停 VIN 并切换占位；即使 VIN 仍输出旧帧，也不能用这些帧重置无信号计时。105 的常规模式切换在此窗口内恢复，避免向浏览器连续发送占位与新尺寸两个 IDR、引起额外解码停顿。真实断信号实测从关闭 HDMI 到占位 IDR 约 4.5 秒。已知无 HDMI 时不得反复重建 DMA 环。两种画面切换时都重新从 IDR 开始，使 WebRTC 及录制端可解码。

`/proc/lt6911_info/hdmi_rx_status` 只表示插头存在。判定锁定还要确认桥接芯片电源为 `on`、`status` 为 `new res` 或 `stable`，且宽、高、帧率均非零。占位期间每次编码前重新检查输入，信号恢复后启动 VIN 并切回实时帧。

`ax630c-once` 属于单独的 `onekvm-video-ax630c-diag` 包，可在独占编码器的条件下验证完整 H.264 输出。不要在 Core 正在采集时运行它。

## 输入模式切换

EDID 使用库级 ABI，不依赖采集会话。查询只读 `/proc/lt6911_info/edid_snapshot`，
避免读取 `edid` 引起桥片复位。写入接受 256 字节，由内核保留固件版本、回读校验
并热插拔生效，无需整机重启。

LT6911UXC 的 HDMI 接收尺寸和 CSI 有效像素可能在不同时刻就绪。后端每 100 ms
检查桥片发布的模式；VIN 停帧时立即复查。确认新尺寸后重建 DMA 环并输出 IDR，
输出尺寸未变时保留编码器。只读 `rx_geometry` 是候选尺寸，必须连续确认并由
正式发布尺寸纠正；没有真实 VIN 帧时不能将候选视为已生效。

模式切换的无信号宽限为 3.2 秒，避免普通切换插入占位 IDR。真实断信号仍输出
占位图。H.264 VUI 声明 `max_num_reorder_frames=0`、`max_dec_frame_buffering=2`，
与编码器只输出显示顺序 I/P 帧的行为一致。

TC358743 可在稳定 `QUERY_DV_TIMINGS` 后重开采集；LT6911UXC 还需等待 CSI 像素。
提前停 VIN、重复武装 CSI 或提高寄存器轮询频率不能替代这一步。寄存器访问必须
受驱动锁保护，禁止用户态与内核驱动并发切换寄存器页。

桥片固件分析、差异镜像和读写工具由 `onekvm-firmware-lt6911uxc` 维护。固件优化
必须识别板型和原始版本，保留 EDID/校准数据，写前备份并完整回读校验；设备实验
和逐档测量不属于本流水线规范。
