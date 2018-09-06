SUBDIRS = src data po doc

ACLOCAL_AMFLAGS = -I m4 ${ACLOCAL_FLAGS}

EXTRA_DIST = gtk-doc.make README.md

AM_DISTCHECK_CONFIGURE_FLAGS = \
	--enable-gtk-doc \
	--enable-docbook-docs \
	--with-systemdsystemunitdir=$$dc_install_base/$(systemdsystemunitdir)
