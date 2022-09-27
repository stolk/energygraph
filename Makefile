
SRC=\
energygraph.c \
grapher.c

DISTFILES=\
$(SRC) \
Makefile \
energygraph.1 \
grapher.h \
hsv.h \
README.md \
LICENSE \
images

CFLAGS += -g -Wall -O2 -Wno-format-truncation

energygraph: $(SRC)
	$(CC) $(CFLAGS) -o energygraph $(SRC) -lm

clean:
	rm -f energygraph *.o

run: energygraph
	sudo ./energygraph

	
install: energygraph
	install -d ${DESTDIR}/usr/bin
	install -m 755 energygraph ${DESTDIR}/usr/bin/

uninstall:
	rm -f ${DESTDIR}/usr/bin/energygraph

tarball:
	tar cvzf ../energygraph_1.1.orig.tar.gz $(DISTFILES)

packageupload:
	debuild -S
	dput ppa:b-stolk/ppa ../energygraph_1.1-1_source.changes

