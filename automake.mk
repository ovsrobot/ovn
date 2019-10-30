man_MANS += ovn-architecture.7
EXTRA_DIST += ovn-architecture.7.xml
CLEANFILES += ovn-architecture.7

# OVN northbound E-R diagram
#
# If "python" or "dot" is not available, then we do not add graphical diagram
# to the documentation.
if HAVE_PYTHON
if HAVE_DOT
OVSDB_DOT = $(run_python) ${OVSDIR}/ovsdb/ovsdb-dot.in
ovn-nb.gv: ${OVSDIR}/ovsdb/ovsdb-dot.in $(srcdir)/ovn-nb.ovsschema
	$(AM_V_GEN)$(OVSDB_DOT) --no-arrows $(srcdir)/ovn-nb.ovsschema > $@
ovn-nb.pic: ovn-nb.gv ${OVSDIR}/ovsdb/dot2pic
	$(AM_V_GEN)(dot -T plain < ovn-nb.gv | $(PYTHON) ${OVSDIR}/ovsdb/dot2pic -f 3) > $@.tmp && \
	mv $@.tmp $@
OVN_NB_PIC = ovn-nb.pic
OVN_NB_DOT_DIAGRAM_ARG = --er-diagram=$(OVN_NB_PIC)
CLEANFILES += ovn-nb.gv ovn-nb.pic
endif
endif

# OVN northbound schema documentation
EXTRA_DIST += ovn-nb.xml
CLEANFILES += ovn-nb.5
man_MANS += ovn-nb.5

OVSDB_DOC = $(run_python) ${OVSDIR}/ovsdb/ovsdb-doc
ovn-nb.5: \
	${OVSDIR}/ovsdb/ovsdb-doc $(srcdir)/ovn-nb.xml $(srcdir)/ovn-nb.ovsschema $(OVN_NB_PIC)
	$(AM_V_GEN)$(OVSDB_DOC) \
		$(OVN_NB_DOT_DIAGRAM_ARG) \
		--version=$(VERSION) \
		$(srcdir)/ovn-nb.ovsschema \
		$(srcdir)/ovn-nb.xml > $@.tmp && \
	mv $@.tmp $@

# OVN southbound E-R diagram
#
# If "python" or "dot" is not available, then we do not add graphical diagram
# to the documentation.
if HAVE_PYTHON
if HAVE_DOT
ovn-sb.gv: ${OVSDIR}/ovsdb/ovsdb-dot.in $(srcdir)/ovn-sb.ovsschema
	$(AM_V_GEN)$(OVSDB_DOT) --no-arrows $(srcdir)/ovn-sb.ovsschema > $@
ovn-sb.pic: ovn-sb.gv ${OVSDIR}/ovsdb/dot2pic
	$(AM_V_GEN)(dot -T plain < ovn-sb.gv | $(PYTHON) ${OVSDIR}/ovsdb/dot2pic -f 3) > $@.tmp && \
	mv $@.tmp $@
OVN_SB_PIC = ovn-sb.pic
OVN_SB_DOT_DIAGRAM_ARG = --er-diagram=$(OVN_SB_PIC)
CLEANFILES += ovn-sb.gv ovn-sb.pic
endif
endif

# OVN southbound schema documentation
EXTRA_DIST += ovn-sb.xml
CLEANFILES += ovn-sb.5
man_MANS += ovn-sb.5

ovn-sb.5: \
	${OVSDIR}/ovsdb/ovsdb-doc $(srcdir)/ovn-sb.xml $(srcdir)/ovn-sb.ovsschema $(OVN_SB_PIC)
	$(AM_V_GEN)$(OVSDB_DOC) \
		$(OVN_SB_DOT_DIAGRAM_ARG) \
		--version=$(VERSION) \
		$(srcdir)/ovn-sb.ovsschema \
		$(srcdir)/ovn-sb.xml > $@.tmp && \
	mv $@.tmp $@

# OVN interconnection northbound E-R diagram
#
# If "python" or "dot" is not available, then we do not add graphical diagram
# to the documentation.
if HAVE_PYTHON
if HAVE_DOT
ovn-inb.gv: ${OVSDIR}/ovsdb/ovsdb-dot.in $(srcdir)/ovn-inb.ovsschema
	$(AM_V_GEN)$(OVSDB_DOT) --no-arrows $(srcdir)/ovn-inb.ovsschema > $@
