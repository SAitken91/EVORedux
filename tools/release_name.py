"""Release a claimed player name so the next login with it claims it again.

Use when a player lost their trials_revive_login.key (e.g. reinstalled without keeping it).
Their scores and ghosts stay attached to the name.

Usage (on the server, from /opt/trials_revive):
    sudo -u trials python3 tools/release_name.py <player name>
"""
import os
import sqlite3
import sys

db_path = os.path.join(os.path.dirname(__file__), "..", "trials.db")
if len(sys.argv) != 2:
    sys.exit(__doc__)
db = sqlite3.connect(db_path)
cur = db.execute("DELETE FROM account_keys WHERE name=?", (sys.argv[1].lower(),))
db.commit()
print(f"released '{sys.argv[1]}'" if cur.rowcount else f"'{sys.argv[1]}' was not claimed")
