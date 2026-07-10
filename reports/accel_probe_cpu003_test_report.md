# tirtc_accel_device_probe CPU 0.03 测试报告

测试时间：2026-07-10

## 测试条件

- 镜像：`docker-hub.tange365.com/runtime/tirtc-accel-probe-runner:test`
- 网关脚本：`script/run_accel_probe_with_netem_gateway.sh`
- CPU 限制：`CPU_LIMIT=0.03`
- netem 丢包：`LOSS=0`
- netem 延迟：`DELAY_MS=0`
- probe 网段：`172.45.0.0/24`
- uplink 网段：`172.46.0.0/24`
- endpoint：`http://ep-test-tirtc.tange365.com`
- peer：`whips://echo?device_id=TESTZHUOHY00`

## 网关与网络条件

网关路由已修正为双网卡模式：

```text
default via 172.46.0.1 dev eth1
172.45.0.0/24 dev eth0 proto kernel scope link src 172.45.0.2
172.46.0.0/24 dev eth1 proto kernel scope link src 172.46.0.2
```

probe 路由：

```text
default via 172.45.0.2 dev eth0
172.45.0.0/24 dev eth0 proto kernel scope link src 172.45.0.3
```

本次 `DELAY_MS=0`、`LOSS=0`，所以网关 uplink 网卡的 qdisc 为：

```text
qdisc noqueue 0: root refcnt 2
```

connect 测试前 ping：

```text
4 packets transmitted, 4 received, 0% packet loss
rtt min/avg/max/mdev = 18.161/18.530/19.113/0.389 ms
```

audio 测试前 ping：

```text
4 packets transmitted, 4 received, 0% packet loss
rtt min/avg/max/mdev = 17.318/66.666/213.325/84.674 ms
```

## connect 结果

```text
connect_success: 5/5 100.00%
connect_cost: count=5 avg=1769.50ms p50=1227.13ms p90=4002.70ms p95=4002.70ms p99=4002.70ms
```

结论：在 CPU 只给 0.03 核时，连接仍能成功，但首轮耗时明显偏高，整体连接耗时受 CPU 调度影响较大。

## audio 10 轮最终汇总

```text
音频多轮汇总: 成功轮次=10/10
音频包数多轮汇总: 设备发送=4998 发送失败=0 服务端收到=4118 设备收到回声=4118 服务端收包率=82.39% 回声收包率=82.39%
音频首包上行延迟(设备到服务端): 样本数=10 平均=103.59ms 中位数=86.09ms P90=224.81ms P95=317.34ms P99=317.34ms
音频首包回声总延迟(设备发出到收到回声): 样本数=10 平均=172.75ms 中位数=166.35ms P90=302.50ms P95=398.91ms P99=398.91ms
音频卡顿次数多轮汇总: 样本数=10 平均=1.90 中位数=2 P90=3 P95=4 P99=4
音频卡顿时长多轮汇总: 样本数=10 平均=1038.65ms 中位数=860.61ms P90=1939.75ms P95=2488.40ms P99=2488.40ms
音频卡顿占比多轮汇总: 样本数=10 平均=9.43% 中位数=7.75% P90=17.63% P95=22.62% P99=22.62%
音频下行延迟多轮汇总(服务端到设备): 样本数=4118 平均=72.07ms 中位数=73.24ms P90=131.36ms P95=176.44ms P99=264.69ms
```

## 服务端收到少于设备发送的定位

本次不是 netem 丢包导致：

- `LOSS=0`，且 ping 是 `0% packet loss`。
- `DELAY_MS=0`，qdisc 显示 `noqueue`，没有注入延迟/丢包。
- probe 路由确认流量经过 netem 网关。

更可能的原因是 CPU 限制过低导致发送侧/SDK 媒体线程调度不足：

- 设备侧 `发送失败=0`，说明 `TiRtcSendAudioStream` 调用层面没有返回失败。
- 服务端只观测到 `4118/4998`，收包率 `82.39%`，说明“SDK 接受发送调用”和“服务端实际观测到媒体帧”不是同一个指标。
- 第 4 轮设备只发送了 `498` 包，而理论值是 `10s / 20ms = 500` 包，说明在 `CPU_LIMIT=0.03` 下，probe 的发送循环本身已经出现调度不足。
- 日志中多次出现 `RTC_THREAD_STAT`，且多轮出现明显卡顿：平均卡顿时长 `1038.65ms`，平均卡顿占比 `9.43%`。

结论：`CPU_LIMIT=0.03` 已经低到会影响音频发送节奏和 RTC 内部线程调度。`发送失败=0` 不能代表服务端必然收到；当前缺口主要是 CPU 饥饿导致的媒体链路实际出包/处理不足。

## 原始日志

- `reports/accel_probe_cpu003_connect.log`
- `reports/accel_probe_cpu003_audio_10.log`
- `reports/accel_probe_cpu003_smoke.log`
