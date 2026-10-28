"""Quazal PRUDP v0 transport (Ubisoft flavour) as used by Trials Evolution PC.

Packet layout (verified against captured SYN packets):
    u8  src vport     u8  dst vport     u8  type_flags (type = &7, flags = >>3)
    u8  session id    u32 signature     u16 sequence
    [u32 connection signature]   SYN / CONNECT only
    [u8  fragment id]            DATA only
    [u16 payload size]           when FLAG_HAS_SIZE
    payload
    u8  checksum                 sum-of-access-key based
"""
import collections
import json
import os
import socket
import struct
import threading
import time
import zlib

SYN, CONNECT, DATA, DISCONNECT, PING = range(5)
TYPE_NAMES = {SYN: "SYN", CONNECT: "CONNECT", DATA: "DATA", DISCONNECT: "DISCONNECT", PING: "PING"}
FLAG_ACK, FLAG_RELIABLE, FLAG_NEED_ACK, FLAG_HAS_SIZE = 1, 2, 4, 8

ACCESS_KEY = b"6Xaw3Ror"
RC4_KEY = b"CD&ML"


def checksum(data, key=ACCESS_KEY):
    words = len(data) // 4
    total = sum(struct.unpack_from(f"<{words}I", data)) & 0xFFFFFFFF
    return (sum(key) + sum(data[words * 4:]) + sum(total.to_bytes(4, "little"))) & 0xFF


class RC4:
    def __init__(self, key):
        s = list(range(256))
        j = 0
        for i in range(256):
            j = (j + s[i] + key[i % len(key)]) & 0xFF
            s[i], s[j] = s[j], s[i]
        self.s, self.i, self.j = s, 0, 0

    def crypt(self, data):
        s, i, j = self.s, self.i, self.j
        out = bytearray(len(data))
        for n, b in enumerate(data):
            i = (i + 1) & 0xFF
            j = (j + s[i]) & 0xFF
            s[i], s[j] = s[j], s[i]
            out[n] = b ^ s[(s[i] + s[j]) & 0xFF]
        self.i, self.j = i, j
        return bytes(out)


class Packet:
    def __init__(self, src, dst, ptype, flags, session=0, sig=0, seq=0, conn_sig=0, frag=0, payload=b""):
        self.src, self.dst, self.type, self.flags = src, dst, ptype, flags
        self.session, self.sig, self.seq, self.conn_sig, self.frag = session, sig, seq, conn_sig, frag
        self.payload = payload

    @classmethod
    def parse(cls, data):
        if checksum(data[:-1]) != data[-1]:
            raise ValueError("bad checksum")
        src, dst, tf, session, sig, seq = struct.unpack_from("<BBBBIH", data)
        p = cls(src, dst, tf & 7, tf >> 3, session, sig, seq)
        off = 10
        if p.type in (SYN, CONNECT):
            (p.conn_sig,) = struct.unpack_from("<I", data, off)
            off += 4
        if p.type == DATA:
            p.frag = data[off]
            off += 1
        end = len(data) - 1
        if p.flags & FLAG_HAS_SIZE:
            (size,) = struct.unpack_from("<H", data, off)
            off += 2
            end = off + size
        p.payload = data[off:end]
        return p

    def build(self):
        out = struct.pack("<BBBBIH", self.src, self.dst, self.type | (self.flags << 3),
                          self.session, self.sig, self.seq)
        if self.type in (SYN, CONNECT):
            out += struct.pack("<I", self.conn_sig)
        if self.type == DATA:
            out += bytes([self.frag])
        if self.flags & FLAG_HAS_SIZE:
            out += struct.pack("<H", len(self.payload))
        out += self.payload
        return out + bytes([checksum(out)])

    def __repr__(self):
        flags = "".join(c for bit, c in ((1, "A"), (2, "R"), (4, "N"), (8, "S")) if self.flags & bit)
        return (f"<{TYPE_NAMES.get(self.type, self.type)} {flags or '-'} {self.src:02x}->{self.dst:02x} "
                f"sess={self.session} sig={self.sig:08x} seq={self.seq} csig={self.conn_sig:08x} "
                f"frag={self.frag} len={len(self.payload)}>")


