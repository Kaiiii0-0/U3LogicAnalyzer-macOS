# u3ctl — U3 逻辑分析仪无头 CLI(Agent 操控入口)

`u3ctl` 是 [agent-control-plan.md](../../agent-control-plan.md) 落地的 P1 无头核心:
不经 GUI,直接驱动 libsigrok / libsigrokdecode,完成 **采集 → 保存 → 解码 → 导出**
全链路,所有命令输出 JSON,供 AI Agent(ZCode/Claude 等)经 Bash 或 MCP 调用。

## 构建

```bash
./build.sh                 # 第 5 步构建 u3ctl(--no-u3ctl 跳过)
# 或单独构建(需先跑过 build.sh 步骤 1-3,产出 install/):
cmake -B build_u3ctl -S tools/u3ctl && cmake --build build_u3ctl
```

二进制:`build_u3ctl/u3ctl`(rpath 已指向 `install/lib`,无需环境变量;
解码器路径自动解析 `$SIGROKDECODE_DIR` → `install/share/libsigrokdecode/decoders`)。

## 命令一览(P1)

```bash
u3ctl devices list [--conn SUBSTR]                 # 枚举设备(含 demo)
u3ctl capabilities [--driver NAME] [--conn SUBSTR] # 采样率表/触发类型/阈值/配置能力
u3ctl capture [--driver demo|wch-ch32h417-libusb] [--conn SUBSTR]
              [--rate 16M] [--channels 8] [--samples 1M | --time 50ms]
              [--trigger "D0:rising+D1:high"] …    # 重复 --trigger = 顺序多级
              [--pre 10] [--threshold 1.2V] [--mode logic|adc]
              [--timeout 15] [--out FILE.sr] [--overwrite]
              [--demo-pattern random]              # demo 驱动专用
u3ctl session info FILE.sr                         # 采样率/样本数/时长/通道/触发
u3ctl sessions list [--dir DIR] / session delete FILE.sr
u3ctl decode FILE.sr --stack i2c[,eeprom24xx]      # 堆叠解码
              [--channel scl=D0 --channel sda=D1]  # PD 通道 → 设备通道
              [--pdopt k=v] [--from 8k --to 12k] [--class start,ack]
              [--search TEXT] [--max-rows 500] [--binary payload.bin]
u3ctl decoders list [--search SUBSTR] / decoders info PD
u3ctl export FILE.sr --format csv|vcd|hex|ascii|bits|binary|wav --out FILE
```

## JSON 契约

- 成功:单个 JSON 对象到 stdout;失败:`{"error": "...", "command": "..."}` + 退出码 2。
- 数量/时间参数统一 SI 后缀:`16M`、`512K`、`1Mi`、`50ms`、`1.2us`。
- `decode` 输出 `total_rows / returned / truncated`,防上下文爆炸;
  窗口 `--from/--to` 接受样本号或时间,行内同时给样本号与秒。

## Agent 标准闭环

```
devices list → capabilities → capture → session info
  → decode(全窗口,看 total_rows)→ --search/--class 收敛异常
  → 缩小窗口再 decode/export 深挖 → 结论(附 .sr 路径供 GUI 回看)
```

`.sr` 文件可直接被 GUI(`U3LogicAnalyzer -i file.sr`)打开回看。

## 测试

```bash
python3 tools/u3ctl/tests/make_uart_sr.py captures/uart.sr 1000000 100000 0x55 0x33 0xAA
build_u3ctl/u3ctl decode captures/uart.sr --stack uart --channel tx=D0 \
    --pdopt baudrate=100000        # → decoded bytes: ['55', '33', 'AA']
```

## 真机(macOS)注意

- 驱动名 `wch-ch32h417-libusb`;GUI 正在采集时设备被独占,`capture` 会报错并提示。
- 触发为**软件触发**(0/1/上升/下降/边沿,支持多级顺序);"按协议内容触发"
  不存在——全量采集后用 `decode --search` 事后定位。
- 电压阈值范围按硬件批次不同(0.6–1.5V 或 0.8–2.3V),`capabilities` 可查。
