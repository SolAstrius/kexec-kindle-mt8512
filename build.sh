#!/bin/sh
# Build module/kexec_min.ko against Amazon's kernel source, in Docker.
#
#   ./build.sh Kindle_src_5.17.3_4386490030.tar.gz
#
# The tarball is Amazon's GPL source release for the Kindle. Only its
# gplrelease/linux-4.9.tar.gz is used; it is unpacked once into build/linux.
set -eu
cd "$(dirname "$0")"

[ $# -eq 1 ] || { echo "usage: $0 Kindle_src_<version>.tar.gz" >&2; exit 2; }
src=$1

if [ ! -f build/linux/Makefile ]; then
	mkdir -p build/linux
	echo "unpacking the kernel source into build/linux"
	tar -xzOf "$src" gplrelease/linux-4.9.tar.gz | tar -xz -C build/linux
fi

docker build -q -t kexec-kindle-mt8512-build docker >/dev/null
docker run --rm -u "$(id -u):$(id -g)" -v "$PWD:/work" -w /work kexec-kindle-mt8512-build sh -ec '
	cp kernel/config build/linux/.config
	make -s -C build/linux olddefconfig modules_prepare
	# CRCs of the running kernel, so the module passes MODVERSIONS checks.
	cp kernel/Module.symvers build/linux/Module.symvers
	make -s -C build/linux M=/work/module \
		KCFLAGS="-Wno-error=attribute-alias -Wno-error=stringop-truncation" modules
	arm-linux-gnueabihf-strip --strip-debug module/kexec_min.ko
	modinfo -F vermagic module/kexec_min.ko
'
echo "built module/kexec_min.ko"
