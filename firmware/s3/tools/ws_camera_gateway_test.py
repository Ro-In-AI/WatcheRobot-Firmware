#!/usr/bin/env python3
"""Local Watcher camera gateway harness.

This script emulates the local gateway/server side for Watcher camera media
testing:

1. Replies to UDP discovery:
   {"cmd":"DISCOVER",...} -> {"cmd":"ANNOUNCE","ip":"x.x.x.x","port":8765,...}
2. Hosts a WebSocket server for the Watcher firmware.
3. Validates text frames such as sys.ack / sys.nack / evt.camera.state.
4. Validates binary camera media packets with the fixed WSPK header.
5. Saves received JPEG payloads to disk for inspection.

WSPK is the 4-byte ASCII magic at the front of each camera media binary packet.
In this project it is the fixed packet marker that lets the gateway recognize
and parse Watcher camera media frames on the WebSocket stream.
"""

from __future__ import annotations

import argparse
import asyncio
import json
import logging
import socket
import struct
import sys
import time
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

import websockets


DISCOVERY_PORT = 37020
WS_PORT = 8765
DEFAULT_VERSION = "1.0.0"
DEFAULT_OUTPUT_DIR = Path("build/ws_camera_gateway_test")
WSPK_HEADER = struct.Struct("<4sBBHII")
WSPK_MAGIC = b"WSPK"
WSPK_FRAME_VIDEO = 2
WSPK_FRAME_IMAGE = 3
WSPK_FLAG_FIRST = 0x01
WSPK_FLAG_LAST = 0x02
WSPK_FLAG_KEYFRAME = 0x04
JPEG_SOI = b"\xFF\xD8"
JPEG_EOI = b"\xFF\xD9"


def configure_logging(level: str) -> None:
    logging.basicConfig(
        level=getattr(logging, level.upper(), logging.INFO),
        format="%(asctime)s %(levelname)s %(message)s",
        datefmt="%H:%M:%S",
    )


def detect_local_ip() -> str:
    probes = [("8.8.8.8", 80), ("1.1.1.1", 80)]
    for host, port in probes:
        try:
            with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
                sock.connect((host, port))
                ip = sock.getsockname()[0]
                if ip and not ip.startswith("127."):
                    return ip
        except OSError:
            continue

    try:
        ip = socket.gethostbyname(socket.gethostname())
        if ip:
            return ip
    except OSError:
        pass

    return "127.0.0.1"


def format_flags(flags: int) -> str:
    names: list[str] = []
    if flags & WSPK_FLAG_FIRST:
        names.append("FIRST")
    if flags & WSPK_FLAG_LAST:
        names.append("LAST")
    if flags & WSPK_FLAG_KEYFRAME:
        names.append("KEYFRAME")
    unknown = flags & ~(WSPK_FLAG_FIRST | WSPK_FLAG_LAST | WSPK_FLAG_KEYFRAME)
    if unknown:
        names.append(f"0x{unknown:02X}")
    return "|".join(names) if names else "0"


def frame_type_name(frame_type: int) -> str:
    if frame_type == WSPK_FRAME_VIDEO:
        return "video"
    if frame_type == WSPK_FRAME_IMAGE:
        return "image"
    return f"unknown({frame_type})"


@dataclass
class WspkPacket:
    frame_type: int
    flags: int
    stream_id: int
    seq: int
    payload_len: int
    payload: bytes

    @classmethod
    def parse(cls, data: bytes) -> "WspkPacket":
        if len(data) < WSPK_HEADER.size:
            raise ValueError(f"binary frame too short for WSPK header: {len(data)} bytes")

        magic, frame_type, flags, stream_id, seq, payload_len = WSPK_HEADER.unpack_from(data)
        if magic != WSPK_MAGIC:
            raise ValueError(f"invalid WSPK magic: {magic!r}")

        payload = data[WSPK_HEADER.size :]
        if len(payload) != payload_len:
            raise ValueError(
                f"payload length mismatch: header={payload_len}, actual={len(payload)}"
            )

        return cls(
            frame_type=frame_type,
            flags=flags,
            stream_id=stream_id,
            seq=seq,
            payload_len=payload_len,
            payload=payload,
        )

    def is_jpeg(self) -> bool:
        return self.payload.startswith(JPEG_SOI) and self.payload.endswith(JPEG_EOI)


