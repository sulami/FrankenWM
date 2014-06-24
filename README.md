monsterwm-xcb
=============

This is the sulami-edition of [cloudef's monsterwm-xcb][xcb], which is the
xcb-version of [c00kiemon5ter's monsterwm][orig]. I mainly added patches from
the original to the xcb version.

  [xcb]: https://github.com/cloudef/monsterwm-xcb
  [orig]: https://github.com/c00kiemon5ter/monsterwm

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

Monsterwm uses git branches as dwm-style patches. Just git merge a branch from
this repo into master and you should be golden. Patches are only tested on
their own, multiple patches might collide, please contact me when this happens.

The branches are (so far):

  * master - the base to build on
  * centerwindow - place the current window floating in the center of the
  screen
  * fibonacci - tiling mode which halfs the windows going down the stack (think
  bspwm)
  * initlayouts - uses one desktop per layout by default
  * showhide - show/hide all windows on all desktops (still somewhat buggy)
  * uselessgaps - add gaps around the windows to see you wallpaper
  * cleanup - my personal code cleanup branch, do not use it. behaves like
  master but does not take patches (yet)

Bugs
----

Fill bugs only on monsterwm-xcb, when you are sure the bug doesn't occur on
[monsterwm][].

[monsterwm-xcb issues][monsterwm-xcb-bug] | [monsterwm issues][monsterwm-bug]

   [monsterwm-bug]: https://github.com/c00kiemon5ter/monsterwm/issues
   [monsterwm-xcb-bug]: https://github.com/Cloudef/monsterwm-xcb/issues


License
-------

Licensed under MIT/X Consortium License, see [LICENSE][law] file for more
copyright and license information.

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

