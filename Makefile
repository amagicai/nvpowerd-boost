# nvpowerd-boost — LD_PRELOAD Dynamic Boost interposer
#
# Targets:
#   make            build nvpowerd-boost.so
#   make install    install library + systemd override (delta defaults to 0)
#   make uninstall  remove installed files
#   make clean

CC        ?= cc
CFLAGS    ?= -O2 -Wall -Wextra -fPIC
LDFLAGS   ?=
LDLIBS    ?= -ldl -lpthread

PREFIX      ?= /usr/local
LIBDIR      ?= $(PREFIX)/lib
SYSTEMD_DIR ?= /etc/systemd/system/nvidia-powerd.service.d

TARGET := nvpowerd-boost.so
OVERRIDE := systemd/nvidia-powerd.service.d/nvpowerd-boost.conf

.PHONY: all install uninstall clean

all: $(TARGET)

$(TARGET): nvpowerd-boost.c
	$(CC) $(CFLAGS) -shared -o $@ $< $(LDFLAGS) $(LDLIBS)

install: $(TARGET)
	install -Dm755 $(TARGET) $(DESTDIR)$(LIBDIR)/$(TARGET)
	install -Dm644 $(OVERRIDE) $(DESTDIR)$(SYSTEMD_DIR)/nvpowerd-boost.conf
	@echo
	@echo "Installed $(LIBDIR)/$(TARGET)"
	@echo "Installed $(SYSTEMD_DIR)/nvpowerd-boost.conf"
	@echo
	@echo "The override is INERT by default (NVIOC_BOOST_DELTA=0)."
	@echo "To enable a boost, edit the Environment= lines in:"
	@echo "    $(SYSTEMD_DIR)/nvpowerd-boost.conf"
	@echo "set NVIOC_BOOST_DELTA to the number of milliwatts to ADD"
	@echo "(e.g. 25000 for +25 W beyond the stock boost), then run:"
	@echo "    sudo systemctl daemon-reload"
	@echo "    sudo systemctl restart nvidia-powerd"

uninstall:
	rm -f $(DESTDIR)$(LIBDIR)/$(TARGET)
	rm -f $(DESTDIR)$(SYSTEMD_DIR)/nvpowerd-boost.conf

clean:
	rm -f $(TARGET)
