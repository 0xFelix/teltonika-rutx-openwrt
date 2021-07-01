# teltonika-rutx-openwrt

This is the Teltonika RUTX package feed for OpenWrt.
It contains the r2ec kernel module which is needed for LEDs and GPIOs provided by the STM32 EC of Teltonika RUTX devices.

The r2ec kernel modules was extracted from the Teltonika GPL sources (`RUTX_R_00.02.06.1`) found at [GPL - Teltonika Networks Wiki](https://wiki.teltonika-networks.com/view/GPL) and adapted for Linux 5.4+ kernels.

## Usage

This repository is intended to be layered on-top of an OpenWrt buildroot. If you do not have an OpenWrt buildroot installed, see the documentation at: [OpenWrt Buildroot – Installation](http://wiki.openwrt.org/doc/howto/buildroot.exigence) on the OpenWrt support site.

To add it to the buildroot add the following line to feeds.conf.default (or feeds.conf if it exists):
```
src-git teltonika https://github.com/0xFelix/teltonika-rutx-openwrt.git
```

To install all its package definitions, run:
```
./scripts/feeds update teltonika-rutx
./scripts/feeds install -a -p teltonika-rutx
```

Then run `make menuconfig` and select `kmod-r2ec` under `Kernel modules --> Other modules`
