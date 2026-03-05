#!/bin/bash
#
# myshot.sh - Wayland screenshot tool
#
# Wraps myfreeze + slurp + grim + wl-copy into a clean screenshot workflow.
# myfreeze handles the screen capture and freeze internally using
# wlr-screencopy (no temp PNG file needed), so this script only needs
# to drive the freeze lifecycle and run slurp/grim for the final capture.
#
# Usage:
#   myshot [-a] [-c]
#
# Flags:
#   -a   Area mode: freeze screen, select a region with slurp, capture it
#   -c   Clipboard only: don't save to disk, just copy to clipboard
#
# Examples:
#   myshot          # fullscreen screenshot, save + copy to clipboard
#   myshot -c       # fullscreen, clipboard only
#   myshot -a       # area select, save + copy to clipboard
#   myshot -a -c    # area select, clipboard only
#
# Dependencies:
#   myfreeze   - screen freeze overlay (built from myfreeze.c)
#   grim       - Wayland screenshot capture (area/fullscreen)
#   slurp      - Wayland interactive region selector
#   wl-copy    - clipboard copy (wl-clipboard package)
#   notify-send- desktop notifications (libnotify)
#
set -e

AREA=0
CLIPBOARD=0

# Parse flags
for arg in "$@"; do
    case "$arg" in
        -a) AREA=1 ;;
        -c) CLIPBOARD=1 ;;
    esac
done

# Output filename: ~/Pictures/Shot_YYYYMMDD_HHMMSS.png
TIMESTAMP=$(date +%Y%m%d_%H%M%S)
OUTFILE="$HOME/Pictures/Shot_$TIMESTAMP.png"

if [ "$AREA" -eq 1 ]; then
    # Launch myfreeze as a coprocess.
    # myfreeze captures the screen itself via wlr-screencopy, displays it
    # as a frozen overlay, then blocks on stdin waiting for us to close the pipe.
    #   FREEZE[0] = read end  (we read the PID from here)
    #   FREEZE[1] = write end (we close this to signal myfreeze to unfreeze)
    coproc FREEZE { myfreeze; }

    # myfreeze prints its PID only after the frozen frame is confirmed
    # on screen via a wl_surface.frame callback. Blocking here ensures
    # slurp always sees the frozen screen, never the live desktop.
    read -r FREEZE_PID <&"${FREEZE[0]}"

    # Run slurp on top of the frozen overlay to select a region.
    # slurp outputs "X,Y WxH" (grim -g format). Escape cancels and
    # returns an empty string.
    SELECTION=$(slurp -b '#282828aa' -c '#ebdbb2ff')

    # Closing the write end of the coproc pipe causes myfreeze's stdin
    # to hit EOF. myfreeze then destroys the overlay and exits cleanly.
    exec {FREEZE[1]}>&-

    # Wait for myfreeze to fully exit. The compositor redraws the real
    # desktop during myfreeze's teardown, so grim always captures live
    # content and never the frozen overlay.
    wait "$FREEZE_PID" 2>/dev/null || true

    # Exit silently if user pressed Escape in slurp
    [ -z "$SELECTION" ] && exit 0

    if [ "$CLIPBOARD" -eq 1 ]; then
        # Pipe grim output directly to wl-copy (no file written to disk)
        grim -g "$SELECTION" - | wl-copy
        notify-send 'myshot' 'Area copied to clipboard'
    else
        grim -g "$SELECTION" "$OUTFILE"
        wl-copy < "$OUTFILE"
        notify-send 'myshot' "Area saved to $OUTFILE"
    fi
else
    # Fullscreen - no freeze needed, capture directly
    if [ "$CLIPBOARD" -eq 1 ]; then
        grim - | wl-copy
        notify-send 'myshot' 'Fullscreen copied to clipboard'
    else
        grim "$OUTFILE"
        wl-copy < "$OUTFILE"
        notify-send 'myshot' "Fullscreen saved to $OUTFILE"
    fi
fi

