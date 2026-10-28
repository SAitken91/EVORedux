"""Quazal RMC serialization, Kerberos tickets and the TicketGranting / SecureConnection services."""
import hashlib
import json
import hmac
import os
import re
import secrets
import socket
import sqlite3
import struct
import threading
import time
import uuid

from prudp import NAT_SEEN, RC4
from track_library import BLOB_ID_BASE, PROP_BLOB_TRACK, TYPE_SHOT_BLOB, TYPE_TRACK_BLOB, blob_content_id

SERVER_PID = 2           # pid of the secure server principal


def _server_secret(path="server_secret.key"):
    """Per-install random secret. Tickets are sealed with it, so it must not be guessable or
    published: anyone who knows it can mint a ticket for any player. Created on first run."""
    try:
        with open(path, "rb") as f:
            return f.read().strip()
    except FileNotFoundError:
        key = secrets.token_hex(32).encode()
        fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
        with os.fdopen(fd, "wb") as f:
            f.write(key)
        return key


SECURE_PASSWORD = _server_secret()
# Kerberos passwords the client derives its key from (see JobUbiAuthenticationLogin and the
# Guest login path in the decompiled SDK). Player LoginEx uses the dummy UbiAuthentication password.
DEFAULT_PASSWORD = b"UbiDummyPwd"
ACCOUNT_PASSWORDS = {"Tracking": b"JaDe!", "Guest": b"h7fyctiuucf"}

QRESULT_SUCCESS = 0x00010001
QRESULT_NOT_IMPLEMENTED = 0x80010002
QRESULT_INVALID_USERNAME = 0x80030064
QRESULT_INVALID_PASSWORD = 0x80030065
QRESULT_ACCESS_DENIED = 0x8003006b
# The revival client shim replaces the Ubisoft password with a per-install key starting with this.
REVIVAL_KEY_PREFIX = "trv1-"


class Reader:
    def __init__(self, data):
        self.data, self.off = data, 0

    def take(self, n):
        out = self.data[self.off:self.off + n]
        self.off += n
        return out

    def u8(self): return self.take(1)[0]
    def u16(self): return struct.unpack("<H", self.take(2))[0]
    def u32(self): return struct.unpack("<I", self.take(4))[0]
    def u64(self): return struct.unpack("<Q", self.take(8))[0]

    def string(self):
        n = self.u16()
        return self.take(n).rstrip(b"\x00").decode("utf-8", "replace")

    def buffer(self):
        return self.take(self.u32())

    def rest(self):
        return self.data[self.off:]


class Writer:
    def __init__(self):
        self.parts = []

    def u8(self, v): self.parts.append(struct.pack("<B", v)); return self
    def u16(self, v): self.parts.append(struct.pack("<H", v)); return self
    def u32(self, v): self.parts.append(struct.pack("<I", v)); return self
    def u64(self, v): self.parts.append(struct.pack("<Q", v)); return self
    def raw(self, b): self.parts.append(b); return self

    def string(self, s):
        b = s.encode() + b"\x00"
        return self.u16(len(b)).raw(b)

    def buffer(self, b):
        return self.u32(len(b)).raw(b)

    def bytes(self):
        return b"".join(self.parts)


def rmc_response(proto, call_id, method, body):
    inner = struct.pack("<BBII", proto, 1, call_id, method | 0x8000) + body
    return struct.pack("<I", len(inner)) + inner


def rmc_error(proto, call_id, error):
    inner = struct.pack("<BBII", proto, 0, error, call_id)
    return struct.pack("<I", len(inner)) + inner


# ---- Kerberos ----------------------------------------------------------------

def derive_key(pid, password):
    key = password
    for _ in range(65000 + pid % 1024):
        key = hashlib.md5(key).digest()
    return key


def kerberos_encrypt(key, data):
    enc = RC4(key).crypt(data)
    return enc + hmac.new(key, enc, hashlib.md5).digest()


def kerberos_decrypt(key, data):
    enc, mac = data[:-16], data[-16:]
    if hmac.new(key, enc, hashlib.md5).digest() != mac:
        raise ValueError("kerberos HMAC mismatch")
    return RC4(key).crypt(enc)


SECURE_KEY = derive_key(SERVER_PID, SECURE_PASSWORD)


def make_ticket(user_pid, user_password, target_pid):
    session_key = os.urandom(16)
    inner = kerberos_encrypt(SECURE_KEY, session_key + struct.pack("<I", user_pid))
    outer = session_key + struct.pack("<I", target_pid) + struct.pack("<I", len(inner)) + inner
    return kerberos_encrypt(derive_key(user_pid, user_password), outer)


def open_ticket(ticket):
    plain = kerberos_decrypt(SECURE_KEY, ticket)
    return plain[:16], struct.unpack_from("<I", plain, 16)[0]


# ---- Accounts ----------------------------------------------------------------

class Accounts:
    def __init__(self, path="trials.db"):
        self.db = sqlite3.connect(path, check_same_thread=False)
        self.lock = threading.Lock()
        self.db.execute("CREATE TABLE IF NOT EXISTS accounts (pid INTEGER PRIMARY KEY, name TEXT UNIQUE)")
        self.db.execute("INSERT OR IGNORE INTO accounts (pid, name) VALUES (100, 'Tracking')")
        # Optional name shown on the boards instead of the login name (tools/rename_player.py).
        # The login name stays the account key: the game always logs in with it.
        if "display" not in [r[1] for r in self.db.execute("PRAGMA table_info(accounts)")]:
            self.db.execute("ALTER TABLE accounts ADD COLUMN display TEXT")
        # Name claims: the first revival key used with a name owns it. Only an HMAC of the key is
        # kept (keyed with the server secret); the key itself is never stored or logged.
        self.db.execute("CREATE TABLE IF NOT EXISTS account_keys (name TEXT PRIMARY KEY, key_hmac TEXT, "
                        "claimed REAL)")
        self.db.commit()

    def check_key(self, name, key):
        """True if `key` may log in as `name`; claims the name if nobody has yet."""
        digest = hmac.new(SECURE_PASSWORD, key.encode(), hashlib.sha256).hexdigest()
        with self.lock:
            row = self.db.execute("SELECT key_hmac FROM account_keys WHERE name=?", (name.lower(),)).fetchone()
            if row is None:
                self.db.execute("INSERT INTO account_keys VALUES (?, ?, ?)", (name.lower(), digest, time.time()))
                self.db.commit()
                return True
            return hmac.compare_digest(row[0], digest)

    def release_name(self, name):
        """Admin: let the next login with this name claim it again (e.g. a player lost their key)."""
        with self.lock:
            self.db.execute("DELETE FROM account_keys WHERE name=?", (name.lower(),))
            self.db.commit()

    def pid_for(self, name):
        with self.lock:
            row = self.db.execute("SELECT pid FROM accounts WHERE name=?", (name,)).fetchone()
            if row:
                return row[0]
            # New live accounts start at 1,000,000: historical Track Central boards reference
            # original-server PIDs up to ~315,000 and the two must never collide.
            cur = self.db.execute("INSERT INTO accounts (pid, name) VALUES "
                                  "(MAX((SELECT MAX(pid)+1 FROM accounts), 1000000), ?)", (name,))
            self.db.commit()
            return cur.lastrowid

    def name_for(self, pid):
        row = self.db.execute("SELECT name FROM accounts WHERE pid=?", (pid,)).fetchone()
        return row[0] if row else None

    def display_name(self, pid):
        """Name shown to other players: the admin-set display name if any, else the login name.
        A login name that is an e-mail address is never shown."""
        row = self.db.execute("SELECT name, display FROM accounts WHERE pid=?", (pid,)).fetchone()
        if row is None:
            return None
        name, display = row
        if display:
            return display
        return f"Rider{pid}" if "@" in name else name


# ---- TicketGranting (protocol 10) ---------------------------------------------

def revival_key(extra):
    """The revival key inside LoginEx's custom data, or None if the client has no shim.

    Scans the custom data's strings for one carrying the shim's prefix rather than relying on the
    field order. Nothing else in it is kept; this returns only a string that has the prefix.
    """
    off = 0
    while off + 2 <= len(extra):
        size = struct.unpack_from("<H", extra, off)[0]
        if 2 <= size <= 64 and off + 2 + size <= len(extra):
            value = extra[off + 2:off + 2 + size].rstrip(b"\x00")
            if value.startswith(REVIVAL_KEY_PREFIX.encode()) and value.isascii() and len(value) >= 20:
                return value.decode()
        off += 1
    return None


