SHELL := /bin/bash
deps = $(PWD)/deps/

CXXFLAGS+=-g -O0 --std=c++17 -Wall -Wextra -Wpedantic
CPPFLAGS+=-I"$(PWD)/deps/include"
LDFLAGS+=-L"$(PWD)/deps/lib64" -L"$(PWD)/deps/lib"

# Detect android
ifneq ($(strip $(shell uname -a | grep Android)),)
	libportaudio=$(libportaudio_android) -lOpenSLES -l7z
	libportaudio_dep=$(libportaudio_android)
else
	libportaudio=-lportaudio
	libportaudio_dep=
endif

.PHONY: all clean clean-deps clean-all
all: sndlink

libspeexdsp=deps/lib/libspeexdsp.a
$(libspeexdsp):
	mkdir -p deps/{build,lib,lib32,lib64,include,bin,sbin}
	rm -Rf deps/build/speexdsp
	cp -R vendor/speexdsp deps/build/speexdsp
	cd deps/build/speexdsp \
		&& ./autogen.sh \
		&& ./configure --prefix="$(deps)" \
		&& $(MAKE) install

libportaudio_android=deps/lib/libportaudio.a
$(libportaudio_android):
	mkdir -p deps/{build,lib,lib32,lib64,include,bin,sbin}
	rm -Rf deps/build/portaudio_opensles/
	cp -R vendor/portaudio_opensles deps/build/portaudio_opensles
	cd deps/build/portaudio_opensles \
		&& patch configure.in ../../../portaudio_opensles_config.in.diff \
		&& autoconf \
		&& ./configure --prefix="$(deps)" --with-opensles \
		&& $(MAKE) install

sndlink: ./sndlink.cpp Makefile $(libspeexdsp) $(libportaudio_dep)
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) $(LDFLAGS) $< -o $@ \
		$(libspeexdsp) $(libportaudio) -lboost_system -lpthread

clean:
	rm sndlink -fv

clean-deps:
	rm -rfv ./deps

clean-all: clean clean-deps
