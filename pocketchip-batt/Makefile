CFLAGS ?= -O2
CFLAGS += -std=c11 -Wall -Wextra
PREFIX ?= /usr

pocketchip-batt: src/pocketchip-batt.c
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ $<

check: pocketchip-batt
	sh test/run-tests.sh ./pocketchip-batt

coverage:
	$(MAKE) clean
	$(MAKE) CFLAGS="-O0 --coverage" check
	gcov -o . $$(ls *.gcda) | tail -1

install: pocketchip-batt
	install -D -m 0755 pocketchip-batt $(DESTDIR)$(PREFIX)/sbin/pocketchip-batt

clean:
	rm -f pocketchip-batt *.gcda *.gcno *.gcov

.PHONY: check coverage install clean
