# lighttpd2

Lighttpd 2.0 was an experiment and was supposed to replace 1.4.x (same as 1.5 ...), but that plan has been abandoned many years ago.  There will be no production release of Lighttpd 2.0.

Apart from an internal rewrite from scratch it uses a completely new config concept (and language / syntax).

## Documentation

<https://doc.lighttpd.net/lighttpd2/>

## Installation

You can find various repositories for debian packages on <https://debian.lighttpd.net/>.

## Build dependencies

* c compiler
* [meson](https://mesonbuild.com/)
* pkg-config
* libev
* libidn
* ragel
* glib2.0 (>= 2.16)
* optional
  * lua >= 5.1 (optional) (highly recommended)
  * zlib (optional: for mod_deflate deflate/gzip compression)
  * bzip2 (optional: for mod_deflate bzip2 compression)
  * gnutls (optional: for mod_gnutls)
  * openssl (optional: for mod_openssl)

## Build instructions

Setup a build directory `build`:

    meson setup build --prefix /usr/local

Compile it:

    meson compile -C build

Run tests:

    meson test -C build

Install:

    meson install -C build
