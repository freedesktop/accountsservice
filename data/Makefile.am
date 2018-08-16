
dbusifdir   = $(datadir)/dbus-1/interfaces
dbusif_DATA = \
	org.freedesktop.Accounts.xml 		\
	org.freedesktop.Accounts.User.xml

dbusconfdir   = $(sysconfdir)/dbus-1/system.d
dbusconf_DATA = org.freedesktop.Accounts.conf

servicedir       = $(datadir)/dbus-1/system-services
service_in_files = org.freedesktop.Accounts.service.in
service_DATA     = $(service_in_files:.service.in=.service)

$(service_DATA): $(service_in_files) Makefile
	@sed -e "s|\@libexecdir\@|$(libexecdir)|" $< >$@

policydir       = $(datadir)/polkit-1/actions
policy_in_files = org.freedesktop.accounts.policy.in
policy_DATA     = $(policy_in_files:.policy.in=.policy)

org.freedesktop.accounts.policy: org.freedesktop.accounts.policy.in
	$(AM_V_GEN)$(MSGFMT) --xml --template $< -d $(top_srcdir)/po -o $@

if HAVE_SYSTEMD
systemdsystemunit_DATA = \
	accounts-daemon.service

accounts-daemon.service: accounts-daemon.service.in
	@sed -e "s|\@libexecdir\@|$(libexecdir)|" $< >$@

endif

EXTRA_DIST = 			\
	$(dbusif_DATA)		\
	$(dbusconf_DATA)	\
	$(service_in_files)	\
	$(policy_in_files)      \
	accounts-daemon.service.in

DISTCLEANFILES = 		\
	$(service_DATA)		\
	$(policy_DATA)

CLEANFILES = \
	accounts-daemon.service
