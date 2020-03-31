bin_PROGRAMS += controller-ramp/ovn-controller-ramp
controller_ramp_ovn_controller_ramp_SOURCES = \
	controller-ramp/binding.c \
	controller-ramp/binding.h \
	controller-ramp/gateway.c \
	controller-ramp/gateway.h \
	controller-ramp/ovn-controller-ramp.c \
	controller-ramp/ovn-controller-ramp.h \
	controller-ramp/ramp.c \
	controller-ramp/ramp.h
controller_ramp_ovn_controller_ramp_LDADD = lib/libovn.la $(OVS_LIBDIR)/libopenvswitch.la $(OVSBUILDDIR)/vtep/libvtep.la
man_MANS += controller-ramp/ovn-controller-ramp.8
EXTRA_DIST += controller-ramp/ovn-controller-ramp.8.xml
CLEANFILES += controller-ramp/ovn-controller-ramp.8
