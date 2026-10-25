"""Show a player under a different name on the leaderboards.

The game always logs in with the player's Ubisoft *login* name, which Ubisoft doesn't change when
the player changes their display name. This sets the name shown to other players; the login name
stays the account key, so the player's scores, ghosts and login key are unaffected.

Usage (on the server, from /opt/trials_revive):
    sudo -u trials python3 tools/rename_player.py <login name> <shown name>
    sudo -u trials python3 tools/rename_player.py <login name> --clear
"""
import os
import sqlite3
import sys

db_path = os.path.join(os.path.dirname(__file__), "..", "trials.db")
if len(sys.argv) != 3:
    sys.exit(__doc__)
login, shown = sys.argv[1], sys.argv[2]
db = sqlite3.connect(db_path)
if "display" not in [r[1] for r in db.execute("PRAGMA table_info(accounts)")]:
    db.execute("ALTER TABLE accounts ADD COLUMN display TEXT")
row = db.execute("SELECT pid, name, display FROM accounts WHERE name=? COLLATE NOCASE", (login,)).fetchone()
if row is None:
    sys.exit(f"no account with login name '{login}'")
pid, name, old = row
if shown == "--clear":
    db.execute("UPDATE accounts SET display=NULL WHERE pid=?", (pid,))
    db.commit()
    print(f"'{name}' (pid {pid}) shows as its login name again (was '{old}')")
    sys.exit()
if not (1 <= len(shown) <= 32) or not shown.isprintable() or "@" in shown or shown.startswith("Legacy#"):
    sys.exit("shown name must be 1-32 printable characters, no '@', not starting with 'Legacy#'")
clash = db.execute("SELECT pid, name FROM accounts WHERE pid<>? AND (name=? COLLATE NOCASE OR "
                   "display=? COLLATE NOCASE)", (pid, shown, shown)).fetchone()
if clash:
    sys.exit(f"'{shown}' is already used by pid {clash[0]} ('{clash[1]}')")
db.execute("UPDATE accounts SET display=? WHERE pid=?", (shown, pid))
db.commit()
print(f"'{name}' (pid {pid}) now shows as '{shown}'" + (f" (was '{old}')" if old else ""))
