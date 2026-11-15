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
              [--timeout 15] [--out FILE.sr] [--overwrite] [--open]
              [--demo-pattern random]              # demo 驱动专用
u3ctl open FILE.sr                                 # 推送给已运行的 GUI 实例
u3ctl capture-start [capture 参数]                  # 流式:立即返回 job(<1s)
u3ctl capture-status [--job ID]                    # 进度/触发/存活(只读)
u3ctl capture-stop  [--job ID] [--open]            # 优雅停止→最终 JSON
u3ctl capture-list                                 # 全部 job(含 interrupted)
u3ctl session info FILE.sr                         # 采样率/样本数/时长/通道/触发
u3ctl sessions list [--dir DIR] / session delete FILE.sr
u3ctl activity FILE.sr [--bucket N] [--channels D0,D1]   # 每桶翻转数→哪段有信号
u3ctl measure FILE.sr [--channels ...] [--from --to]     # 频率/占空比/脉宽统计/首末边沿
u3ctl edges FILE.sr --channels D0 [--type rising] [--max 200]  # 精确边沿时刻表
u3ctl sample FILE.sr --at 8352                     # 单点各位值
u3ctl slice FILE.sr --from A --to B [--format hex|bits|base64]  # ≤64K 样本窗口
u3ctl overview FILE.sr [--points 2000]             # 每桶 min/max/toggles 粗轮廓
u3ctl profile FILE.sr                              # 自动画像:通道角色+协议候选+活动窗
u3ctl render FILE.sr --out wave.png [--channels D0,D1] [--from --to]
      [--width 1600] [--row-height 40] [--format png|svg]   # 波形图(PNG/SVG)
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
  → profile(通道角色/协议候选/活动窗,冷启动首选)
  → decode(全窗口,看 total_rows)→ --search/--class 收敛异常
  → measure/edges 拿频率与边沿 → render 出波形图 → 结论(附 .sr 路径)
```

`.sr` 文件可直接被 GUI(`U3LogicAnalyzer -i file.sr`)打开回看。

### 在已有 GUI 实例中打开(--open / open)

GUI 常驻时,`capture --open` 或 `u3ctl open FILE.sr` **不会新开窗口**:CLI
直连运行中实例的本地 IPC 通道(`QLocalServer`,路径 `/tmp/u3la-open-<uid>.sock`),
GUI 在自己的窗口里新开一个会话页并回 JSON:

```json
{"file":"captures/x.sr", ..., "gui":"existing-instance"}
```

没有 GUI 在运行时返回 `{"gui_opened":false,"reason":"no running GUI instance"}`
且采集结果照常落盘;GUI 未启动时也可用 `U3LogicAnalyzer --open-existing FILE.sr`
(已有实例则转发给它,没有则以自己加载该文件启动)。macOS 的 Finder 双击 /
`open -a` 打开文档事件走同一条路(`QFileOpenEvent`)。Windows 命名管道客户端待接。

## 测试

```bash
python3 tools/u3ctl/tests/make_uart_sr.py captures/uart.sr 1000000 100000 0x55 0x33 0xAA
build_u3ctl/u3ctl decode captures/uart.sr --stack uart --channel tx=D0 \
    --pdopt baudrate=100000        # → decoded bytes: ['55', '33', 'AA']
```

## 流式采集(附录 C)

**先布防、后触发**:采集跑在"上电 / 复现故障 / 人机对话"期间。worker 与一次性
capture 共用同一管线;状态即磁盘(`captures/.job-<id>.json`,原子写);
SIGTERM 优雅停止是唯一保证 `.sr` 完整的出口,kill -9 的数据如实标记
`interrupted` 不抢救。默认安全超时 600s(`--timeout` 覆盖);不给
`--samples/--time` 则跑到 stop。设备独占:活跃 job 期间二次 start 报 busy。

验证:`python3 tools/u3ctl/tests/test_streaming.py`(V1–V10 demo 矩阵,
V1 启动 <1s 硬门槛;V10 需 GUI 在跑,否则跳过)。

## MCP 原生调用(P3)

`tools/mcp/u3la_mcp/server.py` 是**零依赖**的 MCP stdio server(手写
JSON-RPC 子集,任何 CPython ≥3.9 可跑,无需 pip),把 u3ctl 的 17 个命令
映射为原生 MCP 工具:`u3_list_devices / u3_capabilities / u3_capture /
u3_session_info / u3_profile / u3_activity / u3_measure / u3_edges /
u3_sample / u3_slice / u3_overview / u3_decode / u3_list_decoders /
u3_decoder_info / u3_render / u3_export / u3_open_in_gui`。工具描述内嵌了
全部护栏(USB 独占、软件触发、截断契约、PD 选项匹配)。

**`u3_render` 返回 image content block**——多模态 agent 直接"看"波形,
配合 decode 的样本坐标实现"解出异常→放大看图"闭环。

已注册(本机):
- **ZCode**:`~/.zcode/cli/config.json` → `mcp.servers`(下次会话生效,
  工具名 `mcp__u3-logic-analyzer__u3_capture` 等)
- **Codex**:`~/.codex/config.toml` → `[mcp_servers.u3-logic-analyzer]`
- **OpenCode**:`~/.config/opencode/opencode.json` → `mcp.u3-logic-analyzer`

其它客户端通用注册:

```json
{ "mcpServers": { "u3-logic-analyzer": {
    "command": "<repo>/tools/mcp/u3la_mcp/launch.sh", "args": [] } } }
```

协议已验证:initialize/tools/list/tools/call 全通,真机数据上
session_info/profile/decode/render(image block)端到端通过。

## 真机(macOS)注意
- 驱动名 `wch-ch32h417-libusb`;GUI 正在采集时设备被独占,`capture` 会报错并提示。
- 触发为**软件触发**(0/1/上升/下降/边沿,支持多级顺序);"按协议内容触发"
  不存在——全量采集后用 `decode --search` 事后定位。
- 电压阈值范围按硬件批次不同(0.6–1.5V 或 0.8–2.3V),`capabilities` 可查。