class TicketGranting:
    PROTO = 10

    def __init__(self, accounts, secure_url, log):
        self.accounts, self.secure_url, self.log = accounts, secure_url, log

    def password_for(self, name):
        return ACCOUNT_PASSWORDS.get(name, DEFAULT_PASSWORD)

    def login_body(self, conn, name):
        pid = self.accounts.pid_for(name)
        conn.pid = pid                      # this connection may now request tickets for this pid
        ticket = make_ticket(pid, self.password_for(name), SERVER_PID)
        self.log(f"[auth] login -> pid {pid}")
        return (Writer()
                .u32(QRESULT_SUCCESS)
                .u32(pid)
                .buffer(ticket)
                # RVConnectionData
                .string(self.secure_url)
                .u32(0)                    # m_lstSpecialProtocols
                .string("")                # m_urlSpecialProtocols
                .string("trials-revive")   # strReturnMsg
                .bytes())

    def handle(self, conn, call_id, method, params):
        r = Reader(params)
        if method == 1:      # Login(strUserName): only the game's built-in service accounts
            name = r.string()
            if name not in ACCOUNT_PASSWORDS:
                self.log("[auth] plain Login refused for a player name")
                return rmc_error(self.PROTO, call_id, QRESULT_ACCESS_DENIED)
            return rmc_response(self.PROTO, call_id, method, self.login_body(conn, name))
        if method == 2:      # LoginEx(strUserName, oExtraData)
            # oExtraData is UbiAuthenticationLoginCustomData. With the revival shim installed its
            # password field is the player's revival key; without it, it is their REAL Ubisoft
            # password. Either way it is never logged or stored: only the key's HMAC is kept.
            name = r.string()
            key = revival_key(r.rest())
            if key is None:
                self.log("[auth] login refused: client shim not installed")
                return rmc_error(self.PROTO, call_id, QRESULT_INVALID_PASSWORD)
            if not name or name in ACCOUNT_PASSWORDS or not self.accounts.check_key(name, key):
                self.log("[auth] login refused: name is claimed by another key")
                return rmc_error(self.PROTO, call_id, QRESULT_INVALID_PASSWORD)
            return rmc_response(self.PROTO, call_id, method, self.login_body(conn, name))
        if method == 3:      # RequestTicket(idSource, idTarget)
            source, target = r.u32(), r.u32()
            if not conn.pid or source != conn.pid:   # only for the account logged in on this connection
                return rmc_error(self.PROTO, call_id, QRESULT_ACCESS_DENIED)
            name = self.accounts.name_for(source) or ""
            ticket = make_ticket(source, self.password_for(name), target)
            body = Writer().u32(QRESULT_SUCCESS).buffer(ticket).bytes()
            return rmc_response(self.PROTO, call_id, method, body)
        return None   # unknown method: deferred by the dispatcher


# ---- Tracking / telemetry (protocol 36) ----------------------------------------

class Tracking:
    """Ubisoft telemetry. We ask for no tags and discard anything sent."""
    PROTO = 36
    GET_CONFIGURATION = 4

    def handle(self, conn, call_id, method, params):
        if method == self.GET_CONFIGURATION:
            return rmc_response(self.PROTO, call_id, method, Writer().u32(0).bytes())  # tags: empty list
        # SendTag(1), SendTagAndUpdateUserInfo(2), SendUserInfo(3), SendTags(5): void
        return rmc_response(self.PROTO, call_id, method, b"")


# ---- UbiAccountManagement (protocol 29): account lookups --------------------------

def ubi_account_id(name):
    """Stable stand-in for a Ubisoft account id (they are GUID strings). Keyed with the server
    secret so an id handed to other players can't be checked against guessed login names."""
    digest = hmac.new(SECURE_PASSWORD, name.encode(), hashlib.sha256).digest()
    return str(uuid.UUID(bytes=digest[:16], version=5))


LEGACY_PID_BASE = 2_000_000_000
_legacy_by_uid = {}


def legacy_board_pid(original_pid):
    """PID used for an original-server player on served leaderboards.

    Kept apart from original PIDs on purpose: resolving original PIDs (e.g. track creators) makes
    the client hide installed Track Central tracks, but leaderboard rows must resolve to an account
    and name or the board stays on "loading". So boards use LEGACY_PID_BASE + original PID.
    """
    return LEGACY_PID_BASE + original_pid


def legacy_account_id(board_pid):
    uid = str(uuid.uuid5(uuid.NAMESPACE_URL, f"trials-revive:legacy:{board_pid - LEGACY_PID_BASE}"))
    _legacy_by_uid[uid] = board_pid
    return uid


def legacy_name(ubi_id):
    pid = _legacy_by_uid.get(ubi_id)
    return f"Legacy#{pid - LEGACY_PID_BASE}" if pid is not None else None


def is_legacy_board_pid(pid):
    return pid >= LEGACY_PID_BASE


class UbiAccountManagement:
    PROTO = 29

    def __init__(self, accounts, library=None):
        self.accounts, self.library = accounts, library

    def _all(self):
        rows = self.accounts.db.execute("SELECT pid, name FROM accounts").fetchall()
        return [(pid, name, ubi_account_id(name)) for pid, name in rows]

    def handle(self, conn, call_id, method, params):
        r = Reader(params)
        rows = self._all()
        by_pid = {pid: (name, uid) for pid, name, uid in rows}
        by_name = {name.lower(): uid for _, name, uid in rows}
        by_uid = {uid: (pid, name) for pid, name, uid in rows}
        w = Writer()
        if method == 11:                       # LookupPrincipalIds(list<ubiId>) -> map<ubiId, pid>
            uids = [r.string() for _ in range(r.u32())]
            hits = [(u, by_uid[u][0]) for u in uids if u in by_uid]
            if self.library:
                hits += [(u, self.library.creator_by_account_id(u)) for u in uids
                         if u not in by_uid and self.library.creator_by_account_id(u) is not None]
            hits += [(u, _legacy_by_uid[u]) for u in uids if u in _legacy_by_uid and u not in by_uid]
            w.u32(len(hits))
            for u, pid in hits:
                w.string(u).u32(pid)
        elif method == 12:                     # LookupUbiAccountIDsByPids(list<pid>) -> map<pid, ubiId>
            # Only resolve players this server knows. Resolving original-server players (track
            # creators) to placeholder accounts made the client stall loading Track Central and
            # hide every installed track, so unknown PIDs are left out, as the game expects.
            # Xbox 360 track creators (track library) are answered too: their pids are XUID halves
            # read from installed 360 tracks, and left unanswered the game re-asks forever and its
            # full gamertag queue crashes the next lookup. They come before the legacy range, which
            # some XUID halves fall into.
            pids = list(dict.fromkeys(r.u32() for _ in range(r.u32())))   # map keys must be unique
            hits = []
            for p in pids:
                if p in by_pid:
                    hits.append((p, by_pid[p][1]))
                elif self.library and self.library.is_creator(p):
                    hits.append((p, self.library.creator_account_id(p)))
                elif is_legacy_board_pid(p):
                    hits.append((p, legacy_account_id(p)))
            w.u32(len(hits))
            for pid, uid in hits:
                w.u32(pid).string(uid)
        elif method in (13, 18):               # LookupUbiAccountIDsByUsernames[Global] -> map<name, ubiId>
            hits = [(n, by_name[n.lower()]) for n in (r.string() for _ in range(r.u32())) if n.lower() in by_name]
            w.u32(len(hits))
            for name, uid in hits:
                w.string(name).string(uid)
        elif method in (14, 20):               # LookupUsernamesByUbiAccountIDs[Global] -> map<ubiId, name>
            uids = [r.string() for _ in range(r.u32())]
            hits = [(u, self.accounts.display_name(by_uid[u][0])) for u in uids if u in by_uid]
            if self.library:
                hits += [(u, self.library.creators[self.library.creator_by_account_id(u)])
                         for u in uids if u not in by_uid and self.library.creator_by_account_id(u) is not None]
            hits += [(u, legacy_name(u)) for u in uids if u not in by_uid and legacy_name(u)]
            w.u32(len(hits))
            for uid, name in hits:
                w.string(uid).string(name)
        else:
            return None
        return rmc_response(self.PROTO, call_id, method, w.bytes())


# ---- TitleStorage (protocol 51): TMS news / featured content --------------------

class TitleStorage:
    PROTO = 51

    def handle(self, conn, call_id, method, params):
        if method in (1, 2):   # BrowseContents / BrowseContentsByType -> empty content list
            return rmc_response(self.PROTO, call_id, method, Writer().u32(0).bytes())
        return None


# ---- PlayerStats (protocol 55): leaderboards -----------------------------------

def quazal_datetime(t=None):
    """Quazal DateTime: sec | min<<6 | hour<<12 | day<<17 | month<<22 | year<<26."""
    g = time.gmtime(t)
    return (g.tm_sec | g.tm_min << 6 | g.tm_hour << 12 | g.tm_mday << 17
            | g.tm_mon << 22 | g.tm_year << 26)


