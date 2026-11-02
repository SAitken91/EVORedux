"""Track Central library: community tracks served to every player (e.g. the Xbox 360 archive).

Kept in its own database (track_library.db, built by tools/build_track_library.py) so the track
files never sit in trials.db and the library can be rebuilt or shipped separately.

How the game uses a library track (from the client's Track Central tasks):
  - feeds / search: UserGeneratedContent method 14 (UGCQuery -> list<UGCContentWithMetadataBlob>)
  - details: method 4 (u64 id -> UGCContent)
  - download: the metadata blob (method 5), then the track and screenshot from UserStorage:
    content of type 0x80000003 (track) / 0x80000004 (screenshot) whose property 101 is the track id.

UGCContent properties (ids as the client reads them in RDVDownloadContentTask):
  4 datetime uploaded, 8 name, 9 file name, 10 flags (game type bit | difficulty bit | bit 31),
  11-14 four ints from the track's metadata, 15 downloads, 16 rating (stars = (v-1)//20+1),
  17 rating count.
"""
import lzma
import os
import sqlite3
import struct
import threading
import uuid

PROP_DATE, PROP_NAME, PROP_FILE, PROP_FLAGS = 4, 8, 9, 10
PROP_DOWNLOADS, PROP_RATING, PROP_VOTES = 15, 16, 17
PROP_OWNER = 2                       # search filter: creator pid
PROP_BLOB_TRACK = 101                # UserStorage property: Track Central id of a track/screenshot blob
TYPE_TRACK_BLOB, TYPE_SHOT_BLOB = 0x80000003, 0x80000004

GAME_TYPE_BITS = (0x400, 0x200, 0x800, 0x1000, 0x2000)          # 0 Trials, 2 Supercross, 3 Skill game
DIFFICULTY_BITS = (0x4000, 0x8000, 0x10000, 0x20000, 0x40000)  # 0 beginner .. 4 extreme
GAME_TYPE_MASK = sum(GAME_TYPE_BITS)

# A library track's creator pid is the low 32 bits of the creator's Xbox 360 XUID: the game reads
# that pid from the track header of an installed 360 track and looks its name up
# (UbiAccountManagement 12/14), so the server must answer for exactly these. Unanswered, the game
# re-asks forever, its 100-entry gamertag queue stays full and the next lookup crashes the game.
# UserStorage content ids for library blobs, apart from the autoincrement ids of player uploads.
BLOB_ID_BASE = 1 << 48
# Tracks players upload from the game get ids from here up, clear of the 360 archive's ids.
PLAYER_ID_BASE = 10_000_000
SOURCE_PLAYER, SOURCE_PLAYER_PENDING = "player", "player-pending"   # pending: uploaded, not published

# UGCQuery ids the client sends (RDVGetFeedsTask 0x92bd00 maps a feed's sort + game type to one;
# RDVEnumerateContentTask sends 6/7 for searches)
QUERY_NEW_BY_TYPE, QUERY_MOST_DOWNLOADED, QUERY_TOP_RATED_BY_TYPE, QUERY_TOP_RATED = 2, 3, 4, 5
QUERY_SEARCH = (6, 7)