@dataclass
class PendingCommand:
    command_id: str
    command_type: str
    expected_action: str | None = None
    expected_state: str | None = None
    ack_event: asyncio.Event = field(default_factory=asyncio.Event)
    state_event: asyncio.Event = field(default_factory=asyncio.Event)
    binary_event: asyncio.Event = field(default_factory=asyncio.Event)
    ack_payload: dict[str, Any] | None = None
    state_payload: dict[str, Any] | None = None
    binary_packet: WspkPacket | None = None
    stream_id: int | None = None
    errors: list[str] = field(default_factory=list)


class DiscoveryProtocol(asyncio.DatagramProtocol):
    def __init__(self, harness: "GatewayHarness") -> None:
        self.harness = harness
        self.transport: asyncio.DatagramTransport | None = None

    def connection_made(self, transport: asyncio.BaseTransport) -> None:
        self.transport = transport  # type: ignore[assignment]
        sockname = transport.get_extra_info("sockname")
        logging.info("UDP discovery listener ready on %s", sockname)

    def datagram_received(self, data: bytes, addr: tuple[str, int]) -> None:
        self.harness.discovery_packets += 1
        text = data.decode("utf-8", errors="replace")
        logging.info("UDP RX %s <- %s:%d", text, addr[0], addr[1])

        try:
            payload = json.loads(text)
        except json.JSONDecodeError as exc:
            self.harness.fail(f"invalid discovery JSON from {addr[0]}:{addr[1]}: {exc}")
            return

        if payload.get("cmd") != "DISCOVER":
            logging.warning("Ignoring non-DISCOVER UDP payload from %s:%d", addr[0], addr[1])
            return

        announce = {
            "cmd": "ANNOUNCE",
            "ip": self.harness.announce_ip,
            "port": self.harness.args.ws_port,
            "version": self.harness.args.version,
        }
        raw = json.dumps(announce, separators=(",", ":")).encode("utf-8")
        assert self.transport is not None
        self.transport.sendto(raw, addr)
        logging.info("UDP TX %s -> %s:%d", raw.decode("utf-8"), addr[0], addr[1])

    def error_received(self, exc: Exception) -> None:
        self.harness.fail(f"UDP discovery error: {exc}")


