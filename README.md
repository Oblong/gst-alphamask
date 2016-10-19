
# gst-alphamask

gst-alphamask is a plugin to combine a video stream with an alpha mask stream to
produce a single video stream in A420, ARGB or AYUV formats.


It requires:

- GStreamer >= 1.6.0


# Installation

Download the latest release and untar it. Then, run the typical
sequence:

    $ ./configure --prefix=<gstreamer-prefix>
    $ make
    $ sudo make install

Where *gstreamer-prefix* should preferably be the same as your system
GStreamer installation directory

Once the plugin has been installed you can verify if the elements exist:

    $ gst-inspect-1.0 alphamask
    Plugin Details:
      Name                     alphamask
      Description              Alpha mask combinator
      Filename                 /home/jep/gst/master/plugins/libgstalphamask.so
      Version                  1.0.0
      License                  LGPL
      Source module            gst-alphamask
      Binary package           gst-alphamask
      Origin URL               http://oblong.com/

      alphamask: Alpha mask combinator

      1 features:
      +-- 1 elements


# Giving it a try

Use videotestsrc to generate an alpha masks with the moving ball pattern.

    $ gst-launch-1.0 videotestsrc pattern=2 ! queue ! mixer.sink_0 \
      videotestsrc pattern=18 ! queue ! am.alpha_sink \
      videotestsrc ! queue ! alphamask name=am ! queue ! mixer.sink_1 \
      videomixer name=mixer sink_0::zorder=0 sink_1::zorder=1 ! queue ! glimagesink

    $ gst-launch-1.0 videotestsrc pattern=0 ! queue ! mixer.sink_0 \
      videotestsrc pattern=18 ! queue ! am.alpha_sink \
      videotestsrc pattern=18 ! queue ! alphamask name=am ! queue ! mixer.sink_1 \
      videomixer name=mixer sink_0::zorder=0 sink_1::zorder=1 ! queue ! glimagesink

# License

gst-alphamask is freely available for download under the terms of the
[GNU Lesser General Public License, version 2.0](https://www.gnu.org/licenses/old-licenses/lgpl-2.0.html
"LGPLv2").
