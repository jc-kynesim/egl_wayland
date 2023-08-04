Wayland tear demo

To build:

You'll need at least (I've probably missed some)

sudo apt install libavcodec-dev libavformat-dev libavfilter-dev \
 libdrm-dev libepoxy-dev

meson setup build64
cd build64
meson compile

To test

I've tested on HD (1920x1080 or 1920x1200) monitors. I don't know if this
is critical but there are clearly timing issues here so you might want to
try with that first. Use a Pi4 for this test - bipbop is H264 and you need
h/w decode. The issue exits on Pi5 with HEVC but it is slightly harder to
reproduce reliably.

Grab a copy of bipbop4.mp4
https://1drv.ms/v/s!AruozTLh98Uuhbo94TzSmel0giL5kw?e=1UaIfU

./hello_egl_wayland -d bipbop4.mp4

N.B. the -d in the above command line is important

You should get a test-card with a sweeping white circle on the right. It
should be playing fast but cleanly. Place a window (say the window you
launched the prog from) over the top left hand corner of the video s.t.
the lower edge of the overlapping window is approx level with the centre
of the circle. There should be obvious flickering in the circle.
Interestingly this doesn't occur whilst you are dragging the window.

This repo also builds dtest which attempts to reproduce the problem
without needing h/w video decode. I haven't made it fail here.

The active bits of the display process are all in init_window.c:

display_thread() is the wayland and frame dispatch loop thread

do_display_dmabuf() takes the AVFrame and passes it to
wayland

egl_wayland_out_display() Qs the frames to the display thread

I make no pretense that this code is anything other than a truly horrible
hack so I'm sorry in advance if you try to poke it. If you do need help
please do ask.

