# xterm.js

The terminal emulator behind the shell tab of `graph display`. The one
third-party piece of Graph; vendored so the build stays offline and the
version stays pinned.

    xterm.js     @xterm/xterm      5.5.0    lib/xterm.js, css/xterm.css
    addon-fit    @xterm/addon-fit  0.10.0   lib/addon-fit.js

Taken unmodified from the npm tarballs (registry.npmjs.org), except that the
`sourceMappingURL` trailer is dropped since the maps are not shipped. MIT,
see LICENSE. `tools/term.sh` inlines these into `shell/term.html` and embeds
the result in the shells as `shell/term.h`.
