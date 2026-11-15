---
name: u3-logic-analyzer
description: Operate the WeAct U3 logic analyzer (CH32H417) headlessly via u3ctl — capture digital/analog signals with triggers, decode protocols (I2C/SPI/UART/…, 132 decoders), export waveforms, and push captures into the running GUI. Use when the user asks to capture/采集/抓包 signals, analyze protocol traffic, decode 波形, measure signals, or when debugging embedded buses; also for logic analyzer / 逻辑分析仪 / PulseView related requests.
---

# u3ctl — U3 逻辑分析仪无头操控

`u3ctl` 直接驱动 libsigrok/libsigrokdecode 完成采集→解码→导出，全程 JSON 输出，
无需打开 GUI。所有命令:成功输出单个 JSON 对象；失败输出 `{"error":"...","command":"..."}`
并以退出码 2 结束——用退出码判断成败，用 `jq`/`python3 -m json.tool` 解析结果。

## 二进制与构建

```
BIN=/Users/kaiiii/Documents/Projects/LogicAnalyzer/WeActStudio.U3LogicAnalyzer/build_u3ctl/u3ctl
REPO=/Users/kaiiii/Documents/Projects/LogicAnalyzer/WeActStudio.U3LogicAnalyzer
```

BIN 不存在时在 $REPO 下构建(约 1 分钟):`cmake -B build_u3ctl -S tools/u3ctl && cmake --build build_u3ctl`。
依赖仓库的 `install/`(build.sh 步骤 1-3 的产物),rpath 已内置,无需环境变量。

## 命令面

```bash
$BIN devices list [--conn SUBSTR]                     # 枚举设备(真机 + demo)
$BIN capabilities [--driver wch-ch32h417-libusb]      # 采样率表/触发类型/阈值范围/配置能力
$BIN capture [--driver demo|wch-ch32h417-libusb]
      [--rate 16M] [--channels 8] [--samples 1M | --time 50ms]
      [--trigger "D0:rising+D1:high"] …               # 重复 --trigger = 多级顺序触发
      [--pre 10] [--threshold 1.2V] [--timeout 15]
      [--out captures/x.sr] [--overwrite] [--open]    # --open: 推给运行中的 GUI
$BIN session info FILE.sr                             # 采样率/样本数/时长/通道/触发位置
$BIN profile FILE.sr                                  # 自动画像:通道角色+协议候选+活动窗
$BIN activity FILE.sr [--bucket N] [--channels D0,D1] # 每桶翻转数
$BIN measure FILE.sr [--channels ...] [--from --to]   # 频率/占空比/脉宽/首末边沿
$BIN edges FILE.sr --channels D0 [--type rising] [--max 200]
$BIN sample FILE.sr --at 8352 / slice --from A --to B --format hex|bits|base64 (≤64K)
$BIN overview FILE.sr [--points 2000]                 # 每桶 min/max/toggles
$BIN render FILE.sr --out wave.png [--channels ...] [--width 1600] [--format png|svg]
$BIN decode FILE.sr --stack i2c[,eeprom24xx]          # 堆叠解码
      [--channel scl=D0 --channel sda=D1] [--pdopt k=v]
      [--from 8k --to 12k] [--class start,ack] [--search NACK] [--max-rows 500]
$BIN decoders list [--search i2c] / decoders info PD  # 132 个解码器检索与详情
$BIN export FILE.sr --format csv|vcd|hex|bits|binary|wav --out FILE
$BIN capture-start [capture 参数] --out x.sr         # 流式布防:立即返回 job(<1s)
$BIN capture-status [--job ID]                       # 进度(只读,不碰设备)
$BIN capture-stop [--job ID] [--open]                # 优雅停止→最终 JSON
$BIN capture-list                                    # 全部 job
$BIN open FILE.sr                                     # 推送给已运行的 GUI 实例
```

单位:统一 SI 后缀(`16M`/`512K`/`1Mi`/`50ms`/`1.2us`);窗口 `--from/--to`
接受样本号或时间。decode 行内同时给出样本号与秒,可直接串联后续命令。

