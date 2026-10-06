PS5_PAYLOAD_SDK ?= /opt/ps5-payload-sdk
PS5_HOST ?= 192.168.1.19
PS5_PORT ?= 9021

ifdef PS5_PAYLOAD_SDK
    include $(PS5_PAYLOAD_SDK)/toolchain/prospero.mk
else
    $(error PS5_PAYLOAD_SDK is undefined)
endif

ELF := pstream-install.elf

CFLAGS := -Wall -Werror -g -DTITLE_ID="\"PSTR00001\""
LDADD  := -lSceIpmi -lSceAppInstUtil -lSceNet

all: $(ELF)

$(ELF): main.c sce_sys/param.json sce_sys/icon0.png sce_sys/pic1.png \
        ../www/index.html ../www/app.js ../www/app.css ../www/input.js ../www/hls.min.js ../www/logo.png ../www/favicon.png ../www/version.json \
        ../scripts/pstream-service.elf
	$(CC) $(CFLAGS) $(LDADD) -o $@ main.c

clean:
	rm -f $(ELF)
