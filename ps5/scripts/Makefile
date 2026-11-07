PS5_PAYLOAD_SDK ?= /opt/ps5-payload-sdk

ifdef PS5_PAYLOAD_SDK
    include $(PS5_PAYLOAD_SDK)/toolchain/prospero.mk
else
    $(error PS5_PAYLOAD_SDK is undefined)
endif

ELF := pstream-service.elf

CFLAGS := -Wall -Werror -O2
LDADD  := -lSceHttp2 -lSceSsl -lSceNet -lpthread

all: $(ELF)

$(ELF): pstream-service.c
	$(CC) $(CFLAGS) -o $@ pstream-service.c $(LDADD)

clean:
	rm -f $(ELF)