# tms/ugc.xml: the game's built-in feed list (trialsFMX.exe 0x18d92c8), served with the RedLynx
# Picks filled in. The client shim points the game's fallback document at this (ugc_feeds.cpp).
UGC_XML = (
    '<UGC>\n  <killswitch value="false" />  <online_feeds>'
    '   \t<enum enabled="true" name="FEED_NAME_REDLYNX_PICKS" index="0" gfx="redlynx_picks" />'
    '     <feed enabled="true" name="FEED_NAME_MOST_DOWNLOADED_WEEK" index="1" gfx="high_downloaded" time="7" sort="HIGH_DOWNLOAD" gameType="ALL" />'
    '     <feed enabled="true" name="FEED_NAME_HIGHEST_RATED_WEEK" index="2" gfx="high_rated" time="7" sort="HIGH_RATING" gameType="ALL" />'
    '       <feed enabled="true" name="FEED_NAME_HIGHEST_RATED_TRIALS" index="6" gfx="trials" time="0" sort="HIGH_RATING" gameType="TRIALS" />'
    '     <feed enabled="true" name="FEED_NAME_HIGHEST_RATED_SKILLGAME" index="7" gfx="skillgame" time="0" sort="HIGH_RATING" gameType="SKILLGAME" />'
    '     <feed enabled="true" name="FEED_NAME_HIGHEST_RATED_SUPERCROSS" index="8" gfx="supercross" time="0" sort="HIGH_RATING" gameType="SUPERCROSS" />'
    '     <feed enabled="true" name="FEED_NAME_NEW_TRIALS" index="3" gfx="trials" time="0" sort="DATE" gameType="TRIALS" />'
    '     <feed enabled="true" name="FEED_NAME_NEW_SKILLGAME" index="4" gfx="skillgame" time="0" sort="DATE" gameType="SKILLGAME" />'
    '      <feed enabled="true" name="FEED_NAME_NEW_SUPERCROSS" index="5" gfx="supercross" time="0" sort="DATE" gameType="SUPERCROSS" />'
    '   </online_feeds>   <FEED_NAME_REDLYNX_PICKS>{picks}</FEED_NAME_REDLYNX_PICKS> </UGC>')
PICKS = 20


def flags_for(game_type, difficulty, flag31=0):
    v = GAME_TYPE_BITS[game_type] if game_type < 5 else 0
    v |= DIFFICULTY_BITS[difficulty] if difficulty < 5 else 0
    return v | (0x80000000 if flag31 else 0)


DLC_LABELS = {1: "OoP", 2: "RoD", 3: "OoP+RoD"}       # bit 1 Origin of Pain, bit 2 Riders of Doom


def track_dlc(trk):
    """DLC packs a track needs (bit 1 Origin of Pain, bit 2 Riders of Doom), from its DLC0 chunk:
    u32 pack bits of the DLC objects it uses, then the world it was built in (1 Paine Island,
    2 Riders of Doom). PC and 360 track.trk: magic, header, LZMA props at 37, stream at 46."""
    if not trk or len(trk) < 64 or trk[:4] not in (bytes.fromhex("deadbabe"), bytes.fromhex("cafeb00b")):
        return 0
    try:
        p = trk[37]
        dec = lzma.LZMADecompressor(lzma.FORMAT_RAW, filters=[{
            "id": lzma.FILTER_LZMA1, "lc": p % 9, "lp": p // 9 % 5, "pb": p // 45,
            "dict_size": struct.unpack_from("<I", trk, 38)[0]}])
        body = dec.decompress(trk[46:], max_length=4096)
    except (lzma.LZMAError, ValueError):
        return 0
    o = 0
    while o + 8 <= len(body):
        size = struct.unpack_from(">I", body, o + 4)[0]
        if body[o:o + 4] == b"DLC0":
            if size < 5 or o + 13 > len(body):
                return 0
            world = body[o + 12]
            return (struct.unpack_from(">I", body, o + 8)[0] | (1 << (world - 1) if world in (1, 2) else 0)) & 3
        o += 8 + size
    return 0


def blob_content_id(track_id, kind):
    return BLOB_ID_BASE + track_id * 4 + (1 if kind == TYPE_TRACK_BLOB else 2)


