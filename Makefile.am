#
# linux-ha: Linux-HA code
#
# Copyright (C) 2002 Alan Robertson
#
# This program is free software; you can redistribute it and/or
# modify it under the terms of the GNU General Public License
# as published by the Free Software Foundation; either version 2
# of the License, or (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software
# Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.
#

##auxdir                  = @ac_aux_dir@
##AUX_DIST                = $(auxdir)/install-sh $(auxdir)/missing \
##                        $(auxdir)/mkinstalldirs
##AUX_DIST_EXTRA          = $(auxdir)/readline.m4 $(auxdir)/sys_errlist.m4 \
##                        $(auxdir)/sys_siglist.m4
EXTRA_DIST              = bootstrap ConfigureMe README.in libltdl.tar

RPMREL			= @RPMREL@
RPM			= @RPM@
RPMFLAGS		= -ta @RPMTARGET@
TARFILE			= @TARFILE@
AM_TAR			= tar
RPMDIR=$(HOME)/rpms
WEBDIR=/home/alanr/ha-web/download
HBWEBDIR=/home/alanr/ha-web/heartbeat
RPMSRC=$(DESTDIR)$(RPMDIR)/SRPMS/$(PACKAGE)-$(VERSION)-$(RPMREL).src.rpm
RPM386=$(DESTDIR)$(RPMDIR)/RPMS/i586/$(PACKAGE)-$(VERSION)-$(RPMREL).i586.rpm
RPMstonith=$(DESTDIR)$(RPMDIR)/RPMS/i586/$(PACKAGE)-stonith-$(VERSION)-$(RPMREL).i586.rpm
RPMpils=$(DESTDIR)$(RPMDIR)/RPMS/i586/$(PACKAGE)-pils-$(VERSION)-$(RPMREL).i586.rpm
SHAREDIR=@HA_DATADIR@
HBSHAREDIR=@HA_NOARCHDATAHBDIR@

ALL_RPMS = $(RPMSRC) $(RPM386) $(RPMstonith) $(RPMpils)

AUTOMAKE_OPTIONS	= foreign
##ACLOCAL                 = aclocal -I $(auxdir)

MAINTAINERCLEANFILES    = Makefile.in aclocal.m4 configure DRF/config-h.in \
                        DRF/stamp-h.in libtool.m4 ltdl.m4 libltdl.tar
coredir = 		@HA_COREDIR@
hauser = 		@HA_CCMUSER@

## proc-ha is left out from SUBDIRS (completely obsolete)

# 'buildtools' must be first in the list: tools to aid building of remainder.
SUBDIRS	= buildtools $(LIBLTDL_DIR) pkg port replace include lib	\
          heartbeat membership telecom resources tools doc cts  \
          config contrib

HANDY_DOCS = doc/ChangeLog doc/GettingStarted.html doc/DirectoryMap.txt
HBDOCS = doc/heartbeat_api.html

# Pass these to configure when running "make distcheck"
DISTCHECK_CONFIGURE_FLAGS = --with-initdir=prefix

rpm:		dist
		$(RPM) $(RPMFLAGS) $(TARFILE) </dev/null;

deb:	
	@@echo "====" ; \
	echo "  The debian directory has been removed upon request" ; \
	echo "  of the debian package maintainers. You probably" ; \
	echo "  need to first grab the debian directory from" ; \
	echo "  debian downstream." ; \
	echo "  Maybe rather try the debian way: apt-get source heartbeat" ; \
	echo "====" ; \
	exit 1
## somthing similar to this used to work some time ago...
# rm -rf $(distdir)
# tar -zxf $(TARFILE)
# cd $(distdir) ; dpkg-buildpackage -rfakeroot -us -uc
# rm -rf $(distdir)

OBS_TAR	= heartbeat.tar.gz
hg-dist:
	hg archive -t tgz $(OBS_TAR)

dist-clean-local:
	rm -f autoconf automake autoheader $(PACKAGE)-*.tar.gz

maintainer-clean-local:
	rm -f libltdl.tar

# "pkg" format for Solaris etc.
pkg:
		(cd pkg && $(MAKE) PWD=`pwd` pkg)

# "port" format for BSD
portfile:	dist
		( cd port && $(MAKE) PWD=`pwd` portfile)

drpm:		dist
		$(RPM) --nodeps $(RPMFLAGS) $(TARFILE) </dev/null;
handy: drpm handy-copy
	$HOME/bin/linuxhasums $VERSION

handy-copy:
	cd doc; $(MAKE)
	rm -f $(WEBDIR)/ChangeLog
	cp $(HANDY_DOCS) $(TARFILE) $(ALL_RPMS) $(WEBDIR)
	cp $(HBDOCS) $(HBWEBDIR)

.PHONY: rpm pkg handy handy-copy
