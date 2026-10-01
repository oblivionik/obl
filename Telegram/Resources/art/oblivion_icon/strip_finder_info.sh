#!/bin/sh
# Oblivion: the optional Finder app icon (Settings > Oblivion > App icon)
# sets the custom icon flag in the Finder info of the app bundle, and
# codesign refuses to sign a bundle with Finder info. Drop it before signing,
# the app puts the icon back on its next launch.
xattr -d com.apple.FinderInfo "$1" 2>/dev/null
exit 0