class TrackLibrary:
    COLUMNS = ("content_id INTEGER PRIMARY KEY, owner_pid INTEGER, creator TEXT, name TEXT, folder TEXT, "
               "flags INTEGER, p11 INTEGER, p12 INTEGER, p13 INTEGER, p14 INTEGER, uploaded INTEGER, "
               "downloads INTEGER, rating INTEGER, votes INTEGER, source TEXT, metablob BLOB, "
               "track BLOB, shot BLOB")

    def __init__(self, path="track_library.db"):
        self.path = path
        self.db = sqlite3.connect(path, check_same_thread=False)
        self.lock = threading.Lock()
        self.db.execute(f"CREATE TABLE IF NOT EXISTS tracks ({self.COLUMNS})")
        self.db.execute("CREATE TABLE IF NOT EXISTS creators (pid INTEGER PRIMARY KEY, name TEXT)")
        if "dlc" not in [r[1] for r in self.db.execute("PRAGMA table_info(tracks)")]:
            self.db.execute("ALTER TABLE tracks ADD COLUMN dlc INTEGER")
        # DLC packs each track needs, read once from its track file (shown as a label on its name).
        for cid, trk in self.db.execute("SELECT content_id, track FROM tracks WHERE dlc IS NULL").fetchall():
            self.db.execute("UPDATE tracks SET dlc=? WHERE content_id=?", (track_dlc(trk), cid))
        self.db.commit()
        self.reload()

    def reload(self):
        """Keep the searchable fields in memory (a few thousand rows); blobs stay on disk."""
        with self.lock:
            self.rows = self.db.execute(
                "SELECT content_id, owner_pid, creator, name, folder, flags, p11, p12, p13, p14, uploaded, "
                "downloads, rating, votes, dlc FROM tracks WHERE source IS NOT ?",
                (SOURCE_PLAYER_PENDING,)).fetchall()
            self.by_id = {r[0]: r for r in self.rows}
            self.creators = dict(self.db.execute("SELECT pid, name FROM creators").fetchall())
            self.creator_uids = {self.creator_account_id(pid): pid for pid in self.creators}

    def __len__(self):
        return len(self.rows)

    # -- building --
    def set_creator(self, pid, name):
        self.db.execute("INSERT OR REPLACE INTO creators VALUES (?, ?)", (pid, name))

    def put(self, **t):
        cols = ", ".join(t)
        self.db.execute(f"INSERT OR REPLACE INTO tracks ({cols}) VALUES ({', '.join('?' * len(t))})",
                        tuple(t.values()))

    # -- player uploads (UserGeneratedContent 2 / 3, UserStorage track + screenshot) --
    def owner_of(self, content_id):
        row = self.db.execute("SELECT owner_pid FROM tracks WHERE content_id=?", (content_id,)).fetchone()
        return row[0] if row else None

    def save_player_track(self, content_id, pid, creator, props, metablob, uploaded):
        """Create (content_id 0) or update a player's track; returns its id, or None if the id
        belongs to someone else. It stays hidden from Track Central until published."""
        values = {k: v for k, _, v in props}
        fields = dict(name=str(values.get(PROP_NAME) or "Untitled"), folder=str(values.get(PROP_FILE) or ""),
                      flags=int(values.get(PROP_FLAGS) or 0), metablob=metablob, creator=creator,
                      **{f"p{i}": int(values.get(i, -1)) for i in (11, 12, 13, 14)})
        fields = {k: (v - (1 << 64) if isinstance(v, int) and v >= 1 << 63 else v) for k, v in fields.items()}
        with self.lock:
            if content_id:
                if self.owner_of(content_id) != pid:
                    return None
                sets = ", ".join(f"{k}=?" for k in fields)
                self.db.execute(f"UPDATE tracks SET {sets} WHERE content_id=?", (*fields.values(), content_id))
            else:
                top = self.db.execute("SELECT max(content_id) FROM tracks").fetchone()[0] or 0
                content_id = max(top + 1, PLAYER_ID_BASE)
                self.put(content_id=content_id, owner_pid=pid, uploaded=uploaded, downloads=0, rating=0,
                         votes=0, source=SOURCE_PLAYER_PENDING, **fields)
            self.db.commit()
        return content_id

    def save_player_blob(self, content_id, pid, kind, data):
        column = {TYPE_TRACK_BLOB: "track", TYPE_SHOT_BLOB: "shot"}.get(kind)
        with self.lock:
            if column is None or self.owner_of(content_id) != pid:
                return False
            self.db.execute(f"UPDATE tracks SET {column}=? WHERE content_id=?", (data, content_id))
            if kind == TYPE_TRACK_BLOB:
                self.db.execute("UPDATE tracks SET dlc=? WHERE content_id=?", (track_dlc(data), content_id))
            self.db.commit()
        return True

    def publish(self, content_id, pid):
        """Show a player's track in Track Central once its track file has arrived."""
        with self.lock:
            done = self.db.execute("UPDATE tracks SET source=? WHERE content_id=? AND owner_pid=? "
                                   "AND track IS NOT NULL", (SOURCE_PLAYER, content_id, pid)).rowcount
            self.db.commit()
        if done:
            self.reload()
        return bool(done)

    # -- serving --
    def properties(self, row):
        (cid, owner, creator, name, folder, flags, p11, p12, p13, p14, uploaded,
         downloads, rating, votes, dlc) = row
        if dlc in DLC_LABELS:                 # the PC can't tell players a track needs DLC, so say it
            name = f"{name} [{DLC_LABELS[dlc]}]"
        return [(PROP_DATE, 5, uploaded), (PROP_NAME, 4, name), (PROP_FILE, 4, folder),
                (PROP_FLAGS, 6, flags), (11, 1, p11), (12, 1, p12), (13, 1, p13), (14, 1, p14),
                (PROP_DOWNLOADS, 1, downloads), (PROP_RATING, 1, rating), (PROP_VOTES, 1, votes)]

    def get(self, content_id):
        return self.by_id.get(content_id)

    def blob(self, content_id, column):
        row = self.db.execute(f"SELECT {column} FROM tracks WHERE content_id=?", (content_id,)).fetchone()
        return row[0] if row else None

    def blob_by_storage_id(self, storage_id):
        """UserStorage content id -> (track id, bytes) for a library track/screenshot blob."""
        if storage_id < BLOB_ID_BASE:
            return None
        track_id, kind = divmod(storage_id - BLOB_ID_BASE, 4)
        column = {1: "track", 2: "shot"}.get(kind)
        return self.blob(track_id, column) if column else None

    def query(self, query_id, props, offset, count):
        """UGCQuery -> matching rows, ordered for the feed or search that asked."""
        rows = self.rows
        for pid, _, value in props:
            if pid == PROP_FLAGS and value & GAME_TYPE_MASK:
                rows = [r for r in rows if r[5] & value & GAME_TYPE_MASK]
            elif pid == PROP_NAME and value:
                # Search text matches the track name or the creator's gamertag.
                text = str(value).lower()
                rows = [r for r in rows if text in r[3].lower() or text in (r[2] or "").lower()]
            elif pid == PROP_OWNER and value:
                rows = [r for r in rows if r[1] == value]
            # PROP_DATE (feed time windows such as "this week") is ignored on purpose: the library
            # is an archive, so a window would leave those feeds empty.
        if query_id in (QUERY_TOP_RATED, QUERY_TOP_RATED_BY_TYPE):
            rows = sorted(rows, key=lambda r: (-r[12], -r[13]))
        elif query_id == QUERY_MOST_DOWNLOADED:
            rows = sorted(rows, key=lambda r: -r[11])
        elif query_id == QUERY_NEW_BY_TYPE:
            rows = sorted(rows, key=lambda r: -r[10])
        else:
            rows = sorted(rows, key=lambda r: r[3].lower())
        return rows[offset:offset + count] if count else rows[offset:], len(rows)

    def feeds_document(self):
        """tms/ugc.xml with the most-rated five-star tracks as RedLynx Picks."""
        picks = sorted((r for r in self.rows if r[12] >= 100), key=lambda r: -r[13])[:PICKS]
        return UGC_XML.format(picks="".join(f'<track id="{r[0]}" />' for r in picks))

    # -- creator names (UbiAccountManagement lookups) --
    def is_creator(self, pid):
        return pid in self.creators

    @staticmethod
    def creator_account_id(pid):
        return str(uuid.uuid5(uuid.NAMESPACE_URL, f"trials-revive:creator:{pid}"))

    def creator_by_account_id(self, uid):
        return self.creator_uids.get(uid)


def open_library(path="track_library.db"):
    """The library if it has been built, else None (the server then serves no library tracks)."""
    return TrackLibrary(path) if os.path.exists(path) else None
