#!/usr/bin/env python3
"""Generate the GitHub wiki node list from NODE_REGISTRY.md.

    git clone https://github.com/Niik-l/Natron.wiki.git /tmp/natron-wiki
    python tools/gen_wiki_nodes.py /tmp/natron-wiki
    cd /tmp/natron-wiki && git add -A && git commit -m "Node list: regenerate" && git push

One wiki page per registry section (a table: node / status / one-line
description cut from the registry text) plus Home. Run it whenever a node
row changes; the registry stays the source of truth.
"""
import re, os, sys
HERE = os.path.dirname(os.path.abspath(__file__))
REG = os.path.join(HERE, "..", "NODE_REGISTRY.md")
WIKI = sys.argv[1] if len(sys.argv) > 1 else "/tmp/natron-wiki"
if not os.path.isdir(WIKI):
    sys.exit("wiki clone not found: %s (clone https://github.com/Niik-l/Natron.wiki.git there first)" % WIKI)
REG_URL = "https://github.com/Niik-l/Natron/blob/RB-2.6/NODE_REGISTRY.md"

src = open(REG, encoding="utf-8").read().replace("\r\n", "\n")

# ---- parse sections + rows ----
sections = []  # (title, [rows])
cur = None
for line in src.split("\n"):
    m = re.match(r"^## (.+)$", line)
    if m:
        title = m.group(1).strip()
        if title.lower().startswith("summary"):
            cur = None; continue
        cur = (title, []); sections.append(cur); continue
    if cur and line.startswith("| **"):
        cells = [c.strip() for c in line.strip().strip("|").split(" | ")]
        if len(cells) < 4: continue
        name = cells[0].strip("*")
        pid = cells[1].strip("`")
        status = cells[2]
        desc = " | ".join(cells[3:]).rstrip("|").strip()
        cur[1].append((name, pid, status, desc))

def anchor(title):
    a = title.lower()
    a = re.sub(r"[^\w\s-]", "", a)
    return re.sub(r"\s+", "-", a.strip())

OVERRIDES = {"Deep Compositing — Tier 1+2": "Deep-Compositing",
             "Deep Compositing — Tier 3": "Deep-Compositing-Unreleased",
             "Matchmove / 3D Reconstruction": "Matchmove"}
LABELS = {"Deep Compositing — Tier 1+2": "Deep Compositing",
          "Deep Compositing — Tier 3": "Deep Compositing (not yet in the build)",
          "Matchmove / 3D Reconstruction": "Matchmove"}
def page_name(title):
    t = re.sub(r"\s*\(.*?\)\s*", " ", title).strip()          # drop "(8 nodes)"
    if t in OVERRIDES: return OVERRIDES[t]
    t = t.replace("—", "-").replace("/", "-").replace("&", "and")
    return re.sub(r"\s+", "-", t.strip(" -"))

def short(desc, limit=260):
    d = re.sub(r"\*\*(.+?)\*\*", r"\1", desc)                 # unbold
    d = re.sub(r"\[([^\]]+)\]\([^)]+\)", r"\1", d)             # unlink
    d = re.sub(r"`([^`]*)`", r"\1", d)
    d = re.sub(r"\s+", " ", d).strip()
    if len(d) <= limit: return d
    cut = d[:limit]
    for sep in (". ", "; ", " — ", ", "):
        i = cut.rfind(sep)
        if i > limit * 0.45:
            return cut[:i + (1 if sep == ". " else 0)].rstrip() + " …"
    return cut.rstrip() + " …"

def status_word(s):
    s2 = re.sub(r"\*\*|\(.*?\)", "", s).strip()
    if "retired" in s.lower(): return "Retired"
    if "not registered" in s.lower(): return "Not in the build"
    if "registered" in s.lower(): return "Available"
    return s2 or "Available"

# ---- pages ----
def write(name, body):
    with open(os.path.join(WIKI, name + ".md"), "w", encoding="utf-8", newline="\n") as f:
        f.write(body)
    print("wrote", name + ".md")

home = ["# Natron (Niik-l fork) — node list",
        "",
        "Every node this fork adds to Natron, grouped as in the node menu. One line per node; the full developer notes for each are in "
        f"[NODE_REGISTRY.md]({REG_URL}) (linked from each table).",
        "",
        "Builds: [Releases](https://github.com/Niik-l/Natron/releases) · Problems: [Issues](https://github.com/Niik-l/Natron/issues) · "
        "Building from source: [BUILDING.md](https://github.com/Niik-l/Natron/blob/RB-2.6/BUILDING.md)",
        "",
        "| Category | Nodes |",
        "|---|---|"]
total = 0
for title, rows in sections:
    pn = page_name(title)
    avail = sum(1 for r in rows if status_word(r[2]) == "Available")
    total += avail
    label = re.sub(r"\s*\(.*?\)\s*", "", title).strip()
    label = LABELS.get(label, label)
    note = ""
    if "tier 3" in title.lower():
        note = " (code present, not offered in the build yet)"
    home.append(f"| [[{label}|{pn}]] | {avail} available{note} |")

    body = [f"# {label}", "",
            f"Full notes: [NODE_REGISTRY.md → {label}]({REG_URL}#{anchor(title)})", "",
            "| Node | Status | What it does |", "|---|---|---|"]
    for name, pid, status, desc in rows:
        body.append(f"| **{name}** | {status_word(status)} | {short(desc)} |")
    body += ["", "_Status: **Available** = in the release builds; **Not in the build** = source exists but the node is not registered; "
             "**Retired** = replaced, kept so old projects load._", ""]
    write(pn, "\n".join(body))

home += ["", f"**{total} nodes available** in the current release builds.", "",
         "_Generated from NODE_REGISTRY.md; updated together with it when nodes change._", ""]
write("Home", "\n".join(home))

# drop the placeholder page
t = os.path.join(WIKI, "test.md")
if os.path.exists(t):
    os.remove(t); print("removed test.md")
