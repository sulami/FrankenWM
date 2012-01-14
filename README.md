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