class Connection:
    def __init__(self, server, addr, client_vport):
        self.server, self.addr, self.client_vport = server, addr, client_vport
        self.server_sig = struct.unpack("<I", os.urandom(4))[0]
        self.client_sig = 0
        self.session = 0
        self.send_seq = 1
        self.rx = RC4(RC4_KEY)
        self.tx = RC4(RC4_KEY)
        self.seen = set()
        self.frag_buf = b""
        self.last_seen = time.time()
        self.pid = 0
        self.lock = threading.Lock()

    def send_raw(self, p):
        self.server.sock.sendto(p.build(), self.addr)

    def reply(self, ptype, flags, **kw):
        p = Packet(self.server.vport, self.client_vport, ptype, flags,
                   session=self.session, sig=self.client_sig, **kw)
        self.send_raw(p)
        return p

    # Payload bytes per DATA packet. The client silently drops packets of ~1014 bytes (1001-byte
    # fragments were never acknowledged) but accepts ~910, so stay well under its ~1000-byte MTU.
    FRAGMENT_SIZE = 800

    def send_data(self, plaintext):
        """Send one RMC message reliably, fragmenting it like the client does: each fragment is its
        own reliable DATA packet (own seq, own fresh-RC4 payload with a compression byte); fragment
        ids count 1, 2, ... and the final fragment has id 0."""
        chunks = [plaintext[i:i + self.FRAGMENT_SIZE] for i in range(0, len(plaintext), self.FRAGMENT_SIZE)] or [b""]
        for index, chunk in enumerate(chunks, 1):
            frag = 0 if index == len(chunks) else index
            body = RC4(RC4_KEY).crypt(b"\x00" + chunk)  # 0 = uncompressed
            with self.lock:
                seq = self.send_seq
                self.send_seq += 1
            p = self.reply(DATA, FLAG_RELIABLE | FLAG_NEED_ACK | FLAG_HAS_SIZE, seq=seq, frag=frag, payload=body)
            self.server.track_unacked(self, p)
        self.server.save_sessions()

    def to_dict(self):
        return {"host": self.addr[0], "port": self.addr[1], "client_vport": self.client_vport,
                "server_sig": self.server_sig, "client_sig": self.client_sig,
                "session": self.session, "send_seq": self.send_seq, "pid": self.pid}

    @classmethod
    def from_dict(cls, server, d):
        conn = cls(server, (d["host"], d["port"]), d["client_vport"])
        conn.server_sig, conn.client_sig = d["server_sig"], d["client_sig"]
        conn.session, conn.send_seq, conn.pid = d["session"], d["send_seq"], d["pid"]
        return conn

    def decode_payload(self, payload):
        if not payload:
            return b""
        # Each packet is encrypted with a fresh RC4 state; the compression byte is inside it.
        data = RC4(RC4_KEY).crypt(payload)
        if data[0]:
            return zlib.decompress(data[1:])
        return data[1:]


# NAT echo (OnlineSDK NATTraversalEngine, JobGetPublicURL). Before hosting, the game sends
# small packets from its game socket to the secure server's port to learn its public address:
# a 14-byte game transport header (71 71 05 00 ... ef be ad de), one message byte, then a
# checksum byte = sum of all previous bytes. Message 2 = ECHO, answered with message 2 followed
# by the station URL we saw it from; message 3 = KEEP_ALIVE, needs no answer.
NAT_MAGIC = b"qq"
NAT_HEADER_SIZE = 14
NAT_ECHO, NAT_KEEP_ALIVE = 2, 3
# Recent NAT packets as (ip, port, time): the outside address of each game socket, which is where
# other players must connect to reach a session it hosts. Matched to sessions by time (quazal.py).
NAT_SEEN = collections.deque(maxlen=2000)
NAT_ECHO_ENABLED = False      # multiplayer only (server.py: TRIALS_MULTIPLAYER=1)


def nat_echo_reply(data, addr):
    if len(data) != NAT_HEADER_SIZE + 2 or sum(data[:-1]) & 0xFF != data[-1]:
        return None
    if data[NAT_HEADER_SIZE] != NAT_ECHO:
        return None
    body = data[:NAT_HEADER_SIZE] + bytes([NAT_ECHO]) + f"udp:/address={addr[0]};port={addr[1]}".encode() + b"\x00"
    return body + bytes([sum(body) & 0xFF])


