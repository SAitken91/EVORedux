"""Trials Evolution revival server.

HTTP :80     OnlineConfigService stub (points the game at our sandbox)
UDP  :21170  Quazal PRUDP authentication server
UDP  :21171  Quazal PRUDP secure server

Environment:
  TRIALS_HOST   public IP/hostname players connect to (default 127.0.0.1)
  TRIALS_DEBUG  1 = also log packets and RMC parameters (local troubleshooting only: they
                contain player names and IP addresses; credential calls stay redacted)
"""
import json
import logging
import logging.handlers
import os
import struct
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlsplit

from prudp import PRUDPServer
from track_library import open_library
from quazal import (QRESULT_NOT_IMPLEMENTED, SERVER_PID, Accounts, MatchMaking, SecureConnection,
                    TicketGranting, Tracking, Privileges, UbiAccountManagement,
                    TitleStorage, PlayerStats, UserGeneratedContent, UplayWin, UserStorage, rmc_error, secure_on_connect)

HOST_IP = os.environ.get("TRIALS_HOST", "127.0.0.1")
AUTH_PORT = 21170
SECURE_PORT = 21171
DEBUG = os.environ.get("TRIALS_DEBUG") == "1"

_logger = logging.getLogger("trials")
_logger.setLevel(logging.DEBUG if DEBUG else logging.INFO)
for _handler in (logging.StreamHandler(),
                 logging.handlers.RotatingFileHandler("traffic.log", maxBytes=5_000_000, backupCount=3,
                                                      encoding="utf-8")):
    _handler.setFormatter(logging.Formatter("%(asctime)s %(message)s", "%Y-%m-%d %H:%M:%S"))
    _logger.addHandler(_handler)


def log(msg):
    _logger.info(msg)


def debug(msg):
    _logger.debug(msg)


# Only games running the revival DLL ask for this config ID (it replaces Ubisoft's built-in
# e940a83f... in memory; see client_shim/orbit_shim.cpp). Any other ID gets 404, so a game
# started without the DLL stays offline, as with Ubisoft's dead servers, and never reaches the
# login step that would send the player's real Ubisoft password.
REVIVAL_CONFIG_ID = "c5855ba01ddd954b564be2c8a4ab86e4"

ONLINE_CONFIG = [
    {"Name": "SandboxUrl", "Values": [f"prudp:/address={HOST_IP};port={AUTH_PORT}"]},
    {"Name": "SandboxUrlWS", "Values": [f"{HOST_IP}:{AUTH_PORT}"]},
]


class ConfigHandler(BaseHTTPRequestHandler):
    """OnlineConfigService stub plus the HTTP side of UserStorage (ghost upload/download)."""

    def _reply(self, code, body=b"", ctype="application/octet-stream"):
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        debug(f"HTTP GET {self.path}")
        if "GetOnlineConfig" in self.path:
            query = parse_qs(urlsplit(self.path).query)
            if query.get("onlineConfigID") != [REVIVAL_CONFIG_ID]:
                log("online config refused: game started without the revival DLL")
                return self._reply(404)
            return self._reply(200, json.dumps(ONLINE_CONFIG).encode(), "application/json; charset=utf-8")
        if self.path == "/revival/ticker.txt":
            # Read by the client shim at startup: revival messages for the menu ticker.
            try:
                with open("ticker.txt", "rb") as f:
                    return self._reply(200, f.read(4096), "text/plain; charset=utf-8")
            except FileNotFoundError:
                return self._reply(404)
        if self.path == "/revival/ugc.xml":
            # Track Central feed list, read by the client shim at startup (ugc_feeds.cpp).
            if not track_library:
                return self._reply(404)
            return self._reply(200, track_library.feeds_document().encode(), "text/xml; charset=utf-8")
        if self.path.startswith("/ugc/content/"):
            data = user_storage.http_download(self.path.rsplit("/", 1)[1])
            return self._reply(200, data) if data is not None else self._reply(404)
        self._reply(404)

    def _upload(self):
        try:
            length = int(self.headers.get("Content-Length") or 0)
        except ValueError:
            return self._reply(400)
        # Check the size before reading so a bogus Content-Length can't exhaust memory.
        if not 0 <= length <= UserStorage.MAX_SIZE:
            return self._reply(413)
        if not self.path.startswith("/ugc/upload/"):
            return self._reply(404)
        body = self.rfile.read(length) if length else b""
        debug(f"HTTP {self.command} {self.path} {length} bytes type={self.headers.get('Content-Type')}")
        ok = user_storage.http_upload(self.path.rsplit("/", 1)[1], body)
        self._reply(200 if ok else 404)

    do_PUT = do_POST = _upload

    def log_message(self, *args):
        pass


