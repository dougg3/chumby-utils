LDLIBS = -lgpiod

all: chumby_card_reader_daemon

chumby_card_reader_daemon.o: chumby_card_reader_daemon.c

chumby_card_reader_daemon: chumby_card_reader_daemon.o

clean:
	-rm -f chumby_card_reader_daemon.o
	-rm -f chumby_card_reader_daemon
