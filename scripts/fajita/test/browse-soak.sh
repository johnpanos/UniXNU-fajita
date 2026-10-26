#!/bin/bash
# browse-soak.sh MINUTES [URL...]: browse heavy pages in Safari on a held session (session.sh start / wifi-test.sh with WIFI_HOLD=1)
# for MINUTES, and watch for crashes. Each round opens the next URL (fjbundle --open), scrolls the page down and back up, then
# prints one status line: new CRASH rows in the session's USB console, the plugin's PF row (problems, refused queues, GPU-failed
# command buffers), its newest problem (P0) and free memory. A crash is counted once however often the console repeats its row. Ends with a summary; exit 1 if backboardd crashed.
# Run it through bg.sh (it runs for minutes):
#   FJB=/usr/sbin/fjbundle-0826 scripts/fajita/test/bg.sh start soak --max 1500 scripts/fajita/test/browse-soak.sh 10
# FJB: the fjbundle pushed to the phone (session.sh push .cache/fajita-inputs/fjbundle /private/var/ngpu/sbin/NAME; run as
# /usr/sbin/NAME). Default URLs: news.ycombinator.com, two wikipedia articles, apple.com, the HN front page's second page.
# SCROLLS (default 6) swipes per page; PAGE_WAIT (default 8) seconds for a page to load.
TD=$(cd "$(dirname "$0")" && pwd); ROOT=$(cd "$TD/../../.." && pwd)
W=${FAJITA_WORK:-$ROOT/.cache/fajita-work}
MIN=${1:?usage: browse-soak.sh MINUTES [URL...]}; shift
FJB=${FJB:?set FJB to the fjbundle path on the phone}
URLS=("$@")
[ ${#URLS[@]} -gt 0 ] || URLS=(https://news.ycombinator.com/ https://en.wikipedia.org/wiki/Linux https://www.apple.com/
                               https://en.wikipedia.org/wiki/World_War_II https://news.ycombinator.com/news?p=2 https://www.apple.com/iphone/)
S="$TD/session.sh"
NAME=$(cat "$W/session.current" 2>/dev/null); CON=$W/logs/$NAME/usb-console.txt
[ -f "$CON" ] || { echo "no held session (session.current=$NAME)"; exit 2; }
KM=${KM:-$(ls -t "$ROOT"/.cache/fajita-out*/unixnu/fajita-ios16/kernel/kernel.macho 2>/dev/null | head -1)}
start=$(date +%s); end=$((start + MIN * 60)); round=0; base=$(grep -ac "CRASH p" "$CON")
echo "browse-soak: session $NAME, $MIN min, ${#URLS[@]} URLs, console $CON (CRASH rows before: $base)"
k() { timeout 20 "$S" call kcmd "$@" >/dev/null 2>&1; }
while [ "$(date +%s)" -lt $end ]; do
  url=${URLS[$((round % ${#URLS[@]}))]}; round=$((round + 1))
  timeout 25 "$S" run "$FJB --open $url" >/dev/null 2>&1 || echo "  open $url: no answer"
  sleep "${PAGE_WAIT:-8}"
  for i in $(seq "${SCROLLS:-6}"); do k touch swipe xy 540 1800 540 700; sleep 1.5; done
  for i in 1 2; do k touch swipe xy 540 700 540 1900; sleep 1.5; done
  crashes=$(grep -a "CRASH p" "$CON" | tail -n +$((base + 1)) | sed "s/^.*CRASH p/CRASH p/" | sort -u)
  bb=$(grep -ac "CRASH p[0-9]* backboardd" <<<"$crashes")
  rows=$(KM=$KM timeout 30 python3 "$TD/rows.py" 2>/dev/null)
  pf=$(grep -m1 '^PF ' <<<"$rows"); p0=$(grep -m1 '^P0 ' <<<"$rows")
  mem=$(grep -a '^MEM ' "$CON" | tail -1 | grep -o 'free4K=[0-9]*')
  printf '[%s] r%d %-40s crashes+%d (backboardd %d) %s %s | %s\n' "$(date +%H:%M:%S)" $round "${url:0:40}" \
    "$(grep -c . <<<"$crashes")" "$bb" "${mem:-free4K=?}" "${pf:-PF ?}" "${p0:0:90}"
  [ -n "$crashes" ] && grep -a "CRASH p" <<<"$crashes" | sort | uniq -c | sed 's/^/    /' | head -5
done
crashes=$(grep -a "CRASH p" "$CON" | tail -n +$((base + 1)) | sed "s/^.*CRASH p/CRASH p/" | sort -u)
bb=$(grep -ac "CRASH p[0-9]* backboardd" <<<"$crashes")
echo "browse-soak: $round pages in $(( ($(date +%s) - start) / 60 )) min; CRASH rows $(grep -c . <<<"$crashes"), backboardd $bb"
[ "$bb" = 0 ]