def parse_rmc_request(msg):
    size, proto = struct.unpack_from("<IB", msg)
    proto &= 0x7F
    call_id, method = struct.unpack_from("<II", msg, 5)
    return proto, call_id, method, msg[13:4 + size]


SECURE_URL = f"prudps:/address={HOST_IP};port={SECURE_PORT};CID=1;PID={SERVER_PID};sid=1;stream=3;type=2"
# Calls whose parameters contain the player's real Ubisoft credentials.
SENSITIVE_CALLS = {(10, 2), (11, 4)}   # TicketGranting.LoginEx, SecureConnection.RegisterEx
accounts = Accounts()
# Track Central library (track_library.db, built by tools/build_track_library.py); optional.
track_library = open_library() if os.environ.get("TRIALS_LIBRARY", "1") != "0" else None
if track_library:
    log(f"track library: {len(track_library)} tracks by {len(track_library.creators)} creators")
user_storage = UserStorage(accounts, log, HOST_IP, track_library)
secure_connection = SecureConnection(SECURE_URL, log)
SERVICES = {
    "auth": {TicketGranting.PROTO: TicketGranting(accounts, SECURE_URL, log)},
    "secure": {
        SecureConnection.PROTO: secure_connection,
        MatchMaking.PROTO: MatchMaking(secure_connection, log, lambda pid: is_online(pid)),
        Tracking.PROTO: Tracking(),
        Privileges.PROTO: Privileges(),
        UbiAccountManagement.PROTO: UbiAccountManagement(accounts, track_library),
        TitleStorage.PROTO: TitleStorage(),
        PlayerStats.PROTO: PlayerStats(accounts, log, user_storage),
        UserGeneratedContent.PROTO: UserGeneratedContent(accounts, log, track_library),
        UplayWin.PROTO: UplayWin(),
        UserStorage.PROTO: user_storage,
    },
}


def make_handler(name):
    services = SERVICES[name]

    deferred = set()

    def on_message(conn, msg):
        try:
            proto, call_id, method, params = parse_rmc_request(msg)
        except Exception as e:
            log(f"[{name}] undecodable message ({e}): {len(msg)} bytes")
            return
        key = (conn.pid, proto, method, call_id)
        if key not in deferred:   # retransmissions of a deferred call are logged once
            if (proto, method) in SENSITIVE_CALLS:
                debug(f"[{name}] RMC pid={conn.pid} proto={proto} method={method} call={call_id} params=<redacted>")
            else:
                debug(f"[{name}] RMC pid={conn.pid} proto={proto} method={method} call={call_id} params={params.hex()}")
                debug(f"[{name}]     ascii: {''.join(chr(b) if 32 <= b < 127 else '.' for b in params)}")
        service = services.get(proto)
        response = service.handle(conn, call_id, method, params) if service else None
        if response is None:
            # Not implemented yet: leave it unacked so the client retransmits instead of
            # failing; it gets answered after the service is added and the server restarts.
            if key not in deferred:
                log(f"[{name}]     -> DEFERRED: no handler for protocol {proto} method {method}")
                deferred.add(key)
            return False
        deferred.discard(key)
        conn.send_data(response)

    return on_message


PRUDP_SERVERS = {}
ONLINE_TIMEOUT = 90   # seconds without any packet; connected games keep pinging the server


def is_online(pid):
    """True while pid has a secure-server connection that has been heard from recently."""
    server = PRUDP_SERVERS.get("secure")
    if server is None:
        return True
    now = time.time()
    return any(c.pid == pid and now - c.last_seen < ONLINE_TIMEOUT for c in list(server.conns.values()))


def run_prudp(name, port):
    on_connect = secure_on_connect(log) if name == "secure" else None
    server = PRUDP_SERVERS[name] = PRUDPServer(name, port, 0x31, make_handler(name), log, on_connect, debug)
    server.serve()


if __name__ == "__main__":
    threading.Thread(target=run_prudp, args=("auth", AUTH_PORT), daemon=True).start()
    threading.Thread(target=run_prudp, args=("secure", SECURE_PORT), daemon=True).start()
    httpd = ThreadingHTTPServer(("0.0.0.0", 80), ConfigHandler)
    log(f"HTTP listening on 80, advertising {HOST_IP}" + (" (TRIALS_DEBUG on)" if DEBUG else ""))
    httpd.serve_forever()