class GatewayHarness:
    def __init__(self, args: argparse.Namespace) -> None:
        self.args = args
        self.announce_ip = args.announce_ip or detect_local_ip()
        self.output_dir = Path(args.output_dir).resolve()
        self.output_dir.mkdir(parents=True, exist_ok=True)

        self.discovery_transport: asyncio.DatagramTransport | None = None
        self.websocket: Any = None
        self.websocket_connected = asyncio.Event()

        self.expectations: dict[str, PendingCommand] = {}
        self.capture_expectation: PendingCommand | None = None
        self.video_config_expectation: PendingCommand | None = None
        self.video_start_expectation: PendingCommand | None = None
        self.video_stop_expectation: PendingCommand | None = None

        self.failures: list[str] = []
        self.discovery_packets = 0
        self.text_frames = 0
        self.binary_frames = 0
        self.invalid_binary_frames = 0
        self.saved_files: list[Path] = []
        self.video_frame_count = 0
        self.image_frame_count = 0

    def fail(self, message: str) -> None:
        logging.error("%s", message)
        self.failures.append(message)

    async def start_udp_server(self) -> None:
        loop = asyncio.get_running_loop()
        transport, _ = await loop.create_datagram_endpoint(
            lambda: DiscoveryProtocol(self),
            local_addr=(self.args.bind_host, self.args.discovery_port),
            allow_broadcast=True,
        )
        self.discovery_transport = transport  # type: ignore[assignment]

    async def websocket_handler(self, websocket: Any) -> None:
        peer = getattr(websocket, "remote_address", None)
        logging.info("WebSocket connected: %s", peer)
        self.websocket = websocket
        self.websocket_connected.set()
        try:
            async for message in websocket:
                if isinstance(message, str):
                    await self.handle_text(message)
                else:
                    await self.handle_binary(message)
        except websockets.ConnectionClosed as exc:
            logging.info("WebSocket closed: code=%s reason=%s", exc.code, exc.reason)
        finally:
            self.websocket = None

    async def handle_text(self, text: str) -> None:
        self.text_frames += 1
        logging.info("WS RX TEXT %s", text)

        try:
            message = json.loads(text)
        except json.JSONDecodeError as exc:
            self.fail(f"invalid JSON text frame: {exc}")
            return

        msg_type = message.get("type")
        if msg_type == "sys.ack":
            self.handle_sys_ack(message)
        elif msg_type == "sys.nack":
            self.handle_sys_nack(message)
        elif msg_type == "evt.camera.state":
            self.handle_camera_state(message)
        else:
            logging.info("Ignoring non-camera text frame type=%r", msg_type)

    async def handle_binary(self, payload: bytes) -> None:
        self.binary_frames += 1
        try:
            packet = WspkPacket.parse(payload)
        except ValueError as exc:
            self.invalid_binary_frames += 1
            self.fail(f"invalid binary frame: {exc}")
            return

        logging.info(
            "WS RX BIN type=%s flags=%s stream_id=%d seq=%d payload_len=%d",
            frame_type_name(packet.frame_type),
            format_flags(packet.flags),
            packet.stream_id,
            packet.seq,
            packet.payload_len,
        )

        if packet.frame_type == WSPK_FRAME_IMAGE:
            await self.handle_image_packet(packet)
        elif packet.frame_type == WSPK_FRAME_VIDEO:
            await self.handle_video_packet(packet)
        else:
            self.fail(f"unexpected WSPK frame_type={packet.frame_type}")

    def handle_sys_ack(self, message: dict[str, Any]) -> None:
        data = message.get("data")
        if not isinstance(data, dict):
            self.fail("sys.ack missing data object")
            return

        command_id = data.get("command_id")
        command_type = data.get("command_type")
        if not isinstance(command_id, str) or not command_id:
            self.fail("sys.ack missing command_id")
            return
        if not isinstance(command_type, str) or not command_type:
            self.fail("sys.ack missing command_type")
            return

        expectation = self.expectations.get(command_id)
        if expectation is None:
            self.fail(f"sys.ack for unknown command_id={command_id}")
            return

        if expectation.command_type != command_type:
            expectation.errors.append(
                f"command_type mismatch: expected={expectation.command_type}, actual={command_type}"
            )
            self.fail(expectation.errors[-1])

        stream_id = data.get("stream_id")
        if stream_id is not None and not isinstance(stream_id, int):
            self.fail(f"sys.ack stream_id has invalid type: {type(stream_id).__name__}")
            return

        expectation.stream_id = stream_id
        expectation.ack_payload = data
        expectation.ack_event.set()

    def handle_sys_nack(self, message: dict[str, Any]) -> None:
        data = message.get("data")
        if not isinstance(data, dict):
            self.fail("sys.nack missing data object")
            return

        command_id = data.get("command_id")
        command_type = data.get("command_type")
        reason = data.get("reason")
        self.fail(
            "device rejected command: "
            f"command_id={command_id!r} command_type={command_type!r} reason={reason!r}"
        )

    def handle_camera_state(self, message: dict[str, Any]) -> None:
        data = message.get("data")
        if not isinstance(data, dict):
            self.fail("evt.camera.state missing data object")
            return

        action = data.get("action")
        state = data.get("state")
        if not isinstance(action, str) or not action:
            self.fail("evt.camera.state missing action")
            return
        if not isinstance(state, str) or not state:
            self.fail("evt.camera.state missing state")
            return

        expectation: PendingCommand | None = None
        if action == "capture_image":
            expectation = self.capture_expectation
        elif action == "video_config":
            expectation = self.video_config_expectation
        elif action == "start_video":
            expectation = self.video_start_expectation
        elif action == "stop_video":
            expectation = self.video_stop_expectation

        if expectation is None:
            logging.warning("Unmatched evt.camera.state action=%s state=%s", action, state)
            return

        if expectation.expected_state and expectation.expected_state != state:
            expectation.errors.append(
                f"camera state mismatch: action={action}, expected={expectation.expected_state}, actual={state}"
            )
            self.fail(expectation.errors[-1])

        stream_id = data.get("stream_id")
        if stream_id is not None and expectation.stream_id is not None and stream_id != expectation.stream_id:
            expectation.errors.append(
                f"camera state stream_id mismatch: expected={expectation.stream_id}, actual={stream_id}"
            )
            self.fail(expectation.errors[-1])

        expectation.state_payload = data
        expectation.state_event.set()

    async def handle_image_packet(self, packet: WspkPacket) -> None:
        self.image_frame_count += 1
        if not packet.is_jpeg():
            self.fail(
                f"image packet stream={packet.stream_id} seq={packet.seq} does not contain a full JPEG"
            )

        if self.capture_expectation is not None:
            expected_stream = self.capture_expectation.stream_id
            if expected_stream is not None and packet.stream_id != expected_stream:
                self.fail(
                    f"capture image stream_id mismatch: expected={expected_stream}, actual={packet.stream_id}"
                )
            self.capture_expectation.binary_packet = packet
            self.capture_expectation.binary_event.set()

        self.save_packet(packet)

    async def handle_video_packet(self, packet: WspkPacket) -> None:
        if packet.payload_len == 0:
            if not (packet.flags & WSPK_FLAG_LAST):
                self.fail(
                    f"zero-payload video packet missing LAST flag: stream={packet.stream_id} seq={packet.seq}"
                )

            if self.video_stop_expectation is not None:
                expected_stream = self.video_stop_expectation.stream_id
                if expected_stream is not None and packet.stream_id != expected_stream:
                    self.fail(
                        f"video end stream_id mismatch: expected={expected_stream}, actual={packet.stream_id}"
                    )
                self.video_stop_expectation.binary_packet = packet
                self.video_stop_expectation.binary_event.set()
            return

        self.video_frame_count += 1
        if not packet.is_jpeg():
            self.fail(
                f"video packet stream={packet.stream_id} seq={packet.seq} does not contain a full JPEG"
            )

        if self.video_start_expectation is not None:
            expected_stream = self.video_start_expectation.stream_id
            if expected_stream is not None and packet.stream_id != expected_stream:
                self.fail(
                    f"video frame stream_id mismatch: expected={expected_stream}, actual={packet.stream_id}"
                )
            if self.video_start_expectation.binary_packet is None:
                self.video_start_expectation.binary_packet = packet
                self.video_start_expectation.binary_event.set()

        self.save_packet(packet)

    def save_packet(self, packet: WspkPacket) -> None:
        stream_dir = self.output_dir / f"stream_{packet.stream_id:04d}"
        stream_dir.mkdir(parents=True, exist_ok=True)

        kind = frame_type_name(packet.frame_type)
        suffix = ".jpg" if packet.is_jpeg() else ".bin"
        path = stream_dir / f"{kind}_seq_{packet.seq:06d}{suffix}"
        path.write_bytes(packet.payload)
        self.saved_files.append(path)
        logging.info("Saved %s", path)

    async def send_json(self, payload: dict[str, Any]) -> None:
        if self.websocket is None:
            raise RuntimeError("websocket not connected")

        text = json.dumps(payload, separators=(",", ":"))
        logging.info("WS TX TEXT %s", text)
        await self.websocket.send(text)

    async def wait_for_event(
        self, event: asyncio.Event, label: str, timeout: float | None = None
    ) -> None:
        wait_timeout = timeout if timeout is not None else self.args.command_timeout
        try:
            await asyncio.wait_for(event.wait(), timeout=wait_timeout)
        except asyncio.TimeoutError as exc:
            raise TimeoutError(f"timeout waiting for {label} after {wait_timeout:.1f}s") from exc

    def register_expectation(self, expectation: PendingCommand) -> None:
        self.expectations[expectation.command_id] = expectation

    async def run_capture_flow(self) -> None:
        command_id = f"cam-shot-{int(time.time())}"
        self.capture_expectation = PendingCommand(
            command_id=command_id,
            command_type="ctrl.camera.capture_image",
            expected_action="capture_image",
            expected_state="completed",
        )
        self.register_expectation(self.capture_expectation)

        await self.send_json(
            {
                "type": "ctrl.camera.capture_image",
                "code": 0,
                "data": {"command_id": command_id},
            }
        )

        await self.wait_for_event(self.capture_expectation.ack_event, "capture sys.ack")
        await self.wait_for_event(self.capture_expectation.binary_event, "capture WSPK image")
        await self.wait_for_event(
            self.capture_expectation.state_event, "capture evt.camera.state"
        )

    async def run_video_flow(self) -> None:
        config_id = f"cam-cfg-{int(time.time())}"
        self.video_config_expectation = PendingCommand(
            command_id=config_id,
            command_type="ctrl.camera.video_config",
            expected_action="video_config",
            expected_state="accepted",
        )
        self.register_expectation(self.video_config_expectation)

        await self.send_json(
            {
                "type": "ctrl.camera.video_config",
                "code": 0,
                "data": {
                    "command_id": config_id,
                    "width": self.args.width,
                    "height": self.args.height,
                    "fps": self.args.fps,
                    "quality": self.args.quality,
                },
            }
        )

        await self.wait_for_event(self.video_config_expectation.ack_event, "video_config sys.ack")
        await self.wait_for_event(
            self.video_config_expectation.state_event, "video_config evt.camera.state"
        )

        start_id = f"cam-start-{int(time.time())}"
        self.video_start_expectation = PendingCommand(
            command_id=start_id,
            command_type="ctrl.camera.start_video",
            expected_action="start_video",
            expected_state="started",
        )
        self.register_expectation(self.video_start_expectation)

        await self.send_json(
            {
                "type": "ctrl.camera.start_video",
                "code": 0,
                "data": {"command_id": start_id, "fps": self.args.fps},
            }
        )

        await self.wait_for_event(self.video_start_expectation.ack_event, "start_video sys.ack")
        await self.wait_for_event(
            self.video_start_expectation.state_event, "start_video evt.camera.state"
        )
        await self.wait_for_event(
            self.video_start_expectation.binary_event, "first video WSPK frame"
        )

        logging.info("Video stream running for %.1f seconds", self.args.video_seconds)
        await asyncio.sleep(self.args.video_seconds)

        stop_id = f"cam-stop-{int(time.time())}"
        self.video_stop_expectation = PendingCommand(
            command_id=stop_id,
            command_type="ctrl.camera.stop_video",
            expected_action="stop_video",
            expected_state="stopped",
        )
        self.register_expectation(self.video_stop_expectation)

        if self.video_start_expectation.stream_id is not None:
            self.video_stop_expectation.stream_id = self.video_start_expectation.stream_id

        await self.send_json(
            {
                "type": "ctrl.camera.stop_video",
                "code": 0,
                "data": {"command_id": stop_id},
            }
        )

        await self.wait_for_event(self.video_stop_expectation.ack_event, "stop_video sys.ack")
        await self.wait_for_event(self.video_stop_expectation.binary_event, "video end WSPK frame")
        await self.wait_for_event(
            self.video_stop_expectation.state_event, "stop_video evt.camera.state"
        )

    async def run_scenario(self) -> None:
        if self.args.scenario in ("capture", "all"):
            await self.run_capture_flow()

        if self.args.scenario in ("video", "all"):
            await self.run_video_flow()

        if self.args.scenario == "listen":
            logging.info("Listen mode enabled; press Ctrl+C to stop.")
            while True:
                await asyncio.sleep(1.0)

    def print_summary(self) -> None:
        logging.info("----- Summary -----")
        logging.info("announce_ip=%s", self.announce_ip)
        logging.info("discovery_packets=%d", self.discovery_packets)
        logging.info("text_frames=%d", self.text_frames)
        logging.info("binary_frames=%d", self.binary_frames)
        logging.info("invalid_binary_frames=%d", self.invalid_binary_frames)
        logging.info("image_frames=%d", self.image_frame_count)
        logging.info("video_frames=%d", self.video_frame_count)
        logging.info("saved_files=%d", len(self.saved_files))
        logging.info("output_dir=%s", self.output_dir)
        if self.failures:
            logging.error("validation_failures=%d", len(self.failures))
            for failure in self.failures:
                logging.error("failure: %s", failure)
        else:
            logging.info("validation=PASS")

    async def run(self) -> int:
        await self.start_udp_server()
        async with websockets.serve(
            self.websocket_handler,
            self.args.bind_host,
            self.args.ws_port,
            max_size=None,
            ping_interval=None,
        ):
            logging.info(
                "WebSocket server ready on ws://%s:%d",
                self.announce_ip if self.args.bind_host == "0.0.0.0" else self.args.bind_host,
                self.args.ws_port,
            )
            logging.info(
                "Waiting for Watcher connection (scenario=%s, connect_timeout=%.1fs)",
                self.args.scenario,
                self.args.connect_timeout,
            )

            try:
                await self.wait_for_event(
                    self.websocket_connected,
                    "Watcher WebSocket connection",
                    timeout=self.args.connect_timeout,
                )
                await self.run_scenario()
            except KeyboardInterrupt:
                logging.info("Interrupted by user")
            except Exception as exc:  # noqa: BLE001
                self.fail(str(exc))
            finally:
                self.print_summary()
                if self.discovery_transport is not None:
                    self.discovery_transport.close()

        return 1 if self.failures else 0


