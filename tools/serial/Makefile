SERIALDUMP := serialdump

CFLAGS += -Wall -W -g -pedantic

all:	$(SERIALDUMP)

clean:
	-$(RM) $(SERIALDUMP)

$(SERIALDUMP):	serialdump.c
	$(CC) $(CFLAGS) -o $@ $<