class PlayerStats:
    """Leaderboards built from the game's own WriteStats uploads.

    Decoded from real uploads: on a race track board stat 100 = the game's own score (higher is
    better, 0 on skill games), stat 101 = faults and stat 102 = time in ms; on a points skill game
    stat 101 = the points. Boards 1 and 2 are global ranking boards ordered by stat 160.
    Track boards keep each player's best run; global boards keep the latest totals.
    """
    PROTO = 55
    WRITE_STATS = 1
    READ_STATS_BY_PLAYERS = 2
    NEAR_PLAYER, BY_RANK, BY_PLAYERS = 3, 4, 5
    NEAR_PLAYER2, BY_RANK2, BY_PLAYERS2 = 10, 11, 12
    GLOBAL_BOARDS = {1, 2}
    STAT_FAULTS, STAT_TIME, STAT_RANKING = 101, 102, 160
    STAT_POINTS = 101       # skill games: same slot as faults
    STAT_GHOST_SIZE = 104   # size of the ghost uploaded for this run

    def __init__(self, accounts, log, storage=None, boards_path="boards.json"):
        self.accounts, self.db, self.log, self.storage = accounts, accounts.db, log, storage
        self.lock = threading.Lock()
        try:
            with open(boards_path, encoding="utf-8") as f:
                self.higher_is_better = set(json.load(f).get("higher_is_better", []))
        except FileNotFoundError:
            self.higher_is_better = set()
        self.db.execute("CREATE TABLE IF NOT EXISTS stat_writes "
                        "(id INTEGER PRIMARY KEY, pid INTEGER, received REAL, params BLOB)")
        self.db.execute("CREATE TABLE IF NOT EXISTS player_stats (pid INTEGER, board INTEGER, "
                        "stats TEXT, submitted REAL, PRIMARY KEY (pid, board))")
        # Rebuild bests from the raw upload history so board-order changes apply retroactively.
        self.db.execute("DELETE FROM player_stats")
        for pid, received, params in self.db.execute(
                "SELECT pid, received, params FROM stat_writes ORDER BY id").fetchall():
            self._apply_write(pid, params, received, replay=True)
        self.db.commit()

    # -- storage --
    def _sort_key(self, board, stats):
        if board in self.GLOBAL_BOARDS:
            return (-stats.get(self.STAT_RANKING, (1, 0))[1],)
        if board in self.higher_is_better:   # skill games: further / more points is better
            return (-stats.get(self.STAT_POINTS, (1, 0))[1],)
        return (stats.get(self.STAT_FAULTS, (1, 1 << 62))[1], stats.get(self.STAT_TIME, (1, 1 << 62))[1])

    def _apply_write(self, pid, params, when, replay=False):
        r = Reader(params)
        with self.lock:
            for _ in range(r.u32()):
                board = r.u32()
                [r.u32() for _ in range(r.u32())]                  # m_contextIDs
                stats = {}
                for _ in range(r.u32()):
                    sid = r.u32()
                    stats[sid] = read_variant(r)
                row = self.db.execute("SELECT stats FROM player_stats WHERE pid=? AND board=?",
                                      (pid, board)).fetchone()
                best = True
                if row and board not in self.GLOBAL_BOARDS:
                    old = {int(k): tuple(v) for k, v in json.loads(row[0]).items()}
                    best = self._sort_key(board, stats) < self._sort_key(board, old)
                if (not replay and board not in self.GLOBAL_BOARDS and self.storage
                        and self.STAT_GHOST_SIZE in stats):
                    self.storage.resolve_ghost(pid, board, stats[self.STAT_GHOST_SIZE][1], best)
                if not best:
                    continue                                        # not a personal best
                self.db.execute("INSERT OR REPLACE INTO player_stats VALUES (?, ?, ?, ?)",
                                (pid, board, json.dumps(stats), when))
            self.db.commit()

    def _ranking(self, board):
        rows = self.db.execute("SELECT pid, stats, submitted FROM player_stats WHERE board=?",
                               (board,)).fetchall()
        entries = [(pid, {int(k): tuple(v) for k, v in json.loads(st).items()}, sub)
                   for pid, st, sub in rows]
        entries.sort(key=lambda e: (self._sort_key(board, e[1]), e[2]))
        return entries

    # -- serialization --
    def _write_stat_set(self, w, pid, stats, submitted, stat_ids):
        w.u32(pid).string(self.accounts.display_name(pid) or f"Player{pid}").u64(quazal_datetime(submitted))
        w.u32(len(stat_ids))
        for sid in stat_ids:                                        # PropertyVariant { id, value }
            t, v = stats.get(sid, (1, 0))
            w.u32(sid)
            write_variant(w, t, v)

    def _write_result(self, w, query, ranking, picks):
        board, context, reset, stat_ids = query
        w.u32(board).u32(context).u32(reset).u32(len(ranking)).u32(len(picks))
        # m_score: unchanged from what has been verified in game (stat 101); the rows read the stat set
        key_stat = self.STAT_RANKING if board in self.GLOBAL_BOARDS else self.STAT_POINTS
        for rank, (pid, stats, submitted) in picks:                 # PlayerRank
            self._write_stat_set(w, pid, stats, submitted, stat_ids)
            w.u32(0).u32(rank)                                      # m_rankStatus, m_rank (1-based)
            write_variant(w, *stats.get(key_stat, (1, 0)))          # m_score

    @staticmethod
    def _queries(r, version):
        out = []
        for _ in range(r.u32()):
            board, context, reset = r.u32(), r.u32(), r.u32()
            stats = [r.u32() for _ in range(r.u32())]
            if version == 2:
                [r.u32() for _ in range(r.u32())]                  # m_estimatedPIDs
            out.append((board, context, reset, stats))
        return out

    def handle(self, conn, call_id, method, params):
        r = Reader(params)
        if method == self.WRITE_STATS:
            now = time.time()
            self.db.execute("INSERT INTO stat_writes (pid, received, params) VALUES (?, ?, ?)",
                            (conn.pid, now, params))
            self.db.commit()
            self._apply_write(conn.pid, params, now)
            self.log(f"[stats] WriteStats from pid {conn.pid} applied ({len(params)} bytes)")
            return rmc_response(self.PROTO, call_id, method, b"")

        if method in (self.NEAR_PLAYER, self.BY_RANK, self.BY_PLAYERS,
                      self.NEAR_PLAYER2, self.BY_RANK2, self.BY_PLAYERS2):
            target_pid = start = count = 0
            pids = None
            if method in (self.NEAR_PLAYER, self.NEAR_PLAYER2):
                target_pid, count = r.u32(), r.u32()
            elif method in (self.BY_RANK, self.BY_RANK2):
                start, count = r.u32(), r.u32()
            elif method == self.BY_PLAYERS:
                pids = {r.u32() for _ in range(r.u32())}
            queries = self._queries(r, 2 if method >= 10 else 1)
            w = Writer().u32(len(queries))
            for query in queries:
                ranking = self._ranking(query[0])
                ranked = list(enumerate(ranking, 1))
                if method in (self.BY_PLAYERS, self.BY_PLAYERS2):
                    picks = [x for x in ranked if pids is None or x[1][0] in pids]
                elif method in (self.BY_RANK, self.BY_RANK2):
                    first = max(start, 1) - 1
                    picks = ranked[first:first + count]
                else:                                               # near player
                    idx = next((i for i, x in enumerate(ranked) if x[1][0] == target_pid), 0)
                    lo = max(0, idx - count // 2)
                    picks = ranked[lo:lo + count]
                self._write_result(w, query, ranking, picks)
            return rmc_response(self.PROTO, call_id, method, w.bytes())

        if method == self.READ_STATS_BY_PLAYERS:
            pids = [r.u32() for _ in range(r.u32())]
            n = r.u32()
            w = Writer().u32(n)
            for _ in range(n):                                      # StatboardQuery -> StatboardResult
                board = r.u32()
                contexts = [r.u32() for _ in range(r.u32())]
                reset = r.u32()
                stat_ids = [r.u32() for _ in range(r.u32())]
                [(r.u32(), r.u32()) for _ in range(r.u32())]      # m_sortCriterias
                rows = {pid: (stats, sub) for pid, stats, sub in self._ranking(board)}
                hits = [(p, *rows[p]) for p in pids if p in rows]
                w.u32(board).u32(contexts[0] if contexts else 0).u32(reset).u32(len(hits))
                for pid, stats, sub in hits:
                    self._write_stat_set(w, pid, stats, sub, stat_ids)
                w.u32(len(stat_ids))                                # m_defaultStatValues
                for sid in stat_ids:
                    w.u32(sid)
                    write_variant(w, 1, 0)
            return rmc_response(self.PROTO, call_id, method, w.bytes())
        return None


# ---- UserGeneratedContent (protocol 101, Trials-specific): tracks + track leaderboards ----

class UserGeneratedContent:
    """Trials-specific Track Central service. Method IDs from the client stubs; reply layouts from
    the client's reply parser (FUN_0093d110) and its per-type readers:

      UGCPlayerStatSet     = u32 pid, string name, u64 datetime, u64 score, u64 descriptor
      UGCPlayerRank        = UGCPlayerStatSet, u32 rankStatus, u32 rank, u64 displayed score
      UGCLeaderboardResult = u64 trackId, list<UGCPlayerRank>          (method 11 -> list of these)
      UGCStatboardResult   = u64 trackId, list<UGCPlayerStatSet>       (method 12 -> list of these)
      UGCContent           = u64 id, u32 owner pid, list<{u32 id, variant}>                (method 4)
      UGCContentWithMetadataBlob = u64 id, u32 owner pid, buffer metadata blob, list<property>
                                                       (methods 13 and 14 -> list of these)
    Track Central browsing (feeds, search, download) serves the track library (track_library.py).
    """
    PROTO = 101
    EMPTY_LIST = {1}                          # (list reply; verified safe in-game)
    GET_CONTENT, GET_METADATA_BLOB = 4, 5     # (u64 id) -> UGCContent / buffer
    GET_WITH_BLOBS = 13                       # (list<u64> ids) -> list<UGCContentWithMetadataBlob>
    QUERY_WITH_BLOBS = 14                     # (u32 query, u32 offset, u32 count, list<property>)
    VOID = {3, 6, 7, 8}                       # publish, download count, rate, report
    WRITE_SCORE = 10                          # (u64 trackId, u64 score, u64 descriptor)
    READ_BY_RANK = 11                         # (u64 trackId, u32 start, u32 count)
    READ_BY_PLAYERS = 12                      # (u64 trackId, list<u32> pids)
    # Track info/blob replies (4, 5, 13) are structs not mapped yet: a placeholder body corrupted
    # the client heap (crash dump: abort in free()), so they get an RMC error, which the game handles.
    NOT_DECODED = {2}
    QRESULT_UGC_UNAVAILABLE = 0x80010001
    HISTORICAL_DATE = 1512691200.0            # 2017-12-08, when the archived boards were cached

    def __init__(self, accounts, log, library=None):
        self.accounts, self.db, self.log, self.library = accounts, accounts.db, log, library
        # TRIALS_HISTORY=0 serves live runs only (used to isolate client behaviour).
        self.include_history = os.environ.get("TRIALS_HISTORY", "1") != "0"
        self.max_rows = int(os.environ.get("TRIALS_MAX_ROWS", "0"))
        self.db.execute("CREATE TABLE IF NOT EXISTS ugc_runs (id INTEGER PRIMARY KEY, pid INTEGER, "
                        "track_id INTEGER, score INTEGER, descriptor INTEGER, received REAL)")
        if "historical" not in [r[1] for r in self.db.execute("PRAGMA table_info(ugc_runs)")]:
            self.db.execute("ALTER TABLE ugc_runs ADD COLUMN historical INTEGER NOT NULL DEFAULT 0")
        # Carry over scores stored by the earlier one-row-per-track table.
        if self.db.execute("SELECT name FROM sqlite_master WHERE name='ugc_scores'").fetchone():
            self.db.execute("INSERT INTO ugc_runs (pid, track_id, score, descriptor, received) "
                            "SELECT pid, track_id, score, descriptor, received FROM ugc_scores")
            self.db.execute("DROP TABLE ugc_scores")
        self.db.commit()

    @staticmethod
    def _s64(v):   # SQLite integers are signed 64-bit; store u64s bit-for-bit
        return struct.unpack("<q", struct.pack("<Q", v & 0xFFFFFFFFFFFFFFFF))[0]

    @staticmethod
    def _u64(v):
        return v & 0xFFFFFFFFFFFFFFFF

    def _board(self, track_id):
        """Each player's best run on a user track, best first.

        The client encodes every Track Central score so that HIGHER is better, whatever the track
        type: races send 0x4000... minus their faults/time (e.g. 0x3FFFFFF3_FFC5EF00 beat
        0x3FFFFFE3_FFC6FA80, which had more faults), skill games send 0x4000... plus their points
        (0x400C5004_00000000). The descriptor only selects the display format.

        Historical rows (imported from archived boards by tools/import_history.py) rank alongside
        live runs but are keyed separately so an original-server player never merges with a live one.
        """
        rows = self.db.execute("SELECT pid, score, descriptor, received, historical FROM ugc_runs "
                               "WHERE track_id=?" + ("" if self.include_history else " AND historical=0"),
                               (self._s64(track_id),)).fetchall()
        best = {}
        for pid, score, desc, when, hist in rows:
            score, desc = self._u64(score), self._u64(desc)
            key = (bool(hist), pid)
            if key not in best or score > best[key][0]:
                best[key] = (score, desc, when or self.HISTORICAL_DATE)
        ranked = sorted(best.items(), key=lambda kv: (-kv[1][0], kv[0][0], kv[1][2]))
        return [(pid, score, desc, when, hist) for (hist, pid), (score, desc, when) in ranked]

    def _write_stat_set(self, w, pid, score, desc, when, historical=False):
        if historical:
            name, pid = f"Legacy#{pid}", legacy_board_pid(pid)
        else:
            name = self.accounts.display_name(pid) or f"Player{pid}"
        w.u32(pid).string(name).u64(quazal_datetime(when)).u64(score).u64(desc)

    def _write_content(self, w, row, with_blob):
        w.u64(row[0]).u32(row[1])
        if with_blob:
            w.buffer(self.library.blob(row[0], "metablob") or b"")
        write_properties(w, self.library.properties(row))

    def _library_reply(self, call_id, method, r):
        lib = self.library
        if method == self.QUERY_WITH_BLOBS:
            query_id, offset, count = r.u32(), r.u32(), r.u32()
            props = read_properties(r)
            rows, total = lib.query(query_id, props, offset, count) if lib else ([], 0)
            self.log(f"[ugc] query {query_id} offset={offset} count={count} props={props} -> {len(rows)}/{total}")
        elif method == self.GET_WITH_BLOBS:
            ids = [r.u64() for _ in range(r.u32())]
            rows = [lib.get(i) for i in ids if lib and lib.get(i)]
        elif method == self.GET_CONTENT:
            row = lib.get(r.u64()) if lib else None
            if row is None:                    # not a library track (e.g. one only on the old server)
                return rmc_error(self.PROTO, call_id, self.QRESULT_UGC_UNAVAILABLE)
            w = Writer()
            self._write_content(w, row, False)
            return rmc_response(self.PROTO, call_id, method, w.bytes())
        else:                                  # GET_METADATA_BLOB
            content_id = r.u64()
            blob = lib.blob(content_id, "metablob") if lib else None
            if blob is None:
                return rmc_error(self.PROTO, call_id, self.QRESULT_UGC_UNAVAILABLE)
            self.log(f"[ugc] metadata blob {content_id}")
            return rmc_response(self.PROTO, call_id, method, Writer().buffer(blob).bytes())
        w = Writer().u32(len(rows))
        for row in rows:
            self._write_content(w, row, True)
        return rmc_response(self.PROTO, call_id, method, w.bytes())

    def handle(self, conn, call_id, method, params):
        r = Reader(params)
        if method in (self.GET_CONTENT, self.GET_METADATA_BLOB, self.GET_WITH_BLOBS, self.QUERY_WITH_BLOBS):
            try:
                return self._library_reply(call_id, method, r)
            except (struct.error, ValueError, IndexError) as e:
                # A request layout not seen before: log it and answer as "unavailable", which the
                # game handles (a guessed reply body can corrupt the client heap).
                self.log(f"[ugc] method {method} request not understood ({e}) params={params.hex()}")
                return rmc_error(self.PROTO, call_id, self.QRESULT_UGC_UNAVAILABLE)
        if method == self.WRITE_SCORE:
            track_id, score, descriptor = r.u64(), r.u64(), r.u64()
            self.db.execute("INSERT INTO ugc_runs (pid, track_id, score, descriptor, received) "
                            "VALUES (?, ?, ?, ?, ?)",
                            (conn.pid, self._s64(track_id), self._s64(score), self._s64(descriptor), time.time()))
            self.db.commit()
            self.log(f"[ugc] score pid={conn.pid} track={track_id} score={score} descriptor={descriptor:#x}")
            return rmc_response(self.PROTO, call_id, method, b"")

        if method == self.READ_BY_RANK:
            track_id, start, count = r.u64(), r.u32(), r.u32()
            board = self._board(track_id)
            first = max(start, 1) - 1
            picks = list(enumerate(board, 1))[first:first + count] if count else list(enumerate(board, 1))[first:]
            if self.max_rows:                                      # TRIALS_MAX_ROWS: diagnostic cap
                picks = picks[:self.max_rows]
            # An empty board must be an empty *outer* list: the client reads the first row of any
            # result it gets without checking (FUN_0092b760 -> checked-iterator abort on empty).
            if not picks:
                self.log(f"[ugc] board track={track_id} start={start} count={count} -> empty")
                return rmc_response(self.PROTO, call_id, method, Writer().u32(0).bytes())
            w = Writer().u32(1).u64(track_id).u32(len(picks))            # one UGCLeaderboardResult
            for rank, (pid, score, desc, when, hist) in picks:
                self._write_stat_set(w, pid, score, desc, when, hist)
                # The rank row's trailing u64 is the score the game *displays* (sending 0 showed
                # 134217727 faults / 4473:55.455, i.e. the all-ones decode of an empty score).
                w.u32(0).u32(rank).u64(score)
            self.log(f"[ugc] board track={track_id} start={start} count={count} -> {len(picks)}/{len(board)}")
            return rmc_response(self.PROTO, call_id, method, w.bytes())

        if method == self.READ_BY_PLAYERS:
            track_id = r.u64()
            pids = [r.u32() for _ in range(r.u32())]
            rows = {pid: (score, desc, when, hist) for pid, score, desc, when, hist in self._board(track_id)
                    if not hist}                                          # players asked for are live accounts
            hits = [p for p in pids if p in rows]
            if not hits:                                                  # same rule as method 11
                return rmc_response(self.PROTO, call_id, method, Writer().u32(0).bytes())
            w = Writer().u32(1).u64(track_id).u32(len(hits))              # one UGCStatboardResult
            for pid in hits:
                self._write_stat_set(w, pid, *rows[pid])
            return rmc_response(self.PROTO, call_id, method, w.bytes())

        if method in self.VOID:
            return rmc_response(self.PROTO, call_id, method, b"")
        if method in self.EMPTY_LIST:
            return rmc_response(self.PROTO, call_id, method, Writer().u32(0).bytes())
        if method in self.NOT_DECODED:
            self.log(f"[ugc] method {method} not decoded yet -> error reply params={params.hex()}")
            return rmc_error(self.PROTO, call_id, self.QRESULT_UGC_UNAVAILABLE)
        return None


# ---- UserStorage (protocol 53): per-player content, e.g. ghost replays -----------

def read_variant(r):
    """Quazal variant: type byte + value (1 int64, 2 double, 3 bool, 4 string, 5 datetime, 6 uint64)."""
    t = r.u8()
    if t == 0:
        return t, None
    if t in (1, 5, 6):
        return t, r.u64()
    if t == 2:
        return t, struct.unpack("<d", r.take(8))[0]
    if t == 3:
        return t, r.u8()
    if t == 4:
        return t, r.string()
    raise ValueError(f"unknown variant type {t}")


def write_variant(w, t, v):
    w.u8(t)
    if t in (1, 5, 6):
        w.u64(v & 0xFFFFFFFFFFFFFFFF)
    elif t == 2:
        w.raw(struct.pack("<d", v))
    elif t == 3:
        w.u8(v)
    elif t == 4:
        w.string(v)


def read_properties(r):
    return [(r.u32(), *read_variant(r)) for _ in range(r.u32())]


def write_properties(w, props):
    w.u32(len(props))
    for pid, t, v in props:
        w.u32(pid)
        write_variant(w, t, v)


class UserStorage:
    """Per-player content: Trials stores ghost replays here (typeID 0x80000001).

    Upload: SaveContentAndGetUploadInfo -> HTTP PUT/POST to /ugc/upload/<token> -> UploadEnd.
    The client builds URLs as protocol + host + path verbatim (JobUserStorageUpload::
    StepProcessUpload), so the protocol field must include "://".
    Download: GetContentURL -> HTTP GET /ugc/content/<token>, or GetContentDB inline.
    HTTP URLs use random per-item tokens, not the sequential ids, so nobody can walk every
    player's uploads or overwrite someone else's pending upload by guessing ids.
    Search: content whose properties match every query parameter (e.g. id 2 owner, id 100 track).
    """
    PROTO = 53
    SEARCH_CONTENTS, SEARCH_WITH_TOTAL, DELETE = 1, 2, 3
    SAVE_AND_GET_UPLOAD_INFO, UPLOAD_END, GET_CONTENT_DB, GET_CONTENT_URL = 6, 7, 8, 9
    GET_META_DATA, GET_OWN_CONTENTS = 11, 21
    BY_PLAYERS, BY_PLAYERS_WITH_TOTAL = 25, 26
    VOID = {12, 13, 16, 17, 18, 19, 20, 24}   # like/unlike, favourites, report, play count, stats, tag
    MAX_SIZE = 8 * 1024 * 1024

    def __init__(self, accounts, log, http_host, library=None):
        self.db, self.log, self.http_host, self.library = accounts.db, log, http_host, library
        self.lock = threading.Lock()
        self.db.execute("CREATE TABLE IF NOT EXISTS user_content (content_id INTEGER PRIMARY KEY, "
                        "type_id INTEGER, pid INTEGER, properties TEXT, data BLOB, created REAL)")
        self.db.execute("CREATE TABLE IF NOT EXISTS user_content_pending (pending_id INTEGER PRIMARY KEY, "
                        "type_id INTEGER, content_id INTEGER, pid INTEGER, properties TEXT, size INTEGER, "
                        "data BLOB, created REAL)")
        self.db.execute("CREATE TABLE IF NOT EXISTS ghost_candidates (content_id INTEGER, pid INTEGER, "
                        "properties TEXT, data BLOB, created REAL)")
        for table in ("user_content", "user_content_pending"):
            if "token" not in [r[1] for r in self.db.execute(f"PRAGMA table_info({table})")]:
                self.db.execute(f"ALTER TABLE {table} ADD COLUMN token TEXT")
        for (cid,) in self.db.execute("SELECT content_id FROM user_content WHERE token IS NULL").fetchall():
            self.db.execute("UPDATE user_content SET token=? WHERE content_id=?", (secrets.token_urlsafe(18), cid))
        self.db.commit()

    def resolve_ghost(self, pid, track, size, personal_best):
        """Called by PlayerStats when a track run arrives. stat 104 of that run is its ghost's size,
        which pairs the run with the candidate uploaded just before it."""
        with self.lock:
            rows = self.db.execute("SELECT rowid, content_id, properties, data FROM ghost_candidates "
                                   "WHERE pid=? ORDER BY created DESC", (pid,)).fetchall()
            for rowid, content_id, props, data in rows:
                if [100, 1, track] in json.loads(props) and len(data) == size:
                    if personal_best:
                        self.db.execute("UPDATE user_content SET properties=?, data=?, created=? "
                                        "WHERE content_id=? AND pid=?", (props, data, time.time(), content_id, pid))
                        self.log(f"[storage] new best ghost pid={pid} track={track} ({size} bytes)")
                    self.db.execute("DELETE FROM ghost_candidates WHERE rowid=?", (rowid,))
                    break
            self.db.execute("DELETE FROM ghost_candidates WHERE created < ?", (time.time() - 600,))
            self.db.commit()

    # -- HTTP side (called from the HTTP server) --
    def http_upload(self, token, body):
        with self.lock:
            row = self.db.execute("SELECT pending_id, size FROM user_content_pending WHERE token=?",
                                  (token,)).fetchone()
            if row is None or len(body) > self.MAX_SIZE:
                return False
            self.db.execute("UPDATE user_content_pending SET data=? WHERE pending_id=?", (body, row[0]))
            self.db.commit()
        self.log(f"[storage] upload pending={row[0]} received {len(body)} bytes (expected {row[1]})")
        return True

    def http_download(self, token):
        if token.startswith("lib-") and self.library:          # Track Central library blob (public)
            try:
                return self.library.blob_by_storage_id(int(token[4:]))
            except ValueError:
                return None
        row = self.db.execute("SELECT data FROM user_content WHERE token=?", (token,)).fetchone()
        return row[0] if row else None

    def _data(self, content_id):
        row = self.db.execute("SELECT data FROM user_content WHERE content_id=?", (content_id,)).fetchone()
        return row[0] if row else None

    # -- RMC side --
    def _content_rows(self, type_id, params, pids=None):
        rows = self.db.execute("SELECT content_id, type_id, pid, properties FROM user_content "
                               "WHERE type_id=? ORDER BY created DESC", (type_id,)).fetchall()
        want = {(pid, v) for pid, _, v in params}
        out = []
        for cid, tid, pid, props_json in rows:
            props = [tuple(p) for p in json.loads(props_json)]
            have = {(p, v) for p, _, v in props}
            if want <= have and (pids is None or pid in pids):
                out.append((cid, tid, pid, props))
        return out

    @staticmethod
    def _write_content(w, cid, tid, pid, props):
        w.u32(tid).u64(cid).u32(pid)        # UserContent { m_key{typeID, contentID}, m_pid, m_properties }
        write_properties(w, props)

    def _library_rows(self, type_id, params):
        """Track / screenshot blobs of Track Central library tracks, found by property 101."""
        rows = []
        for pid, _, value in params:
            track = self.library.get(value) if pid == PROP_BLOB_TRACK else None
            if track:
                rows.append((blob_content_id(value, type_id), type_id, track[1], [(PROP_BLOB_TRACK, 6, value)]))
        return rows

    def _search(self, r, method):
        pids = None
        if method in (self.BY_PLAYERS, self.BY_PLAYERS_WITH_TOTAL):
            pids = {r.u32() for _ in range(r.u32())}
        type_id, query_id, offset, size = r.u32(), r.u32(), r.u32(), r.u32()
        params = read_properties(r)
        if type_id in (TYPE_TRACK_BLOB, TYPE_SHOT_BLOB) and self.library:
            rows = self._library_rows(type_id, params)
        else:
            rows = self._content_rows(type_id, params, pids)
        page = rows[offset:offset + size] if size else rows[offset:]
        w = Writer().u32(len(page))
        for row in page:
            self._write_content(w, *row)
        if method in (self.SEARCH_WITH_TOTAL, self.BY_PLAYERS_WITH_TOTAL):
            w.u32(len(rows))
        self.log(f"[storage] search type={type_id:#x} query={query_id} params={params} -> {len(page)}/{len(rows)}")
        return w.bytes()

    def _library_item(self, method, type_id, content_id):
        if method == self.GET_CONTENT_URL:
            self.log(f"[storage] library blob url track={(content_id - BLOB_ID_BASE) // 4}")
            return Writer().string("http://").string(self.http_host).string(f"/ugc/content/lib-{content_id}").bytes()
        if method == self.GET_CONTENT_DB:
            return Writer().buffer(self.library.blob_by_storage_id(content_id) or b"").bytes()
        track_id = (content_id - BLOB_ID_BASE) // 4                     # GET_META_DATA
        track = self.library.get(track_id) if self.library else None
        w = Writer()
        self._write_content(w, content_id, type_id, track[1] if track else 0, [(PROP_BLOB_TRACK, 6, track_id)])
        return w.bytes()

    def handle(self, conn, call_id, method, params):
        r = Reader(params)
        if method in (self.SEARCH_CONTENTS, self.SEARCH_WITH_TOTAL, self.BY_PLAYERS, self.BY_PLAYERS_WITH_TOTAL):
            return rmc_response(self.PROTO, call_id, method, self._search(r, method))

        if method == self.SAVE_AND_GET_UPLOAD_INFO:
            props = read_properties(r)
            size = r.u32()
            type_id, content_id = r.u32(), r.u64()
            token = secrets.token_urlsafe(18)
            with self.lock:
                cur = self.db.execute("INSERT INTO user_content_pending (type_id, content_id, pid, properties, "
                                      "size, created, token) VALUES (?, ?, ?, ?, ?, ?, ?)",
                                      (type_id, content_id, conn.pid, json.dumps(props), size, time.time(), token))
                self.db.commit()
                pending_id = cur.lastrowid
            self.log(f"[storage] upload slot pending={pending_id} pid={conn.pid} type={type_id:#x} "
                     f"size={size} props={props}")
            w = (Writer().string("http://").string(self.http_host).string(f"/ugc/upload/{token}")
                 .u64(pending_id).u32(0))                       # UserContentURL, pendingID, headers
            return rmc_response(self.PROTO, call_id, method, w.bytes())

        if method == self.UPLOAD_END:
            pending_id, ok = r.u64(), r.u8()
            with self.lock:
                row = self.db.execute("SELECT type_id, content_id, pid, properties, data FROM "
                                      "user_content_pending WHERE pending_id=? AND pid=?",
                                      (pending_id, conn.pid)).fetchone()
                if row is None:
                    return rmc_error(self.PROTO, call_id, QRESULT_NOT_IMPLEMENTED)
                type_id, content_id, pid, props, data = row
                if ok and data is not None:
                    if content_id:
                        # The game re-uploads its ghost after every finish, slower runs included.
                        # Hold it as a candidate; PlayerStats promotes it only for a personal best.
                        self.db.execute("INSERT INTO ghost_candidates VALUES (?, ?, ?, ?, ?)",
                                        (content_id, pid, props, data, time.time()))
                    else:
                        cur = self.db.execute("INSERT INTO user_content (type_id, pid, properties, data, created, "
                                              "token) VALUES (?, ?, ?, ?, ?, ?)",
                                              (type_id, pid, props, data, time.time(), secrets.token_urlsafe(18)))
                        content_id = cur.lastrowid
                self.db.execute("DELETE FROM user_content_pending WHERE pending_id=?", (pending_id,))
                self.db.commit()
            self.log(f"[storage] upload end pending={pending_id} ok={ok} -> content {content_id}")
            return rmc_response(self.PROTO, call_id, method, Writer().u32(type_id).u64(content_id).bytes())

        if method in (self.GET_CONTENT_URL, self.GET_CONTENT_DB, self.GET_META_DATA, self.DELETE):
            type_id, content_id = r.u32(), r.u64()
            if content_id >= BLOB_ID_BASE and method != self.DELETE:
                return rmc_response(self.PROTO, call_id, method, self._library_item(method, type_id, content_id))
            if method == self.GET_CONTENT_URL:
                row = self.db.execute("SELECT token FROM user_content WHERE content_id=?", (content_id,)).fetchone()
                token = row[0] if row else "missing"
                w = Writer().string("http://").string(self.http_host).string(f"/ugc/content/{token}")
            elif method == self.GET_CONTENT_DB:
                w = Writer().buffer(self._data(content_id) or b"")
            elif method == self.GET_META_DATA:
                rows = [x for x in self._content_rows(type_id, []) if x[0] == content_id]
                w = Writer()
                self._write_content(w, *(rows[0] if rows else (content_id, type_id, 0, [])))
            else:
                self.db.execute("DELETE FROM user_content WHERE content_id=? AND pid=?", (content_id, conn.pid))
                self.db.commit()
                w = Writer()
            return rmc_response(self.PROTO, call_id, method, w.bytes())

        if method == self.GET_OWN_CONTENTS:
            type_id = r.u32()
            rows = [x for x in self._content_rows(type_id, []) if x[2] == conn.pid]
            w = Writer().u32(len(rows))
            for row in rows:
                self._write_content(w, *row)
            return rmc_response(self.PROTO, call_id, method, w.bytes())

        if method in self.VOID:
            return rmc_response(self.PROTO, call_id, method, b"")
        return None


# ---- UplayWin (protocol 49): Uplay actions / rewards ------------------------------

class UplayWin:
    """Uplay actions/rewards lists. Ubisoft's reward catalogue is gone, so lists come back empty."""
    PROTO = 49

    # 1 GetActions(start, count, sort, lang, platform, game), 2 GetActionsCompleted(...),
    # 7 (lang, platform, game): all return lists.
    LIST_METHODS = {1, 2, 5, 7, 8}   # 5 GetRewards(...), 8 GetSectionsByKey(key, lang, platform, game)

    def handle(self, conn, call_id, method, params):
        if method in self.LIST_METHODS:
            return rmc_response(self.PROTO, call_id, method, Writer().u32(0).bytes())
        return None


# ---- Privileges (protocol 35) ----------------------------------------------------

class Privileges:
    """GetPrivileges(localeCode) -> std_map<uint32, Privilege{m_ID: uint32, m_description: string}>.

    Trials checks 21 privilege IDs for Uplay reward items ("ActiveItems") and writes the result
    into the save, so an empty map would re-lock items players already own. Until the exact IDs
    are pinned down, grant a range that covers them (kept small enough for one PRUDP packet).
    """
    PROTO = 35
    GRANTED_IDS = range(1, 81)

    def handle(self, conn, call_id, method, params):
        if method == 1:
            w = Writer().u32(len(self.GRANTED_IDS))
            for pid in self.GRANTED_IDS:
                w.u32(pid).u32(pid).string("")     # map key, Privilege.m_ID, m_description
            return rmc_response(self.PROTO, call_id, method, w.bytes())
        return None   # unknown method: deferred by the dispatcher


def local_network_ip():
    """This PC's own network address (no packet is sent), for sessions hosted on the server's PC."""
    try:
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
            s.connect(("192.0.2.1", 9))   # TEST-NET address: only picks the outgoing interface
            return s.getsockname()[0]
    except OSError:
        return None


# ---- MatchMaking (protocol 21): multiplayer game sessions ("gatherings") ------------

class MatchMaking:
    """Quazal Rendez-Vous MatchMaking. Sessions live in memory only (they end with the server).

    A gathering arrives as AnyDataHolder: string type name, u32 total length, u32 data length,
    then the object. Base Gathering = u32 id, u32 owner pid, u32 host pid, u16 min players,
    u16 max players, u32 participation policy, u32 policy argument, u32 flags, u32 state,
    string description; subclasses append their own fields, which are kept and echoed as sent.
    """
    PROTO = 21
    REGISTER, UNREGISTER, UNREGISTER_MANY, UPDATE = 1, 2, 3, 4
    PARTICIPATE, CANCEL_PARTICIPATION, GET_PARTICIPANTS = 11, 12, 13
    GET_PARTICIPANTS_URLS = 16
    FIND_BY_TYPE, FIND_BY_DESCRIPTION, FIND_BY_ID, FIND_BY_SINGLE_ID, FIND_BY_OWNER = 17, 18, 20, 21, 22
    FIND_BY_SQL_QUERY = 25                     # (string where-clause, ResultRange) -> list<any>
    LAUNCH_SESSION, UPDATE_SESSION_URL, GET_SESSION_URL, GET_STATE, SET_STATE = 26, 27, 28, 29, 30
    DELETE_GATHERING, FIND_BY_DESCRIPTION_LIKE = 33, 37
    REGISTER_LOCAL_URL, REGISTER_LOCAL_URLS = 38, 39   # (gid, url) / (gid, list<url>) -> nothing
    UPDATE_SESSION_HOST = 40                            # (gid) -> nothing
    GET_SESSION_URLS = 41

    def __init__(self, registry, log, is_online=None):
        self.registry, self.log = registry, log   # SecureConnection: pid -> public station URLs
        # The game doesn't unregister when a player backs out, quits or crashes: sessions whose
        # owner is no longer connected are dropped before every search.
        self.is_online = is_online or (lambda pid: True)
        self.lock = threading.Lock()
        self.gatherings = {}                      # id -> dict
        self.next_id = 0x1000
        # Development aid (TRIALS_TEST_BOT=1, local testing only): an always-open Quick Match
        # session owned by a bot, so one player can see what the game does when joining.
        if os.environ.get("TRIALS_TEST_BOT") == "1":
            self.gatherings[0xB07] = {
                "type": "Gathering", "id": 0xB07, "owner": self.BOT_PID, "host": self.BOT_PID,
                "min": 1, "max": 4, "policy": 1, "policy_arg": 0, "flags": 0, "state": 0,
                "description": "ModeQuickMatch", "extra": b"", "participants": [self.BOT_PID],
                "session_url": "udp:/address=127.0.0.1;port=3051", "open_at": 0}
            self.log("[mm] test bot session 0xb07 is open")

    BOT_PID = 999
    # A session appears in searches only once its host can admit players: the host registers it,
    # spends ~3 s on setup (public-URL probe), then calls UpdateSessionHost and opens. A player who
    # connects earlier is rejected ("admission denied ... status=closed") and gives up.
    OPEN_DELAY = 1.0

    # -- encoding --
    @staticmethod
    def _read_any(r):
        type_name = r.string()
        r.u32()                                    # total length (data length + 4)
        data = r.take(r.u32())
        g = Reader(data)
        fields = {"type": type_name, "id": g.u32(), "owner": g.u32(), "host": g.u32(),
                  "min": g.u16(), "max": g.u16(), "policy": g.u32(), "policy_arg": g.u32(),
                  "flags": g.u32(), "state": g.u32(), "description": g.string()}
        fields["extra"] = g.rest()                 # subclass fields
        return fields

    @staticmethod
    def _write_any(w, g):
        body = (Writer().u32(g["id"]).u32(g["owner"]).u32(g["host"]).u16(g["min"]).u16(g["max"])
                .u32(g["policy"]).u32(g["policy_arg"]).u32(g["flags"]).u32(g["state"])
                .string(g["description"]).raw(g["extra"]).bytes())
        w.string(g["type"]).u32(len(body) + 4).buffer(body)

    def _reply(self, call_id, method, w):
        return rmc_response(self.PROTO, call_id, method, w.bytes())

    def _write_list(self, w, gatherings):
        w.u32(len(gatherings))
        for g in gatherings:
            self._write_any(w, g)
        return w

    def _urls_of(self, g):
        """Where joining players connect (FinishedFindSession at 0x8e06a0 takes address and port of
        the first URL, adds channel 1). Prefer what the host registered for the session; otherwise
        the outside address of the host's game socket, seen in its NAT echo packets."""
        registered = [u for u in g.get("local_urls", []) if "address=" in u]   # not "prudp:/RVCID=n"
        if registered:
            return registered
        if g.get("session_url"):
            return [g["session_url"]]
        entry = self.registry.registry.get(g["host"])
        host_ip = None
        if entry and entry["urls"]:
            m = re.search(r"address=([^;]+)", entry["urls"][0])
            host_ip = m.group(1) if m else None
        if host_ip and not g.get("game_endpoint"):
            # The host's game sends its NAT probes right after registering the session; the first
            # one from its IP after that is its game socket (two players behind one IP stay apart).
            created = g.get("created", 0)
            for ip, port, when in list(NAT_SEEN):
                if ip == host_ip and created - 0.2 <= when <= created + 30:
                    g["game_endpoint"] = (ip, port)
                    break
        if g.get("game_endpoint"):
            ip, port = g["game_endpoint"]
            if ip.startswith("127."):
                # Local testing: the game refuses to connect to 127.0.0.1 (connectToHost 0x8f6f70
                # treats it as "no host"), so name this PC by its network address instead.
                ip = local_network_ip() or ip
            return [f"prudp:/address={ip};port={port}"]
        return list(entry["urls"]) if entry else []

    # FindBySQLQuery: the game sends a WHERE clause such as
    #   WHERE type = 'Gathering' AND description = 'ModeQuickMatch' AND state = 0 ORDER BY id DESC
    # Supported: field = / <> / < / <= / > / >= value, joined by AND, plus ORDER BY field [ASC|DESC].
    SQL_FIELDS = {"type": "type", "description": "description", "state": "state", "id": "id",
                  "owner": "owner", "ownerpid": "owner", "host": "host", "hostpid": "host", "flags": "flags",
                  "minparticipants": "min", "maxparticipants": "max",
                  "participationpolicy": "policy", "policyargument": "policy_arg"}
    SQL_CONDITION = re.compile(r"^\s*(\w+)\s*(=|<>|!=|<=|>=|<|>)\s*('(?:[^']|'')*'|-?\d+)\s*$")
    SQL_OPS = {"=": lambda a, b: a == b, "<>": lambda a, b: a != b, "!=": lambda a, b: a != b,
               "<": lambda a, b: a < b, "<=": lambda a, b: a <= b, ">": lambda a, b: a > b,
               ">=": lambda a, b: a >= b}

    def _sql_find(self, query):
        text = query.strip()
        order_field, descending = "id", False
        m = re.search(r"\s+ORDER\s+BY\s+(\w+)(?:\s+(ASC|DESC))?\s*$", text, re.I)
        if m:
            order_field = self.SQL_FIELDS.get(m.group(1).lower(), "id")
            descending = (m.group(2) or "").upper() == "DESC"
            text = text[:m.start()]
        text = re.sub(r"^\s*WHERE\s+", "", text, flags=re.I)
        conditions = []
        for part in re.split(r"\s+AND\s+", text, flags=re.I) if text.strip() else []:
            c = self.SQL_CONDITION.match(part)
            field = self.SQL_FIELDS.get(c.group(1).lower()) if c else None
            if not field:
                self.log(f"[mm] sql condition not understood, query ignored: {part!r}")
                return []
            value = c.group(3)
            value = value[1:-1].replace("''", "'") if value.startswith("'") else int(value)
            conditions.append((field, self.SQL_OPS[c.group(2)], value))
        own = any(f == "owner" for f, _, _ in conditions)   # "my sessions" queries see unopened ones
        now = time.time()
        found = [g for g in self.gatherings.values()
                 if (own or g.get("open_at", now + 1) <= now)
                 and all(type(g[f]) is type(v) and op(g[f], v) for f, op, v in conditions)]
        return sorted(found, key=lambda g: g[order_field], reverse=descending)

    def _sweep(self):
        for gid in [gid for gid, g in self.gatherings.items()
                    if g["owner"] != self.BOT_PID and not self.is_online(g["owner"])]:
            self._drop(gid, f"owner pid {self.gatherings[gid]['owner']} went offline")

    def _drop(self, gid, why):
        g = self.gatherings.pop(gid, None)
        if g:
            self.log(f"[mm] gathering {gid:#x} removed ({why})")
        return g is not None

    # -- dispatch --
    def handle(self, conn, call_id, method, params):
        r = Reader(params)
        pid = conn.pid
        with self.lock:
            self._sweep()
            if method == self.REGISTER:
                g = self._read_any(r)
                gid, self.next_id = self.next_id, self.next_id + 1
                g.update(id=gid, owner=pid, host=pid, participants=[], session_url="", created=time.time())
                self.gatherings[gid] = g
                self.log(f"[mm] register {g['type']} {gid:#x} by pid {pid}: '{g['description']}' "
                         f"{g['min']}-{g['max']} players, policy {g['policy']}/{g['policy_arg']} "
                         f"flags {g['flags']:#x} extra={g['extra'].hex()}")
                return self._reply(call_id, method, Writer().u32(gid))
            if method == self.UNREGISTER or method == self.DELETE_GATHERING:
                return self._reply(call_id, method, Writer().u8(int(self._drop(r.u32(), f"by pid {pid}"))))
            if method == self.UNREGISTER_MANY:
                ids = [r.u32() for _ in range(r.u32())]
                for gid in ids:
                    self._drop(gid, f"by pid {pid}")
                return self._reply(call_id, method, Writer().u8(1))
            if method == self.UPDATE:
                new = self._read_any(r)
                g = self.gatherings.get(new["id"])
                if g:
                    for k in ("min", "max", "policy", "policy_arg", "flags", "state", "description", "extra"):
                        g[k] = new[k]
                    self.log(f"[mm] update {new['id']:#x}: state {g['state']} flags {g['flags']:#x} extra={g['extra'].hex()}")
                return self._reply(call_id, method, Writer().u8(int(g is not None)))
            if method in (self.PARTICIPATE, self.CANCEL_PARTICIPATION):
                gid, _message = r.u32(), r.string()
                g = self.gatherings.get(gid)
                if g:
                    if method == self.PARTICIPATE and pid not in g["participants"]:
                        g["participants"].append(pid)
                    elif method == self.CANCEL_PARTICIPATION and pid in g["participants"]:
                        g["participants"].remove(pid)
                    self.log(f"[mm] {'join' if method == self.PARTICIPATE else 'leave'} {gid:#x} pid {pid}: {g['participants']}")
                return self._reply(call_id, method, Writer().u8(int(g is not None)))
            if method == self.GET_PARTICIPANTS:
                g = self.gatherings.get(r.u32())
                parts = g["participants"] if g else []
                w = Writer().u32(len(parts))
                for p in parts:
                    w.u32(p)
                return self._reply(call_id, method, w)
            if method == self.GET_PARTICIPANTS_URLS:
                g = self.gatherings.get(r.u32())
                urls = []
                for p in (g["participants"] if g else []):
                    entry = self.registry.registry.get(p)
                    urls += entry["urls"] if entry else []
                w = Writer().u32(len(urls))
                for u in urls:
                    w.string(u)
                return self._reply(call_id, method, w)
            if method in (self.FIND_BY_TYPE, self.FIND_BY_DESCRIPTION, self.FIND_BY_DESCRIPTION_LIKE):
                key = r.string()
                offset, size = r.u32(), r.u32()
                if method == self.FIND_BY_TYPE:
                    found = [g for g in self.gatherings.values() if g["type"] == key]
                elif method == self.FIND_BY_DESCRIPTION:
                    found = [g for g in self.gatherings.values() if g["description"] == key]
                else:                                   # SQL LIKE: % and _ wildcards
                    pattern = "^" + re.escape(key).replace("%", ".*").replace("_", ".") + "$"
                    found = [g for g in self.gatherings.values() if re.match(pattern, g["description"])]
                found = found[offset:offset + size] if size != 0xFFFFFFFF else found[offset:]
                self.log(f"[mm] find method {method} '{key}' by pid {pid} -> {len(found)}")
                return self._reply(call_id, method, self._write_list(Writer(), found))
            if method == self.FIND_BY_SQL_QUERY:
                query = r.string()
                offset, size = r.u32(), r.u32()
                found = self._sql_find(query)
                found = found[offset:offset + size]
                self.log(f"[mm] sql find by pid {pid}: {query!r} -> {len(found)}")
                return self._reply(call_id, method, self._write_list(Writer(), found))
            if method == self.FIND_BY_ID:
                ids = [r.u32() for _ in range(r.u32())]
                return self._reply(call_id, method, self._write_list(
                    Writer(), [self.gatherings[i] for i in ids if i in self.gatherings]))
            if method == self.FIND_BY_SINGLE_ID:
                g = self.gatherings.get(r.u32())
                w = Writer().u8(int(g is not None))
                if g:
                    self._write_any(w, g)
                else:
                    w.string("Gathering").u32(4).u32(0)   # empty holder
                return self._reply(call_id, method, w)
            if method == self.FIND_BY_OWNER:
                owner, offset, size = r.u32(), r.u32(), r.u32()
                found = [g for g in self.gatherings.values() if g["owner"] == owner]
                return self._reply(call_id, method, self._write_list(Writer(), found[offset:offset + size]))
            if method in (self.LAUNCH_SESSION, self.UPDATE_SESSION_URL):
                gid, url = r.u32(), r.string()
                g = self.gatherings.get(gid)
                if g:
                    # Same rule as SecureConnection: share the address we see, not the LAN one.
                    g["session_url"] = public_station_url(url, conn.addr) if "address=" in url else url
                    self.log(f"[mm] session url {gid:#x} by pid {pid}: {g['session_url']}")
                return self._reply(call_id, method, Writer().u8(int(g is not None)))
            if method == self.GET_SESSION_URL:
                g = self.gatherings.get(r.u32())
                urls = self._urls_of(g) if g else []
                return self._reply(call_id, method, Writer().u8(int(bool(urls))).string(urls[0] if urls else ""))
            if method == self.GET_SESSION_URLS:
                g = self.gatherings.get(r.u32())
                urls = self._urls_of(g) if g else []
                w = Writer().u32(len(urls))
                for u in urls:
                    w.string(u)
                return self._reply(call_id, method, w)
            if method in (self.REGISTER_LOCAL_URL, self.REGISTER_LOCAL_URLS):
                gid = r.u32()
                urls = [r.string()] if method == self.REGISTER_LOCAL_URL else [r.string() for _ in range(r.u32())]
                g = self.gatherings.get(gid)
                if g:
                    g["local_urls"] = urls   # the host's own view; other players get the public one
                self.log(f"[mm] local urls {gid:#x} by pid {pid}: {len(urls)}")
                return self._reply(call_id, method, Writer())
            if method == self.UPDATE_SESSION_HOST:
                g = self.gatherings.get(r.u32())
                if g:
                    g["host"] = pid
                    # The host's last setup step; it opens the session right after this.
                    g["open_at"] = time.time() + self.OPEN_DELAY
                    self.log(f"[mm] session host {g['id']:#x} -> pid {pid}")
                return self._reply(call_id, method, Writer())
            if method == self.GET_STATE:
                g = self.gatherings.get(r.u32())
                return self._reply(call_id, method, Writer().u8(int(g is not None)).u32(g["state"] if g else 0))
            if method == self.SET_STATE:
                gid, state = r.u32(), r.u32()
                g = self.gatherings.get(gid)
                if g:
                    g["state"] = state
                    self.log(f"[mm] state {gid:#x} -> {state}")
                return self._reply(call_id, method, Writer().u8(int(g is not None)))
        self.log(f"[mm] method {method} not handled yet params={params.hex()}")
        return None   # deferred by the dispatcher


# ---- Secure server connect + SecureConnection (protocol 11) --------------------

def secure_on_connect(log):
    def on_connect(conn, payload):
        try:
            return _on_connect(conn, payload)
        except Exception as e:
            log(f"[secure] CONNECT payload not understood ({e}): len={len(payload)} {payload.hex()}")
            raise

    def _on_connect(conn, payload):
        r = Reader(payload)
        ticket = r.buffer()
        proof = r.buffer()
        session_key, pid = open_ticket(ticket)
        plain = kerberos_decrypt(session_key, proof)
        proof_pid, cid, check = struct.unpack_from("<III", plain)
        conn.pid, conn.session_key = pid, session_key
        log(f"[secure] ticket ok: pid={pid} proof_pid={proof_pid} cid={cid} check={check:08x}")
        # Matches the SDK's own server-side handler (FUN_00bac710): a plaintext buffer holding
        # check+1. An empty reply crashes the client; a Kerberos-wrapped one is rejected.
        return Writer().buffer(struct.pack("<I", (check + 1) & 0xFFFFFFFF)).bytes()
    return on_connect


def public_station_url(url, addr):
    """The client registers one station URL per local interface (home LAN, VPN, virtual adapters).
    Other players are only ever given the public address we actually see it connect from."""
    scheme, _, params = url.partition(":/")
    rest = [p for p in params.split(";") if p and not p.startswith(("address=", "port="))]
    return f"{scheme}:/" + ";".join([f"address={addr[0]}", f"port={addr[1]}"] + rest)


class SecureConnection:
    """Methods: Register(1), RequestConnectionData(2), RequestURLs(3), RegisterEx(4)."""
    PROTO = 11

    def __init__(self, public_url, log, path="secure_registry.json"):
        self.public_url, self.log, self.path = public_url, log, path
        self.lock = threading.Lock()
        try:
            with open(path, encoding="utf-8") as f:
                self.registry = {int(k): v for k, v in json.load(f).items()}
        except FileNotFoundError:
            self.registry = {}   # pid -> {"cid": int, "urls": [public stationurl]}
        # A registry from before LAN addresses were stripped holds every local address; drop those
        # URLs. Each player's entry is rebuilt with their public address at their next login.
        stale = [e for e in self.registry.values() if len(e["urls"]) > 1]
        for entry in stale:
            entry["urls"] = []
        if stale:
            self._save()

    def _save(self):
        tmp = self.path + ".tmp"
        with open(tmp, "w", encoding="utf-8") as f:
            json.dump(self.registry, f)
        os.replace(tmp, self.path)

    def register_body(self, conn, r):
        urls = [r.string() for _ in range(r.u32())]
        with self.lock:
            cid = max((e["cid"] for e in self.registry.values()), default=1000) + 1
            shared = [public_station_url(urls[0], conn.addr)] if urls else []
            self.registry[conn.pid] = {"cid": cid, "urls": shared}
            self._save()
        self.log(f"[secure] Register pid={conn.pid} cid={cid} ({len(urls)} local urls, sharing public address only)")
        public = urls[0] if urls else self.public_url
        return Writer().u32(QRESULT_SUCCESS).u32(cid).string(public).bytes()

    def handle(self, conn, call_id, method, params):
        r = Reader(params)
        if method in (1, 4):   # Register(vecMyURLs) / RegisterEx(vecMyURLs, hCustomData)
            # RegisterEx's hCustomData repeats the player's Ubisoft credentials: never log it.
            body = self.register_body(conn, r)
            return rmc_response(self.PROTO, call_id, method, body)
        if method in (2, 3):   # RequestConnectionData / RequestURLs(cidTarget, pidTarget)
            cid_target, pid_target = r.u32(), r.u32()
            entry = self.registry.get(pid_target)
            if entry is None and cid_target:
                entry = next((e for e in self.registry.values() if e["cid"] == cid_target), None)
            urls = entry["urls"] if entry else []
            w = Writer().u8(1 if entry else 0).u32(len(urls))
            for url in urls:
                w.string(url)
                if method == 2:          # ConnectionData { stationurl, uint32 connectionID }
                    w.u32(entry["cid"])
            return rmc_response(self.PROTO, call_id, method, w.bytes())
        return None   # unknown method: deferred by the dispatcher
