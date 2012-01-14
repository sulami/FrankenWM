monsterwm-xcb
=============

→ tiny(!) and monsterous!
----------------------
This is xcb port of monsterwm tiling window manager.

For more detailed README refer to vanilla monsterwm
[upstream]: https://github.com/c00kiemon5ter/monsterwm

Installation
------------

You need libxcb, xcb-proto, xcb-util, xcb-util-keysyms and xcb-util-wm then,
copy `config.def.h` as `config.h`
and edit to suit your needs.
Build and install.

    $ cp config.def.h config.h
    $ $EDITOR config.h
    $ make
    # make clean install


Bugs
----

!IMPORTANT!
   Fill bug only on monsterwm-xcb when you are sure the bug doesn't occur on vanilla monsterwm by [c00kiemonster][].
   Otherwise fill the bug to monsterwm's upstream.

  [upstream]: https://github.com/c00kiemon5ter/monsterwm/issues
  [xcb]: https://github.com/Cloudef/monsterwm-xcb/issues


License
-------

Licensed under MIT/X Consortium License, see [LICENSE][law] file for more copyright and license information.

  [law]: https://raw.github.com/Cloudef/monsterwm-xcb/master/LICENSE

Thanks
------

[the suckless team][skls] for [dwm][],
[moetunes][] for [dminiwm][],
[pyknite][] for [catwm][],
[c00kiemonster][] for [monsterwm][]
[xcb documentation][] for [unhelpful and helpful documetnation][]

  [skls]: http://suckless.org/
  [dwm]:  http://dwm.suckless.org/
  [moetunes]: https://github.com/moetunes
  [dminiwm]:  https://bbs.archlinux.org/viewtopic.php?id=126463
  [pyknite]: https://github.com/pyknite
  [catwm]:   https://github.com/pyknite/catwm
  [monsterwm]: https://github.com/c00kiemon5ter/monsterwm
