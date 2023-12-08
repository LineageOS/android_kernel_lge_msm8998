#!/bin/bash

DEFCONFIG_FILE=$1

if [ -z "$DEFCONFIG_FILE" ]; then
	echo "Need defconfig file(xxx_defconfig)!"
	exit -1
fi

if [ ! -e arch/arm64/configs/$DEFCONFIG_FILE ]; then
	echo "No such file : arch/arm64/configs/$DEFCONFIG_FILE"
	exit -1
fi

# make .config
env KCONFIG_NOTIMESTAMP=true \
make O=out ARCH=arm64 CROSS_COMPILE=aarch64-linux-android- ${DEFCONFIG_FILE}

# run menuconfig
env KCONFIG_NOTIMESTAMP=true \
make O=out menuconfig ARCH=arm64

make O=out savedefconfig ARCH=arm64
# copy .config to defconfig
mv out/defconfig arch/arm64/configs/${DEFCONFIG_FILE}
# clean kernel object
#make mrproper
rm -rf out
