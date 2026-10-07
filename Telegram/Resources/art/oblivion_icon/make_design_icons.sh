#!/bin/sh
# Oblivion: pictures of the app icon designs offered in
# Settings > Oblivion > App icon (see oblivion_app_icon.cpp and
# qrc/telegram/mac_icons.qrc). The first design, "eclipse", is the bundle
# icon itself (design-mockups/src/install_icon.py) and needs no picture.
#
# Usage: make_design_icons.sh <folder with icon2.png .. icon5.png, 1024 px>
#
# 512 px is enough for the Dock tile on a Retina screen and keeps the
# resources small, the app scales the pictures itself.
set -e
from="${1:?the folder with the 1024 px renders (design-mockups/icons)}"
to="$(cd "$(dirname "$0")" && pwd)"
make() {
	sips -z 512 512 "$from/icon$1.png" --out "$to/oblivion_$2_512.png" >/dev/null
	xattr -c "$to/oblivion_$2_512.png"
}
make 2 portal
make 3 ghost
make 4 shadow
make 5 horizon