## 标准分析闭环(按序执行)

1. `devices list` — 看真机是否在线;不在就问用户要不要 demo 模式
2. `capabilities` — 取采样率档位/触发类型,别猜
3. `capture` — 采样率 ≥ 目标信号带宽 ×5;样本数给足;需要起始定位就配 `--trigger`
4. `session info` — 确认实际采到多少
5. `profile` — 冷启动首选:直接给出通道角色(时钟/数据)、协议候选、活动窗口
6. `decode --stack <pd>` — 先全窗口跑,看 `total_rows`;>0 再用
   `--search`/`--class`/`--from --to` 收敛到关注点
7. `measure`/`edges` 拿频率、占空比、精确边沿;`render` 出 PNG/SVG 波形图;
   `slice`/`sample` 拿位级数据
8. 结论里附 `.sr` 路径;`--open` 或 `open` 可把波形推到用户桌面的 GUI 会话页

冷启动不知道是什么协议时:`profile` 的候选 + guess_bitrate 猜测波特率;
候选解不通再用 `--search` 逐个试(`i2c`/`spi`/`uart`/`can`),哪个 total_rows
高且事件连贯就用哪个。`--driver demo` 只用于验证命令链路。

## 硬约束(违反会误判)

- **USB 独占**:GUI 正在采集/占用设备时 capture 报 open 失败——提示用户停掉 GUI
  采集后重试,不要反复重试。
- **只有软件触发**(0/1/上升/下降/边沿,多级顺序):"按协议内容触发"(如某地址)
  不存在。要定位协议事件:全量采集 → `decode --search` → 缩窗下钻。
- **截断护栏**:decode 输出带 `total_rows/returned/truncated`——`truncated:true`
  时务必先缩窗或加过滤,别拿截断当全量下结论。
- **PD 选项与波形参数必须一致**:`--pdopt baudrate=100000` 必须匹配真实波特率
  (解码器默认值通常不是),否则解出乱码。
- **真机采样率档位不连续**(如 4–200MHz 共 25 档、按 USB 速度与通道数变化),
  用 `capabilities` 查实际可用值。
- **demo 驱动**(`--driver demo`)无硬件可测全流程,适合先验证命令链路;
  demo 波形是随机/图案信号,decode 出的内容无真实协议意义。
- 阈值范围按硬件批次不同(0.6–1.5V 或 0.8–2.3V),先查 `capabilities`。

## 流式采集:何时用、怎么用

**默认用一次性 capture**。只有"事件何时发生不可知/要靠人制造"才用三步式:
- 抓上电瞬间:先 `capture-start` → 让用户上电 → 观察 status 样本增长 → `capture-stop`
- 等罕见故障:`capture-start --trigger <条件> --samples <深度> --timeout 600` 挂后台
- 人机协作:布防后与用户对话,复现完成再 stop,然后 decode/render
注意:不给 --samples/--time 则跑到 stop;kill -9 的数据标记 interrupted 不可用,
SIGTERM(即 capture-stop)是唯一保证 .sr 完整的出口;设备独占,活跃 job 时二次 start 报 busy。

## 无 GUI 打开方式

`capture --open` / `open FILE.sr` 会把波形推到**已运行**的 GUI 实例(不开新窗口),
返回 `"gui":"existing-instance"`;GUI 没开则返回
`"gui_opened":false,"reason":"no running GUI instance"`(文件仍在磁盘,不算失败)。
也可以让用户自己跑 `U3LogicAnalyzer -i FILE.sr`。

## 排错

- `driver not found`:build.sh 步骤 1-3 没跑全,或 install/ 缺失 → 重建
- decode 报 `srd_init failed` / decoder not found:`$SIGROKDECODE_DIR` 被覆盖成
  错误值 → unset 或指向 $REPO/install/share/libsigrokdecode/decoders
- capture 0 样本即结束:检查触发条件是否过严(软件触发等不到就超时/直接结束)
- 全部命令加 `-v` / `-vv` 看 libsigrok 日志(stderr,不污染 JSON)
