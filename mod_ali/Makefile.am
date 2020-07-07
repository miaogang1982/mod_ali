include $(top_srcdir)/build/modmake.rulesam
MODNAME=mod_ali

mod_LTLIBRARIES = mod_ali.la
mod_ali_la_SOURCES  = mod_ali.cpp
mod_ali_la_CFLAGS   = $(AM_CFLAGS)
mod_ali_la_LIBADD   = $(switch_builddir)/libfreeswitch.la -lalibabacloud-idst-common -lalibabacloud-idst-speech
mod_ali_la_LDFLAGS  = -avoid-version -module -no-undefined -shared