ovn-inb.pic: ovn-inb.gv ${OVSDIR}/ovsdb/dot2pic
	$(AM_V_GEN)(dot -T plain < ovn-inb.gv | $(PYTHON) ${OVSDIR}/ovsdb/dot2pic -f 3) > $@.tmp && \
	mv $@.tmp $@
OVN_INB_PIC = ovn-inb.pic
OVN_INB_DOT_DIAGRAM_ARG = --er-diagram=$(OVN_INB_PIC)
CLEANFILES += ovn-inb.gv ovn-inb.pic
endif
endif

# OVN interconnection northbound schema documentation
EXTRA_DIST += ovn-inb.xml
CLEANFILES += ovn-inb.5
man_MANS += ovn-inb.5

ovn-inb.5: \
	${OVSDIR}/ovsdb/ovsdb-doc $(srcdir)/ovn-inb.xml $(srcdir)/ovn-inb.ovsschema $(OVN_INB_PIC)
	$(AM_V_GEN)$(OVSDB_DOC) \
		$(OVN_INB_DOT_DIAGRAM_ARG) \
		--version=$(VERSION) \
		$(srcdir)/ovn-inb.ovsschema \
		$(srcdir)/ovn-inb.xml > $@.tmp && \
	mv $@.tmp $@

# OVN interconnection southbound E-R diagram
#
# If "python" or "dot" is not available, then we do not add graphical diagram
# to the documentation.
if HAVE_PYTHON
if HAVE_DOT
ovn-isb.gv: ${OVSDIR}/ovsdb/ovsdb-dot.in $(srcdir)/ovn-isb.ovsschema
	$(AM_V_GEN)$(OVSDB_DOT) --no-arrows $(srcdir)/ovn-isb.ovsschema > $@
ovn-isb.pic: ovn-isb.gv ${OVSDIR}/ovsdb/dot2pic
	$(AM_V_GEN)(dot -T plain < ovn-isb.gv | $(PYTHON) ${OVSDIR}/ovsdb/dot2pic -f 3) > $@.tmp && \
	mv $@.tmp $@
OVN_ISB_PIC = ovn-isb.pic
OVN_ISB_DOT_DIAGRAM_ARG = --er-diagram=$(OVN_ISB_PIC)
CLEANFILES += ovn-isb.gv ovn-isb.pic
endif
endif

# OVN interconnection southbound schema documentation
EXTRA_DIST += ovn-isb.xml
CLEANFILES += ovn-isb.5
man_MANS += ovn-isb.5

ovn-isb.5: \
	${OVSDIR}/ovsdb/ovsdb-doc $(srcdir)/ovn-isb.xml $(srcdir)/ovn-isb.ovsschema $(OVN_ISB_PIC)
	$(AM_V_GEN)$(OVSDB_DOC) \
		$(OVN_ISB_DOT_DIAGRAM_ARG) \
		--version=$(VERSION) \
		$(srcdir)/ovn-isb.ovsschema \
		$(srcdir)/ovn-isb.xml > $@.tmp && \
	mv $@.tmp $@

# Version checking for ovn-nb.ovsschema.
ALL_LOCAL += ovn-nb.ovsschema.stamp
ovn-nb.ovsschema.stamp: ovn-nb.ovsschema
	$(srcdir)/build-aux/cksum-schema-check $? $@
CLEANFILES += ovn-nb.ovsschema.stamp

# Version checking for ovn-sb.ovsschema.
ALL_LOCAL += ovn-sb.ovsschema.stamp
ovn-sb.ovsschema.stamp: ovn-sb.ovsschema
	$(srcdir)/build-aux/cksum-schema-check $? $@

# Version checking for ovn-inb.ovsschema.
ALL_LOCAL += ovn-inb.ovsschema.stamp
ovn-inb.ovsschema.stamp: ovn-inb.ovsschema
	$(srcdir)/build-aux/cksum-schema-check $? $@
CLEANFILES += ovn-inb.ovsschema.stamp

# Version checking for ovn-isb.ovsschema.
ALL_LOCAL += ovn-isb.ovsschema.stamp
ovn-isb.ovsschema.stamp: ovn-isb.ovsschema
	$(srcdir)/build-aux/cksum-schema-check $? $@
CLEANFILES += ovn-isb.ovsschema.stamp

pkgdata_DATA += ovn-nb.ovsschema
pkgdata_DATA += ovn-sb.ovsschema
pkgdata_DATA += ovn-inb.ovsschema
pkgdata_DATA += ovn-isb.ovsschema

CLEANFILES += ovn-sb.ovsschema.stamp
