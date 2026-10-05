PS5_HOST ?= 192.168.1.19
PS5_PORT ?= 9021

ifdef PS5_PAYLOAD_SDK
    include $(PS5_PAYLOAD_SDK)/toolchain/prospero.mk
else
    $(error PS5_PAYLOAD_SDK is undefined)
endif

ELF := pstream-install.elf

CFLAGS := -Wall -Werror -g -DTITLE_ID="\"PSTR00001\""
LDADD  := -lSceIpmi -lSceAppInstUtil

all: $(ELF)

$(ELF): main.c sce_sys/param.json sce_sys/icon0.png sce_sys/pic1.png
	$(CC) $(CFLAGS) $(LDADD) -o $@ main.c

clean:
	rm -f $(ELF)
