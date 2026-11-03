libgdk_pixbuf-2.0.so.0: gdk-pixbuf 2.42.12, unmodified, from Alpine Linux
3.21's gdk-pixbuf-2.42.12-r1 package (/usr/lib/libgdk_pixbuf-2.0.so.0.4200.12).

The Firefox package ships gdk-pixbuf 2.44, which decodes every image through
glycin; the package replaced glycin with a no-op stub, so no PNG could load and
GTK aborted on its first icon ("Unrecognized image file format"). 2.42 has the
PNG and JPEG loaders built in (using the libpng16/libjpeg8 the package already
ships), and exports every gdk_pixbuf_* symbol the package uses. Kurono writes
this file over /apps/firefox/lib/libgdk_pixbuf-2.0.so.0 at Firefox launch.

License: LGPL-2.1-or-later. Source: https://gitlab.gnome.org/GNOME/gdk-pixbuf
(tag 2.42.12) and Alpine aports main/gdk-pixbuf.
