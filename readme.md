# sndlink

Point to point audio streaming over UDP with minimal latency with support for linux and android (using termux).
I wrote this because my laptop audio broke and I needed a solution to use my mobile phone as an audio output to watch some movies.

Existing solutions did not hold up, because latency was too high (streaming with ffmpeg, ffplay, mpv, vlc).
http://www.pogo.org.uk/~mark/trx/ features low latency, but I could not get it to work with termux on android: Lib oRTP was hard to compile
and alsa under termux does not have a suitable backend (opensles) in termux.

sndlink uses portaudio for alsa, oss, windows and linux support and uses an experimental portaudio version with opensles support on android.
Instead of RTP (which is a fairly large protocol) a very simple UDP protocol with sequence numbers is used.

## Features

* Linux & android support (using termux)
* Likely supports OSX, UNIXes, windows with cygwin
* Stable streaming over network
* Can restart the server/client without restarting the other
* Acting as the system audio output (with pulseaudio)
* Written in modern C++ (powerful language)

## Compiling

You will need a suitable unix build environment with the usual tools; that includes make, a C++17 capable compiler, gnu make, autoconf and so on.
On android I recommend installing https://github.com/termux/termux-app.

On linux you will need the portaudio and boost development packages installed. On android portaudio is automatically built with sndlink.

Clone this repository with submodules:

```
git clone https://github.com/koraa/sndlink --recursive
```

In the directory, you should just be able to compile sndlink; dependencies will automatically be compiled as well:

```
make -j4
```

## Using

```
sndlink server [PORT]
sndlink client IP/NAME [PORT]
```

### Streaming system audio with pulseaudio

Install pulseaudio with alsa support: https://wiki.archlinux.org/index.php/PulseAudio#Back-end_configuration
Also install PulseAudio Volume Control (https://freedesktop.org/software/pulseaudio/pavucontrol/); this is usually a packet called pavucontrol

As described here (https://wiki.archlinux.org/index.php/PulseAudio/Examples#Monitor_specific_output), add a null output:

```
pactl load-module module-null-sink sink_name=sndlink
```

Use PulseAudio Volume Control to have sndlink record from "Null Output" in the Recording tab. Use the "Playback" tab to set "Null Output" as the
device for any application whose audio you want to transmit. Under "Output Devices" you can set "Null Output" as the default device by using the
green check mark icon.
This makes sure any newly started applications' audio is transmitted.

## Copying

Copyright © 2018 by Karolin Varner.

MIT License

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
