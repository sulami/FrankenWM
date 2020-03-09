FrankenWM
=========

*"monsterwm's bastard child"* or *"not the wm your desktop needs, but the one
it deserves"*

FrankenWM is a dynamic tiling WM (comparable to dwm or Awesome), featuring the
v-stack, b-stack, grid, fibonacci, dualstack, equal and monocle layouts out of
the box. If you want to, you can add gaps between the windows as well.

It was once based on monsterwm but has undergone many greater changes including
adding pieces from other WMs (hence the name) and porting all sorts of stuff
from Xlib to XCB. Many of the original monsterwm patches have been ported as
well.

All settings must be set at compile time by editing `config.h` and it does not
feature a status bar (but supports leaving preconfigured space for one). I
prepared a few scripts for different bars [here][sb].

  [sb]: https://gist.github.com/sulami/d6a53179d6d7479e0709

Installation
------------

Note: If you are on Arch Linux, you can use the [AUR][aur].

You need xcb and xcb-utils then, copy `config.def.h` as `config.h` and edit to
suit your needs.  Build and install.

    $ cp config.def.h config.h
    $ $EDITOR config.h
    $ make
    # make clean install

The packages in Arch Linux needed for example would be
`libxcb` `xcb-util` `xcb-util-wm` `xcb-util-keysyms`

  [aur]: https://aur.archlinux.org/packages/frankenwm-git/

Configuration
-------------

Configuration is done by editing `config.h` before compiling FrankenWM.

Usage
-----

I took the time to write a really nice and pretty manpage (man frankenwm, or
man ./[frankenwm.1][man] if you want to read it before installing) covering the
tiling modes and all of the default shortcuts.

  [man]: https://github.com/sulami/frankenwm/blob/master/frankenwm.1

Bugs
----

You can report bugs and request features here: [FrankenWM GitHub issues][gh] or
[ArchLinux Forums][af]

   [gh]: https://github.com/sulami/FrankenWM/issues
   [af]: https://bbs.archlinux.org/viewtopic.php?pid=1470320

Thanks
------

Parts of FrankenWM come from:

 * [c00kiemonster's][cookiemonster] [monsterwm][monsterwm]
 * [cloudef's][cloudef] [monsterwm-xcb][monsterwm-xcb]
 * [venam's][venam] [2bwm][twobwm]


Inspiration from:

 * [suckless'][suckless] [dwm][dwm]
 * [baskerville's][baskerville] [bspwm][bspwm]


  [cookiemonster]: https://github.com/c00kiemon5ter
  [monsterwm]: https://github.com/c00kiemon5ter/monsterwm
  [cloudef]: https://github.com/cloudef
  [monsterwm-xcb]: https://github.com/cloudef/monsterwm-xcb
  [venam]: https://github.com/venam
  [twobwm]: https://github.com/venam/2bwm

  [suckless]: http://suckless.org/
  [dwm]:  http://dwm.suckless.org/
  [baskerville]: https://github.com/baskerville
  [bspwm]: https://github.com/baskerville/bspwm

