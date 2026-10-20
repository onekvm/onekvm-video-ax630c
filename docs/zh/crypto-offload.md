# AX630C 加密 offload ABI

`ax630c.so` 同时导出 video ABI 与
[`onekvm_crypto_backend_query`](../../../onekvm-core/include/onekvm/crypto_backend_v1.h)。
复用 Core 的 Crypto Backend ABI v1，不增加 Pro 专用的应用协议。
应用先检查 ABI major、`struct_size`、feature 和函数指针，再调用 `probe`。
完整调用约定见 [Core ABI 文档](../../../onekvm-core/docs/crypto-backend-abi-v1.md)。

## 算法与边界

| 能力 | 实现 | 限制 |
| --- | --- | --- |
| AES-CTR | EIP-130 硬件 AES | 128/192/256-bit key；完整 16-byte 大端 counter |
| AES-GCM TX | EIP-130 批量 AES + OpenSSL GHASH | 128/256-bit key；12-byte nonce；16-byte tag |
| GCM batch | 合并多个包的 counter blocks，一次 ECB 提交 | 最多 32 项；`completed` 只报告成功前缀 |

GCM 是部分硬件卸载，不声明 EIP-130 原生 GCM 或硬件 GHASH。
厂商公开 token 接口缺少完整的 GCM AAD/tag 布局，不能把 GCM 枚举当作可用实现。
不声明 HMAC-SHA1。descriptor 的 `srtp-tx-offload` 与 `CONCURRENT_H265_VIDEO`
用于 Core 的 H.264/H.265 SRTP TX；105 已通过浏览器 H.264 播放及 90 秒 H.265
WebRTC/加密并发验证，接收 5427 帧，无序号缺口、完整码流解码零错误。

每个 session 由硬件计算 `H = AES_K(0)`。每个包的 `J0` 和递增 counter 组成一个
连续的 ECB 输入，多个包合并提交，输出仅作为 AES keystream。OpenSSL 的 GCM
构造使用自定义 block callback，该 callback 只读取已经完成的硬件结果，不调用
软件 AES。无法匹配的 counter 或失败的硬件提交直接返回错误。常见 8×1200-byte
RTP 批次只提交一次 AES；超过 65552-byte staging 容量的 batch 分组提交。

单个 CTR 输入最多 65536 bytes；GCM 的 AAD 与明文合计最多 65536 bytes。
支持任意字节长度、非对齐地址和完全原地处理，`payload_alignment=1`。
CTR 空输入直接成功；GCM 空明文仍输出认证标签。AES key 只保存在内核 keyed
transform 中；派生的 H、GHASH 表和临时 keystream 属于秘密材料。每次返回前清除
counter/keystream staging，销毁时清理 H、GCM context、缓冲区并关闭所有 fd。

`flags` 必须为零；不支持的算法、密钥、nonce、tag、地址、大小在提交之前拒绝。
失败时 `output_size=0`；调用方丢弃失败输出。batch 失败后不能使用失败项及其后续项。
底层使用同步 ioctl，一次调用对应完整请求，不存在未读 socket record。
失败请求不在同一次调用内重试；恢复后的新请求复用原 fd 和内核 keyed transform。
Core 丢弃失败批次，等待后再次尝试硬件；恢复期间不执行软件替代。
同一个 session 不可并发调用；不同 session 可以并发，由内核串行调度共享引擎。
nonce/counter 由调用方分配，同一把密钥下不得重用；插件不分配协议序号。

## 内核与权限

Linux 6.18 驱动在 `linux-6.18-ax620e/drivers/crypto/axera/`。
配置 `CONFIG_CRYPTO_DEV_AXERA_EIP130=m`，设备树匹配 `axera,ax630c-eip130`。
固件包 `axera-eip130-firmware` 安装 `axera/eip130.bin`。

应用通过 `/dev/axera-eip130` 的版本化 ioctl UAPI 调用硬件，权限为 root `0600`。
生产路径和诊断均不使用 AF_ALG。标准内核 Crypto API 仍注册
`axera-eip130-ecb`、`axera-eip130-cbc` 和 `axera-eip130-ctr`，原生接口按精确驱动名
选择 ECB/CTR，不按通用算法名选择，不允许软件 AES 替代。
UAPI 的唯一源文件为
[`linux/axera_eip130.h`](../../../linux-6.18-ax620e/include/uapi/linux/axera_eip130.h)，
OE 编译任务从该源码导入，并将文件哈希计入任务签名。

| 命令 | 输入/输出 | 约定 |
| --- | --- | --- |
| `AXERA_EIP130_SET_KEY` | 48-byte `axera_eip130_key` | ABI 1、ECB/CTR、16/24/32-byte key；每个 fd 只允许设置一次 |
| `AXERA_EIP130_RUN` | 56-byte `axera_eip130_request` | 最多 65552 bytes；同步完成；仅 `output_size` 为输出字段 |

