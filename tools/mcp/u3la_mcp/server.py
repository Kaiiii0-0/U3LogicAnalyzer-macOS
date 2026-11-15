#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
u3la-mcp — MCP stdio server exposing the U3 logic analyzer (u3ctl) as
native agent tools. Zero third-party dependencies: implements the MCP
stdio transport subset (initialize / tools/list / tools/call) over
newline-delimited JSON-RPC 2.0. Runs on CPython >= 3.9.

Every tool wraps one u3ctl subcommand. Results come back as text
content (the u3ctl JSON, pretty-printed); u3_render additionally
returns the waveform as an image content block so multimodal agents
can look at the capture directly.

Guardrails encoded in the tool descriptions:
- the device is exclusive: stop the GUI capture before u3_capture
- triggers are software-only (0/1/edges, sequential stages); protocol
  content triggers do not exist — capture everything and search
- decode/slice answers carry truncation flags; trust them
"""

import base64
import json
import os
import subprocess
import sys
import tempfile

PROTOCOL_VERSION = "2024-11-05"
SERVER_NAME = "u3-logic-analyzer"
SERVER_VERSION = "0.3.0"

REPO = os.environ.get(
    "U3LA_REPO",
    os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", ".."),
)
REPO = os.path.abspath(REPO)
U3CTL = os.environ.get(
    "U3CTL_BIN", os.path.join(REPO, "build_u3ctl", "u3ctl")
)

# Real-capture examples used in tool descriptions.
DEMO = "demo"

# ---------------------------------------------------------------------------
# Tool definitions. Each: name, description, input schema, argument mapping.
# ---------------------------------------------------------------------------

BOOL = {"type": "boolean"}
STR = {"type": "string"}
NUM = {"type": "number"}

TOOLS = [
    {
        "name": "u3_list_devices",
        "description": (
            "List connected logic analyzer devices (real CH32H417 via USB "
            "plus the virtual demo device). Returns driver name, USB conn "
            "id, serial, and channel names. Run this first."
        ),
        "inputSchema": {"type": "object", "properties": {}},
    },
    {
        "name": "u3_capabilities",
        "description": (
            "Query what a device can do: available sample rates (they are "
            "discrete, do not guess), trigger match types, voltage "
            "threshold range, and which config keys are get/set/list-able. "
            "Check here before setting rates or thresholds."
        ),
        "inputSchema": {
            "type": "object",
            "properties": {
                "driver": {"type": "string", "description": "Driver name, e.g. wch-ch32h417-libusb or demo"},
                "conn": {"type": "string", "description": "Substring of the USB conn id to disambiguate"},
            },
        },
    },
    {
        "name": "u3_capture",
        "description": (
            "Capture logic data into a .sr session file (blocking, bounded "
            "by timeout). THE DEVICE IS EXCLUSIVE: if the GUI is capturing, "
            "open fails with a busy error — ask the user to stop it. "
            "Triggers are SOFTWARE-only: CH:high|low|rising|falling|edge, "
            "'+' joins same-stage conditions, repeating the trigger list "
            "creates sequential stages. Protocol-content triggers (e.g. "
            "'trigger on I2C address 0x50') DO NOT EXIST — capture a "
            "window around the traffic and use u3_decode --search "
            "instead. Sample rate must be one of u3_capabilities' rates. "
            "SI units accepted: --rate 16M, --samples 1M, --time 50ms. "
            "Pass open=true to push the finished capture into the running "
            "GUI for the user to see."
        ),
        "inputSchema": {
            "type": "object",
            "properties": {
                "driver": {"type": "string", "description": "wch-ch32h417-libusb (default) or demo"},
                "conn": {"type": "string"},
                "rate": {"type": "string", "description": "Sample rate, SI suffix ok (16M). Use u3_capabilities list."},
                "channels": {"type": "integer", "description": "First N logic channels to enable (1-16)"},
                "samples": {"type": "string", "description": "Total samples (1M). Unlimited if omitted — then timeout stops it."},
                "time": {"type": "string", "description": "Capture duration (50ms, 2s)"},
                "trigger": {"type": "array", "items": {"type": "string"}, "description": "Stage specs, e.g. [\"D0:rising+D1:high\", \"D2:falling\"] = two sequential stages"},
                "pre": {"type": "integer", "description": "Pre-trigger ratio percent (0-100)"},
                "threshold": {"type": "number", "description": "Logic voltage threshold in volts; check capabilities for the range"},
                "mode": {"type": "string", "description": "logic or adc"},
                "timeout": {"type": "integer", "description": "Hard stop after N seconds (default 30)"},
                "out": {"type": "string", "description": "Output .sr path (default captures/<timestamp>.sr)"},
                "overwrite": BOOL,
                "demo_pattern": {"type": "string", "description": "demo driver only: random|sigrok|incremental|..."},
                "open": {"type": "boolean", "description": "Push the capture into the running GUI instance"},
            },
        },
    },
    {
        "name": "u3_capture_start",
        "description": (
            "STREAMING capture (plan C): arms a capture in a detached "
            "worker and returns a job id at once (<1s), so the capture "
            "can run across a whole conversation — e.g. arm first, THEN "
            "have the user power the board or reproduce a fault, then "
            "u3_capture_stop. Same arguments and device rules as "
            "u3_capture (exclusive USB; sample rates from "
            "u3_capabilities). Without samples/time it runs until "
            "stopped (default safety timeout 600s). Poll "
            "u3_capture_status; finish with u3_capture_stop. Prefer "
            "plain u3_capture for fixed windows — it is simpler."
        ),
        "inputSchema": {
            "type": "object",
            "properties": {
                "driver": {"type": "string"},
                "conn": {"type": "string"},
                "rate": {"type": "string"},
                "channels": {"type": "integer"},
                "samples": {"type": "string", "description": "Auto-stop after N samples"},
                "time": {"type": "string", "description": "Auto-stop after e.g. 2s"},
                "trigger": {"type": "array", "items": {"type": "string"}},
                "pre": {"type": "integer"},
                "threshold": {"type": "number"},
                "timeout": {"type": "integer", "description": "Safety limit seconds (default 600)"},
                "out": {"type": "string"},
                "overwrite": BOOL,
                "demo_pattern": {"type": "string"},
            },
        },
    },
    {
        "name": "u3_capture_status",
        "description": (
            "Progress of a streaming capture: samples so far, trigger "
            "seen, whether the worker is alive. Read-only, never touches "
            "the device. A non-terminal job whose worker died is "
            "reported as interrupted/failed — never as done."
        ),
        "inputSchema": {
            "type": "object",
            "properties": {"job": {"type": "string", "description": "Job id (default: latest active)"}},
        },
    },
    {
        "name": "u3_capture_stop",
        "description": (
            "Gracefully finish a streaming capture: the worker ends the "
            "session, the .sr is completed, and the same JSON shape as "
            "u3_capture comes back. open=true pushes the capture into "
            "the running GUI for the user. A job that refuses to stop "
            "is killed and reported interrupted."
        ),
        "inputSchema": {
            "type": "object",
            "properties": {
                "job": {"type": "string"},
                "open": {"type": "boolean"},
            },
        },
    },
    {
        "name": "u3_capture_jobs",
        "description": "All streaming jobs with status and liveness (done/failed/interrupted included).",
        "inputSchema": {"type": "object", "properties": {}},
    },
    {
        "name": "u3_session_info",
        "description": (
            "Metadata of a .sr capture: samplerate, total samples, "
            "duration, channel list, unitsize, trigger position. Call "
            "before any analysis of a file."
        ),
        "inputSchema": {
            "type": "object",
            "properties": {"file": {"type": "string", "description": "Path to .sr file"}},
            "required": ["file"],
        },
    },
    {
        "name": "u3_profile",
        "description": (
            "Automatic signal profile of a capture: per-channel role "
            "(clock-like / data / idle), guessed bitrates, protocol "
            "candidates (i2c/spi/uart with guessed baud), and merged busy "
            "windows. The recommended first analysis step — it answers "
            "'which decoder and channel mapping should I try'."
        ),
        "inputSchema": {
            "type": "object",
            "properties": {
                "file": {"type": "string"},
                "from": {"type": "string", "description": "Window start (sample or time)"},
                "to": {"type": "string", "description": "Window end"},
            },
            "required": ["file"],
        },
    },
    {
        "name": "u3_activity",
        "description": (
            "Toggle counts per bucket per channel: where in the capture "
            "each line actually moves. Use to find the interesting "
            "window before decoding."
        ),
        "inputSchema": {
            "type": "object",
            "properties": {
                "file": {"type": "string"},
                "bucket": {"type": "string", "description": "Bucket size in samples (SI ok)"},
                "channels": {"type": "string", "description": "Comma list, e.g. D0,D1"},
                "from": {"type": "string"},
                "to": {"type": "string"},
            },
            "required": ["file"],
        },
    },
    {
        "name": "u3_measure",
        "description": (
            "Per-channel measurements in a window: frequency, duty cycle, "
            "pulse-width min/max/mean, first/last edge, toggle count, "
            "clock-likeness. Answers 'is this line a clock, how fast'."
        ),
        "inputSchema": {
            "type": "object",
            "properties": {
                "file": {"type": "string"},
                "channels": {"type": "string"},
                "from": {"type": "string"},
                "to": {"type": "string"},
            },
            "required": ["file"],
        },
    },
    {
        "name": "u3_edges",
        "description": (
            "Exact edge timing for one channel: sample numbers, seconds, "
            "delta_ns between edges, filtered by rising/falling. Capped "
            "by max with a truncation flag."
        ),
        "inputSchema": {
            "type": "object",
            "properties": {
                "file": {"type": "string"},
                "channels": {"type": "string", "description": "Comma list; keep it short (one or two channels)"},
                "type": {"type": "string", "description": "any|rising|falling"},
                "max": {"type": "integer", "description": "Rows per channel (default 200)"},
                "from": {"type": "string"},
                "to": {"type": "string"},
            },
            "required": ["file"],
        },
    },
    {
        "name": "u3_sample",
        "description": "Logic level of every channel at one sample (--at sample number or time).",
        "inputSchema": {
            "type": "object",
            "properties": {
                "file": {"type": "string"},
                "at": {"type": "string", "description": "Sample number or time literal"},
            },
            "required": ["file", "at"],
        },
    },
    {
        "name": "u3_slice",
        "description": (
            "Raw sample window, at most 64K samples: bits (per-channel "
            "0/1 strings), hex, or base64 of the sample words. Errors out "
            "on wider windows — narrow from/to."
        ),
        "inputSchema": {
            "type": "object",
            "properties": {
                "file": {"type": "string"},
                "from": {"type": "string"},
                "to": {"type": "string"},
                "format": {"type": "string", "description": "hex|bits|base64"},
                "channels": {"type": "string"},
            },
            "required": ["file", "from", "to"],
        },
    },
    {
        "name": "u3_overview",
        "description": (
            "Bucketed min/max/toggle outline of the whole or windowed "
            "capture — the long-capture shape at a glance."
        ),
        "inputSchema": {
            "type": "object",
            "properties": {
                "file": {"type": "string"},
                "points": {"type": "integer", "description": "Bucket count (default 2000)"},
                "from": {"type": "string"},
                "to": {"type": "string"},
            },
            "required": ["file"],
        },
    },
    {
        "name": "u3_decode",
        "description": (
            "Run protocol decoder(s) over a capture and return annotation "
            "rows as JSON: start/end sample + seconds, decoder, class, "
            "text. Stackable: stack=[\"i2c\",\"eeprom24xx\"] feeds the "
            "first decoder into the second. Map PD channels to device "
            "channels (e.g. channels={\"scl\":\"D0\",\"sda\":\"D1\"}); "
            "PD option values must match the capture (e.g. uart "
            "baudrate). ALWAYS set max_rows (default 500) and read "
            "total_rows/filtered_rows/truncated — never treat a "
            "truncated page as the full traffic. Use from/to windows "
            "around events found via search."
        ),
        "inputSchema": {
            "type": "object",
            "properties": {
                "file": {"type": "string"},
                "stack": {"type": "array", "items": {"type": "string"}, "description": "Decoder ids, first = base; e.g. [\"i2c\"] or [\"i2c\",\"eeprom24xx\"]"},
                "channels": {"type": "object", "description": "PD channel id -> device channel name, e.g. {\"scl\":\"D0\",\"sda\":\"D1\"}; omit to map by identical names"},
                "pdopt": {"type": "object", "description": "Decoder options, e.g. {\"baudrate\":\"9600\"}"},
                "search": {"type": "string", "description": "Substring match over annotation text (e.g. NACK, 0x50)"},
                "class": {"type": "string", "description": "Comma list of annotation classes (e.g. tx-data,ack)"},
                "max_rows": {"type": "integer"},
                "from": {"type": "string"},
                "to": {"type": "string"},
                "binary": {"type": "string", "description": "Also extract protocol payload bytes to this file"},
            },
            "required": ["file", "stack"],
        },
    },
    {
        "name": "u3_list_decoders",
        "description": "Search the 132 protocol decoders by id/name substring.",
        "inputSchema": {
            "type": "object",
            "properties": {"search": {"type": "string"}},
        },
    },
    {
        "name": "u3_decoder_info",
        "description": "One decoder's contract: required channels, options with defaults, annotation classes. Read this before u3_decode.",
        "inputSchema": {
            "type": "object",
            "properties": {"pd": {"type": "string", "description": "Decoder id, e.g. i2c"}},
            "required": ["pd"],
        },
    },
    {
        "name": "u3_render",
        "description": (
            "Render a waveform image (PNG or SVG) of a window and return "
            "it AS AN IMAGE so a multimodal agent can look at the actual "
            "edges. Sample-exact drawing, red line at the trigger. Keep "
            "windows under ~100k samples per call; combine with u3_decode "
            "row coordinates to zoom into events."
        ),
        "inputSchema": {
            "type": "object",
            "properties": {
                "file": {"type": "string"},
                "channels": {"type": "string", "description": "Comma list; keep to 1-4 rows per image"},
                "from": {"type": "string"},
                "to": {"type": "string"},
                "width": {"type": "integer", "description": "Pixel width (default 1600)"},
                "format": {"type": "string", "description": "png|svg (default png; image block only for png)"},
            },
            "required": ["file", "out", "channels"],
        },
    },
    {
        "name": "u3_export",
        "description": (
            "Export a capture into csv/vcd/hex/bits/binary/wav/wavedrom "
            "for external tools. Returns the written file path; content "
            "is NOT inlined — read the file with normal file tools."
        ),
        "inputSchema": {
            "type": "object",
            "properties": {
                "file": {"type": "string"},
                "format": {"type": "string", "description": "csv|vcd|hex|ascii|bits|binary|wav|wavedrom|..."},
                "out": {"type": "string"},
                "overwrite": BOOL,
            },
            "required": ["file", "format", "out"],
        },
    },
    {
        "name": "u3_open_in_gui",
        "description": (
            "Push a .sr capture into the ALREADY RUNNING GUI instance so "
            "the user sees it as a new session tab (no new window). "
            "Reports gui=existing-instance on success or gui_opened=false "
            "when no GUI is running (the file stays on disk either way)."
        ),
        "inputSchema": {
            "type": "object",
            "properties": {"file": {"type": "string"}},
            "required": ["file"],
        },
    },
]

# Tools whose u3ctl positional layout differs from the generic mapper.
# Mapping spec: list of (schema_key, [cli flags]) — value emits once per
# flag; lists repeat the flag; dicts emit key=value pairs repeated.
ARGMAP = {
    "u3_list_devices": [],
    "u3_capabilities": [("driver", ["--driver"]), ("conn", ["--conn"])],
    "u3_capture": [
        ("driver", ["--driver"]), ("conn", ["--conn"]), ("rate", ["--rate"]),
        ("channels", ["--channels"]), ("samples", ["--samples"]),
        ("time", ["--time"]), ("trigger", ["--trigger"]), ("pre", ["--pre"]),
        ("threshold", ["--threshold"]), ("mode", ["--mode"]),
        ("timeout", ["--timeout"]), ("out", ["--out"]),
        ("overwrite", ["--overwrite"]), ("demo_pattern", ["--demo-pattern"]),
        ("open", ["--open"]),
    ],
    "u3_capture_start": [
        ("driver", ["--driver"]), ("conn", ["--conn"]), ("rate", ["--rate"]),
        ("channels", ["--channels"]), ("samples", ["--samples"]),
        ("time", ["--time"]), ("trigger", ["--trigger"]), ("pre", ["--pre"]),
        ("threshold", ["--threshold"]), ("timeout", ["--timeout"]),
        ("out", ["--out"]), ("overwrite", ["--overwrite"]),
        ("demo_pattern", ["--demo-pattern"]),
    ],
    "u3_capture_status": [("job", ["--job"])],
    "u3_capture_stop": [("job", ["--job"]), ("open", ["--open"])],
    "u3_capture_jobs": [],
    "u3_session_info": [],
    "u3_profile": [("from", ["--from"]), ("to", ["--to"])],
    "u3_activity": [
        ("bucket", ["--bucket"]), ("channels", ["--channels"]),
        ("from", ["--from"]), ("to", ["--to"]),
    ],
    "u3_measure": [("channels", ["--channels"]), ("from", ["--from"]), ("to", ["--to"])],
    "u3_edges": [
        ("channels", ["--channels"]), ("type", ["--type"]), ("max", ["--max"]),
        ("from", ["--from"]), ("to", ["--to"]),
    ],
    "u3_sample": [("at", ["--at"])],
    "u3_slice": [
        ("from", ["--from"]), ("to", ["--to"]), ("format", ["--format"]),
        ("channels", ["--channels"]),
    ],
    "u3_overview": [("points", ["--points"]), ("from", ["--from"]), ("to", ["--to"])],
    "u3_decode": [
        ("stack", ["--stack"]), ("channels", ["--channel"]), ("pdopt", ["--pdopt"]),
        ("search", ["--search"]), ("class", ["--class"]),
        ("max_rows", ["--max-rows"]), ("from", ["--from"]), ("to", ["--to"]),
        ("binary", ["--binary"]),
    ],
    "u3_list_decoders": [("search", ["--search"])],
    "u3_decoder_info": [],
    "u3_render": [
        ("channels", ["--channels"]), ("from", ["--from"]), ("to", ["--to"]),
        ("width", ["--width"]), ("format", ["--format"]), ("out", ["--out"]),
    ],
    "u3_export": [
        ("format", ["--format"]), ("out", ["--out"]), ("overwrite", ["--overwrite"]),
    ],
    "u3_open_in_gui": [],
}

COMMANDS = {
    "u3_list_devices": ["devices", "list"],
    "u3_capabilities": ["capabilities"],
    "u3_capture": ["capture"],
    "u3_capture_start": ["capture-start"],
    "u3_capture_status": ["capture-status"],
    "u3_capture_stop": ["capture-stop"],
    "u3_capture_jobs": ["capture-list"],
    "u3_session_info": ["session", "info"],
    "u3_profile": ["profile"],
    "u3_activity": ["activity"],
    "u3_measure": ["measure"],
    "u3_edges": ["edges"],
    "u3_sample": ["sample"],
    "u3_slice": ["slice"],
    "u3_overview": ["overview"],
    "u3_decode": ["decode"],
    "u3_list_decoders": ["decoders", "list"],
    "u3_decoder_info": ["decoders", "info"],
    "u3_render": ["render"],
    "u3_export": ["export"],
    "u3_open_in_gui": ["open"],
}

# Flags that take no value.
FLAG_ONLY = {"--overwrite", "--open"}


def build_args(tool, args):
    """MCP tool arguments -> u3ctl argv."""
    argv = list(COMMANDS[tool])
    if tool in ("u3_session_info", "u3_open_in_gui", "u3_decoder_info"):
        if tool == "u3_decoder_info":
            argv.append(args.get("pd", ""))
        else:
            argv.append(args.get("file", ""))
        return argv
    if tool == "u3_profile" and "file" in args:
        argv.append(args["file"])

    for key, flags in ARGMAP.get(tool, []):
        if key not in args or args[key] in (None, False):
            continue
        val = args[key]
        for flag in flags:
            if flag in FLAG_ONLY:
                argv.append(flag)
                continue
            if isinstance(val, list):
                for item in val:
                    argv += [flag, str(item)]
            elif isinstance(val, dict):
                for k, v in val.items():
                    argv += [flag, f"{k}={v}"]
            else:
                argv += [flag, str(val)]
    # file positionals for query-style commands
    if "file" in args and tool not in ("u3_capture",):
        argv.append(args["file"])
    return argv


def run_u3ctl(argv, timeout=90):
    proc = subprocess.run(
        [U3CTL] + argv, capture_output=True, text=True, timeout=timeout
    )
    return proc


def tool_result_text(tool, args):
    """Execute one tool; return (content_blocks, is_error)."""
    argv = build_args(tool, args)

    # render: image content when PNG
    if tool == "u3_render":
        out = args.get("out") or os.path.join(
            tempfile.gettempdir(), "u3la_render.png"
        )
        fmt = args.get("format", "png")
        proc = run_u3ctl(argv + (["--out", out] if "out" not in args else []),
                         timeout=120)
        try:
            meta = json.loads(proc.stdout)
        except json.JSONDecodeError:
            meta = None
        if meta is None or "error" in meta:
            return [text_block(proc.stdout.strip() or "render failed")], True
        blocks = [text_block(json.dumps(meta, ensure_ascii=False, indent=1))]
        if os.path.exists(out) and (fmt or "png") == "png":
            with open(out, "rb") as f:
                b64 = base64.b64encode(f.read()).decode("ascii")
            blocks.append({
                "type": "image",
                "data": b64,
                "mimeType": "image/png",
            })
        return blocks, False

    proc = run_u3ctl(argv, timeout=120)
    stdout = proc.stdout.strip()
    try:
        meta = json.loads(stdout)
    except json.JSONDecodeError:
        meta = None

    if meta is None:
        # Non-JSON output: failure path or odd command.
        return [text_block(stdout or (proc.stderr.strip() or "no output"))], proc.returncode != 0

    if "error" in meta:
        return [text_block(json.dumps(meta, ensure_ascii=False, indent=1))], True

    pretty = json.dumps(meta, ensure_ascii=False, indent=1)
    if len(pretty) > 60000:
        pretty = pretty[:60000] + "\n... (truncated by u3la-mcp; narrow the request)"
    return [text_block(pretty)], False


def text_block(text):
    return {"type": "text", "text": text}


def handle_initialize(params):
    return {
        "protocolVersion": PROTOCOL_VERSION,
        "capabilities": {"tools": {}},
        "serverInfo": {"name": SERVER_NAME, "version": SERVER_VERSION},
    }


def handle_tools_list(_params):
    return {"tools": TOOLS}


def handle_tools_call(params):
    name = params.get("name", "")
    args = params.get("arguments", {}) or {}
    if name not in COMMANDS:
        return {
            "content": [text_block(f"unknown tool {name}")],
            "isError": True,
        }
    if not os.path.exists(U3CTL):
        return {
            "content": [text_block(
                f"u3ctl binary not found at {U3CTL}; build it: "
                f"cmake -B build_u3ctl -S tools/u3ctl && cmake --build build_u3ctl"
            )],
            "isError": True,
        }
    try:
        blocks, is_error = tool_result_text(name, args)
    except subprocess.TimeoutExpired:
        blocks = [text_block("u3ctl timed out (device stuck or huge window?)")]
        is_error = True
    return {"content": blocks, "isError": is_error}


def dispatch(method, params):
    if method == "initialize":
        return handle_initialize(params)
    if method == "tools/list":
        return handle_tools_list(params)
    if method == "tools/call":
        return handle_tools_call(params)
    if method == "ping":
        return {}
    return None  # unknown -> no result (notifications ignored)


def main():
    for line in sys.stdin:
        line = line.strip()
        if not line:
            continue
        try:
            msg = json.loads(line)
        except json.JSONDecodeError:
            continue
        method = msg.get("method", "")
        msg_id = msg.get("id")
        is_notification = msg_id is None

        if method == "notifications/initialized":
            continue
        if is_notification:
            continue

        result = dispatch(method, msg.get("params", {}))
        if result is None and method not in ("ping",):
            resp = {
                "jsonrpc": "2.0", "id": msg_id,
                "error": {"code": -32601, "message": f"method not found: {method}"},
            }
        else:
            resp = {"jsonrpc": "2.0", "id": msg_id, "result": result}
        sys.stdout.write(json.dumps(resp, ensure_ascii=False) + "\n")
        sys.stdout.flush()


if __name__ == "__main__":
    main()
