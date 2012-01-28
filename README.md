monsterwm-xcb
=============

→ tiny(!) and monsterous!
----------------------
This is xcb port of monsterwm tiling window manager.
For more detailed README refer to [monsterwm][]

Installation
------------

You need xcb and xcb-utils then,
copy `config.def.h` as `config.h`
and edit to suit your needs.
Build and install.

    $ cp config.def.h config.h
    $ $EDITOR config.h
    $ make
    # make clean install

The packages in Arch Linux needed for example would be
`libxcb` `xcb-util` `xcb-util-wm` `xcb-util-keysym`

Patches
-------

Some extensions to the code are supported in the form of patches.
See other branches for patch and code.

Currently:

 * [fib]            : adds fibonacci layout
 * [monocleborders] : adds borders to the monocle layout
 * [showhide]       : adds a function to show and hide all windows on all desktops
 * [uselessgaps]    : adds gaps around every window on screen
 * [warpcursor]     : cursors follows and is placed in the center of the current window
 * [bloat]          : bloat is merge of all patches with the current master, just for fun

  [fib]:            https://github.com/c00kiemon5ter/monsterwm/tree/fib
  [monocleborders]: https://github.com/c00kiemon5ter/monsterwm/tree/monocleborders
  [showhide]:       https://github.com/c00kiemon5ter/monsterwm/tree/showhide
  [uselessgaps]:    https://github.com/c00kiemon5ter/monsterwm/tree/uselessgaps
  [warpcursor]:     https://github.com/c00kiemon5ter/monsterwm/tree/warpcursor
  [bloat]:          https://github.com/c00kiemon5ter/monsterwm/tree/bloat


Bugs
----

Fill bugs only on monsterwm-xcb, when you are sure the bug doesn't occur on [monsterwm][].

[monsterwm-xcb issues][monsterwm-xcb-bug] | [monsterwm issues][monsterwm-bug]

   [monsterwm-bug]: https://github.com/c00kiemon5ter/monsterwm/issues
   [monsterwm-xcb-bug]: https://github.com/Cloudef/monsterwm-xcb/issues


License
-------

Licensed under MIT/X Consortium License, see [LICENSE][law] file for more copyright and license information.

   [law]: https://raw.github.com/Cloudef/monsterwm-xcb/master/LICENSE

Thanks
------

[the suckless team][skls] for [dwm][],
[moetunes][] for [dminiwm][],
[pyknite][] for [catwm][],
[c00kiemonster][cookiemonster] for [monsterwm][]

  [skls]: http://suckless.org/
  [dwm]:  http://dwm.suckless.org/
  [moetunes]: https://github.com/moetunes
  [dminiwm]:  https://bbs.archlinux.org/viewtopic.php?id=126463
  [pyknite]: https://github.com/pyknite
  [catwm]:   https://github.com/pyknite/catwm
  [monsterwm]: https://github.com/c00kiemon5ter/monsterwm
  [cookiemonster]: https://github.com/c00kiemon5ter
