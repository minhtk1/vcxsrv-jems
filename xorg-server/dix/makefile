ifeq ($(DEBUG),1)
DEFINES += FONTDEBUG XSERVER_DTRACE
endif
DEFINES += PIXMAN_API=

INCLUDES += ..\composite ..\miext\sync

LIBRARY=libdix
libmain_la_SOURCES =    \
	stubmain.c

libdix_la_SOURCES = 	\
	atom.c		\
	colormap.c	\
	cursor.c	\
	devices.c	\
	dispatch.c	\
	dispatch.h	\
	dixfonts.c	\
	main.c		\
	dixutils.c	\
	enterleave.c	\
	enterleave.h	\
	events.c	\
	eventconvert.c  \
	extension.c	\
	gc.c		\
	getevents.c	\
	globals.c	\
	glyphcurs.c	\
	grabs.c		\
	initatoms.c	\
	inpututils.c	\
	pixmap.c	\
	privates.c	\
	property.c	\
	ptrveloc.c	\
	region.c	\
	registry.c	\
	resource.c	\
	selection.c	\
	swaprep.c	\
	swapreq.c	\
	tables.c	\
	touch.c		\
	window.c

CSRCS = $(filter %.c,$(libdix_la_SOURCES)) $(filter %.c,$(libmain_la_SOURCES))