def build_arg_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Local gateway test harness for Watcher camera sys.ack / evt.camera.state / WSPK validation."
    )
    parser.add_argument(
        "--scenario",
        choices=("all", "capture", "video", "listen"),
        default="all",
        help="Test scenario to run after the Watcher connects.",
    )
    parser.add_argument(
        "--bind-host",
        default="0.0.0.0",
        help="Local bind host for both UDP discovery and WebSocket server.",
    )
    parser.add_argument(
        "--announce-ip",
        default="",
        help="IP address returned in the UDP ANNOUNCE response. Defaults to auto-detect.",
    )
    parser.add_argument(
        "--discovery-port",
        type=int,
        default=DISCOVERY_PORT,
        help="UDP discovery port used by the Watcher.",
    )
    parser.add_argument(
        "--ws-port",
        type=int,
        default=WS_PORT,
        help="WebSocket server port announced to the Watcher.",
    )
    parser.add_argument(
        "--version",
        default=DEFAULT_VERSION,
        help="Version string included in the UDP ANNOUNCE response.",
    )
    parser.add_argument(
        "--output-dir",
        default=str(DEFAULT_OUTPUT_DIR),
        help="Directory where received JPEG frames are saved.",
    )
    parser.add_argument(
        "--connect-timeout",
        type=float,
        default=90.0,
        help="Seconds to wait for the Watcher WebSocket connection.",
    )
    parser.add_argument(
        "--command-timeout",
        type=float,
        default=10.0,
        help="Seconds to wait for each expected ack/state/WSPK event.",
    )
    parser.add_argument(
        "--video-seconds",
        type=float,
        default=5.0,
        help="How long to keep video streaming before sending stop_video.",
    )
    parser.add_argument(
        "--width",
        type=int,
        default=640,
        help="Requested video width used in ctrl.camera.video_config.",
    )
    parser.add_argument(
        "--height",
        type=int,
        default=480,
        help="Requested video height used in ctrl.camera.video_config.",
    )
    parser.add_argument(
        "--fps",
        type=int,
        default=5,
        help="Requested video fps used in ctrl.camera.video_config/start_video.",
    )
    parser.add_argument(
        "--quality",
        type=int,
        default=80,
        help="Requested JPEG quality used in ctrl.camera.video_config.",
    )
    parser.add_argument(
        "--log-level",
        default="INFO",
        choices=("DEBUG", "INFO", "WARNING", "ERROR"),
        help="Logging verbosity.",
    )
    return parser


async def async_main(args: argparse.Namespace) -> int:
    harness = GatewayHarness(args)
    return await harness.run()


def main() -> int:
    parser = build_arg_parser()
    args = parser.parse_args()
    configure_logging(args.log_level)

    try:
        return asyncio.run(async_main(args))
    except KeyboardInterrupt:
        logging.info("Interrupted by user")
        return 0


if __name__ == "__main__":
    sys.exit(main())