地址是进程地址，不是物理/DMA 地址；兼容 32-bit 调用的结构布局使用 `__aligned_u64`。
保留字段必须为零；ECB 为整块、无 IV；CTR 使用完整 16-byte 大端 IV。
支持非对齐地址和原地处理；输入先复制到内核自有缓冲，SG 分页构造同时兼容
kmalloc/vmalloc，再由 crypto_engine 使用隔离的持久 DMA staging。
每个 fd 用互斥锁串行处理，多个 fd 由同一 crypto_engine 调度；请求完成后擦除缓存。
密钥、请求和秘密缓冲在关闭 fd 时清理。未知命令返回 `ENOTTY`；重复设密钥为
`EBUSY`；非法元数据、地址分别为 `EINVAL`、`EFAULT`；未设密钥为 `ENOKEY`。
加密错误时不复制硬件输出，`output_size=0`；调用方必须检查返回值和输出长度。
不提供 `/dev/mem`、用户物理地址、原始 token 或厂商 glibc 库接口。

一个 crypto_engine 调度 mailbox 2；完成 IRQ 唤醒等待者。两个持久 DMA 缓冲区将
调用者内存与 DMA 隔离，不在等待期间忙轮询。CTR 的末块按 16 bytes 补零、只返回有效
字节；跨越硬件低 32-bit counter 边界时拆分请求并补上 128-bit carry。
token/固件载荷使用 relaxed MMIO，由后续 doorbell 统一排序；AES 数据通过 DMA
返回，IRQ 仅读取状态/序号字，不逐字读取全部 64-word 响应。
关机先停止接收新请求、排空队列，再与 worker/recovery 互斥地复位和关钟。

100 ms 未收到完成事件时，驱动屏蔽中断、同步 IRQ、复位包括 AXI master 在内的引擎，
将引擎置为 poisoned。后续请求返回错误，DMA 缓冲区不会被后续操作复用。
root 向同一平台设备的 `recover` 写入 `1`，可在工作线程互斥锁内重新初始化引擎；
重新初始化失败时保持隔离。Pro 插件遇到 EIO/ETIMEDOUT 时请求此受控恢复，
原请求仍返回错误；恢复后的新请求继续使用原 fd 和 keyed transform。此方式不依赖模块卸载功能。
`/sys/bus/platform/devices/4900000.crypto/stats` 提供请求、字节、
错误、超时、poisoned、提交/完成、异常 IRQ 和恢复次数；平台设备名称以实际 sysfs 为准。
root 可向 `/sys/module/axera_eip130/parameters/fault_timeout_once` 写入 1，测试一次
丢弃完成中断后的超时隔离；该接口只用于测试。

## 构建与验证

设备产物只通过 `onekvm-distro` OE/kas 构建；调试使用
`ONEKVM_DEBUG_WORKTREES=linux-6.18-ax620e,onekvm-video-ax630c`。
`onekvm-video-ax630c-diag` 的 `ax630c-crypto-test` 会动态装载实际共享库，
与 OpenSSL 对照不同密钥、长度、AAD 和原地处理，验证 counter carry、batch 部分失败、
错误标签/AAD 拒绝、跨 staging 分组的 32 项混合 batch 及四个并发 session，
再输出同步请求微基准。`ax630c-eip130-ioctl-test` 验证原生 UAPI 的元数据、
坏地址、错误后输出长度、原地处理与同一 fd 并发；显式传 `--fault` 才注入超时，
验证保持隔离、拒绝后续请求及同一 fd 在受控恢复后继续工作。
105 还通过禁止整个诊断进程及其线程使用 `socket()` 的 seccomp 验证，确认无需 AF_ALG。

`onekvm-crypto-diag` 提供 `/usr/libexec/onekvm/onekvm-crypto-bench`：

```sh
onekvm-crypto-bench --mode software --payload 1200 --batch 8 --mbps 20 --seconds 10
onekvm-crypto-bench --mode hardware --payload 1200 --batch 8 --mbps 20 --seconds 10
```

软件路径使用 SRTP 相同的 Rust `aes-gcm`；硬件路径装载实际 ABI，不允许回落。
两者固定 AES-128、12-byte AAD 和 16-byte tag，计时前后校验密文/认证；
`--mbps 0` 测饱和吞吐。固定 CPU governor、频率、包长、批次和目标码率，交替多轮测试，
单独采集空闲基线。`system_cpu_percent` 是全部 CPU 的 `/proc/stat` 忙碌比例，
包括 worker、IRQ 和背景服务；`process_cpu_one_core_percent` 只包含测试进程，
不能代表硬件卸载的总 CPU。延迟分位数按整个 batch 记录。

`tools/crypto-stage-profile.py` 用相同操作频率比较 GCM、CTR(N+16) 和短 CTR，
记录调用进程/crypto_engine 线程 CPU、IRQ 和 IPI。它用于区分 GHASH、数据搬运和
每次提交的固定成本；Python 自身开销不作为正式软硬件 CPU 对照。
修改实现后须重复实际 SRTP 接收、解密、视频解码和 H.265 并发验证；
不能只凭进程 CPU 或单独加密吞吐声称音频总 CPU 达到 5%。
105 的固定码率、饱和吞吐和整机 CPU 三轮对照见
[Pro 硬件加密](../../../onekvm-distro/docs/nanokvm-pro-crypto.md#验证与性能测量)。
