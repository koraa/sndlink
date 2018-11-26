deps = $(PWD)/deps/

CXXFLAGS+=-g -O0 --std=c++17 -Wall -Wextra -Wpedantic
CPPFLAGS+=-I"$(PWD)/deps/include"
LDFLAGS+=-L"$(PWD)/deps/lib64" -L"$(PWD)/deps/lib"

sndlink: ./sndlink.cpp Makefile $(libspeexdsp)
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) $(LDFLAGS) $< -o $@ \
		$(libspeexdsp) \
		-lportaudio -lboost_system -lpthread

libspeexdsp=deps/lib/libspeexdsp.a
$(libspeexdsp):
	mkdir -p deps/{build,lib,lib32,lib64,include,bin,sbin}
	rm -Rf deps/build/speexdsp
	cp -R vendor/speexdsp deps/build/speexdsp
	cd deps/build/speexdsp \
		&& ./autogen.sh \
		&& ./configure --prefix="$(deps)" \
		&& $(MAKE) install

PHONY: clean clean-deps clean-all

clean:
	rm sndlink -fv

clean-deps:
	rm -rfv ./deps

clean-all: clean clean-deps
