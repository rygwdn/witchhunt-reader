#!/bin/sh
# The reader's per-book language and hyphenation, end to end, in the native simulator.
#
#   scripts/check_book_language.sh [work-dir]
#
# Build the simulator first (pio run -e simulator). Everything else is here: the fixture book is
# written, the reader is driven through tools/devctl.py, and the outcome is judged from what the
# reader reports, from the pixels it draws, and from the card it writes.
#
# It proves four things:
#
#   1. The language selector reaches the reader. The reader reports the language it lays the book
#      out in (STATE lang=), and the card keeps the choice (recent.json languageOverride).
#   2. The language reaches the layout. The same book laid out in English and in German draws
#      different pages: the German patterns break the fixture's long word where the English
#      patterns do not, so the two pages differ in pixels.
#   3. The hyphenation switch reaches the layout. With the language held still, turning
#      hyphenation off draws a different page.
#   4. A card that cannot be written does not swallow the change. The reader shows the new
#      language anyway, and says in the log that it will not survive a restart.
#
# The reader's settings, caches and progress are written to a disposable SD fixture directory
# (CROSSPOINT_SIM_SD), never to a real card. The work directory keeps the screenshots and the log.
set -eu

here=$(cd "$(dirname "$0")/.." && pwd)
work=${1:-$(mktemp -d "${TMPDIR:-/tmp}/witchhunt-language.XXXXXX")}
simulator="$here/.pio/build/simulator/program"
python=${PYTHON:-python3}
sd="$work/sd"

if [ ! -x "$simulator" ]; then
  echo "build the simulator first: pio run -e simulator" >&2
  exit 2
fi

mkdir -p "$sd/books"
"$python" "$here/scripts/make_language_fixture.py" "$sd/books/language.epub"
# A second book with the same content: the language is per book, so this one must keep its own.
cp "$sd/books/language.epub" "$sd/books/other.epub"

# The steps, as devctl reads them: one per line, '#' for a comment. Every phase reopens the
# reader menu, so the phases do not depend on each other's menu state.
steps=$work/steps.txt
cat > "$steps" <<EOF
# The fixture book: no language in its metadata, so the reader's own setting decides.
cmd:ACT:READER:/books/language.epub
idle:60000:SCREEN
state
# The reader menu: the Settings tab holds the per-book settings.
cmd:BTN:CONFIRM
idle:10000
tab:Settings
# English patterns, and hyphenation on.
set:^Book language\$=English
set:^Hyphenation\$=on
cmd:BTN:BACK
idle:5000
cmd:BTN:BACK
idle:30000
state
check:lang=en
check:hyphen=1
shot:$work/english.png
# The same book in German: the layout must change with the language.
cmd:BTN:CONFIRM
idle:10000
tab:Settings
set:^Book language\$=German
cmd:BTN:BACK
idle:5000
cmd:BTN:BACK
idle:30000
state
check:lang=de
check:hyphen=1
shot:$work/german.png
diff:$work/english.png:$work/german.png:100
# German with hyphenation off: the page must change with the switch.
cmd:BTN:CONFIRM
idle:10000
tab:Settings
set:^Hyphenation\$=off
cmd:BTN:BACK
idle:5000
cmd:BTN:BACK
idle:30000
state
check:lang=de
check:hyphen=0
shot:$work/german-nohyph.png
diff:$work/german.png:$work/german-nohyph.png:100
# The other book: the language is per book, so it must keep its own (none).
cmd:ACT:READER:/books/other.epub
idle:60000:SCREEN
state
check:lang=
check:hyphen=0
EOF

echo "== the language and the hyphenation switch reach the layout =="
CROSSPOINT_SIM_SD="$sd" "$python" "$here/tools/devctl.py" --exec "$simulator" --log "$work/run.log" --script "$steps"

echo "== the card keeps the choice, for that book alone =="
"$python" - "$sd/.crosspoint/recent.json" <<'PY'
import json
import sys

books = json.load(open(sys.argv[1]))["books"]
overrides = {book["path"]: book.get("languageOverride", "") for book in books}
if overrides.get("/books/language.epub") != "de":
    sys.exit(f"the card does not hold the language override for the first book: {overrides}")
if overrides.get("/books/other.epub", "") != "":
    sys.exit(f"the language override leaked to the other book: {overrides}")
print("the card holds the language override for the first book only")
PY

# A card that cannot be written: the book is in the recent list, but the file is read-only.
chmod 444 "$sd/.crosspoint/recent.json"

cat > "$work/steps-failing-save.txt" <<EOF
cmd:ACT:READER:/books/language.epub
idle:60000:SCREEN
cmd:BTN:CONFIRM
idle:10000
tab:Settings
set:^Book language\$=French
cmd:BTN:BACK
idle:5000
cmd:BTN:BACK
idle:30000
state
check:lang=fr
EOF

echo "== a card that cannot be written does not swallow the change =="
CROSSPOINT_SIM_SD="$sd" "$python" "$here/tools/devctl.py" --exec "$simulator" --log "$work/run.log" \
  --script "$work/steps-failing-save.txt"

if ! grep -q "Could not save the book language override" "$work/run.log"; then
  echo "the failed save was not reported in the log" >&2
  exit 1
fi
chmod 644 "$sd/.crosspoint/recent.json" 2>/dev/null || true

echo
echo "ok: the language and the hyphenation switch reach the layout, the card keeps the choice,"
echo "    and a card that cannot be written does not swallow the change"
echo "    screenshots: $work/english.png $work/german.png $work/german-nohyph.png"
echo "    log:        $work/run.log"