class PRUDPServer:
    def __init__(self, name, port, vport, on_message, log, on_connect=None, debug=None):
        self.name, self.port, self.vport = name, port, vport
        self.on_message, self.log, self.on_connect = on_message, log, on_connect
        # Packet-level lines carry player IPs and raw bytes: they only go to the debug log.
        self.debug = debug or (lambda msg: None)
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.bind(("0.0.0.0", port))
        self.conns = {}
        self.unacked = {}
        self.unacked_lock = threading.Lock()
        # Connection state survives server restarts, so redeploying doesn't drop players.
        self.state_path = f"sessions_{name}.json"
        self.state_lock = threading.Lock()
        self.load_sessions()

    def load_sessions(self):
        try:
            with open(self.state_path, encoding="utf-8") as f:
                for d in json.load(f):
                    conn = Connection.from_dict(self, d)
                    self.conns[(conn.addr, conn.client_vport)] = conn
            self.log(f"[{self.name}] restored {len(self.conns)} session(s)")
        except FileNotFoundError:
            pass
        except Exception as e:
            self.log(f"[{self.name}] could not restore sessions: {e}")

    def save_sessions(self):
        with self.state_lock:
            tmp = self.state_path + ".tmp"
            with open(tmp, "w", encoding="utf-8") as f:
                json.dump([c.to_dict() for c in list(self.conns.values()) if c.client_sig], f)
            os.replace(tmp, self.state_path)

    def track_unacked(self, conn, p):
        with self.unacked_lock:
            self.unacked[(conn.addr, conn.client_vport, p.seq)] = [p, time.time(), 0, conn]

    def resend_loop(self):
        while True:
            time.sleep(0.5)
            now = time.time()
            with self.unacked_lock:
                for key, item in list(self.unacked.items()):
                    p, sent, tries, conn = item
                    if now - sent > 1.0:
                        if tries >= 10:
                            del self.unacked[key]
                            if os.environ.get("TRIALS_LOG_ACKS"):
                                self.log(f"[{self.name}] gave up on seq={p.seq} frag={p.frag} "
                                         f"({len(p.payload)} bytes): never acknowledged")
                            continue
                        conn.send_raw(p)
                        item[1], item[2] = now, tries + 1

    def serve(self):
        threading.Thread(target=self.resend_loop, daemon=True).start()
        self.log(f"[{self.name}] PRUDP listening on UDP {self.port} (vport {self.vport:02x})")
        while True:
            data, addr = self.sock.recvfrom(65535)
            if NAT_ECHO_ENABLED and data[:2] == NAT_MAGIC:
                NAT_SEEN.append((addr[0], addr[1], time.time()))
                reply = nat_echo_reply(data, addr)
                if reply:
                    self.sock.sendto(reply, addr)
                    self.debug(f"[{self.name}] NAT echo from {addr}")
                continue
            try:
                p = Packet.parse(data)
            except Exception as e:
                self.debug(f"[{self.name}] bad packet from {addr}: {e} {data.hex()}")
                continue
            try:
                self.handle(addr, p)
            except Exception as e:
                import traceback
                self.log(f"[{self.name}] handler error: {e}\n{traceback.format_exc()}")

    def handle(self, addr, p):
        key = (addr, p.src)
        if p.flags & FLAG_ACK:
            with self.unacked_lock:
                had = self.unacked.pop((addr, p.src, p.seq), None)
            if had and os.environ.get("TRIALS_LOG_ACKS"):
                self.log(f"[{self.name}] ack seq={p.seq} frag={had[0].frag} after {had[2]} resends")
            return

        if p.type == SYN:
            conn = Connection(self, addr, p.src)
            self.conns[key] = conn
            self.debug(f"[{self.name}] {addr[0]}:{addr[1]} {p} -> SYN-ACK")
            conn.reply(SYN, FLAG_ACK, seq=0, conn_sig=conn.server_sig)
            return

        conn = self.conns.get(key)
        if conn is None:
            self.debug(f"[{self.name}] packet for unknown connection {addr} {p}")
            return
        conn.last_seen = time.time()

        if p.type == CONNECT:
            conn.client_sig = p.conn_sig
            conn.session = p.session
            ack_payload = b""
            if self.on_connect and p.payload:
                # CONNECT payloads use the same RC4 + compression-byte framing as DATA.
                reply = self.on_connect(conn, conn.decode_payload(p.payload))
                if reply:
                    ack_payload = RC4(RC4_KEY).crypt(b"\x00" + reply)
            self.debug(f"[{self.name}] {p} payload={p.payload.hex()} -> CONNECT-ACK {ack_payload.hex()}")
            flags = FLAG_ACK | (FLAG_HAS_SIZE if ack_payload else 0)
            conn.reply(CONNECT, flags, seq=p.seq, conn_sig=0, payload=ack_payload)
            self.save_sessions()
            return

        if p.type == PING:
            if p.flags & FLAG_NEED_ACK:
                conn.reply(PING, FLAG_ACK, seq=p.seq)
            return

        if p.type == DISCONNECT:
            self.debug(f"[{self.name}] {addr} disconnected")
            conn.reply(DISCONNECT, FLAG_ACK, seq=p.seq)
            self.conns.pop(key, None)
            self.save_sessions()
            return

        if p.type == DATA:
            def ack():
                if p.flags & FLAG_NEED_ACK:
                    conn.reply(DATA, FLAG_ACK, seq=p.seq, frag=p.frag)

            if p.seq in conn.seen or not p.payload:
                ack()
                return
            if p.frag != 0:   # more fragments follow
                ack()
                conn.seen.add(p.seq)
                conn.frag_buf += conn.decode_payload(p.payload)
                return
            msg = conn.frag_buf + conn.decode_payload(p.payload)
            # on_message returns False for calls we can't answer yet. Leaving those unacked
            # makes the client keep retransmitting instead of showing a hard error, so the
            # call is answered once the service exists and the server is restarted.
            if self.on_message(conn, msg) is False:
                return
            conn.frag_buf = b""
            conn.seen.add(p.seq)
            ack()
