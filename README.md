softhddevice-drm-gles
=====================

A software and GPU emulated HD output device for VDR  
(https://github.com/rellla/vdr-plugin-softhddevice-drm-gles)


Why do we need another softhddevice version?
--------------------------------------------
This is basically a fork of https://github.com/zillevdr/vdr-plugin-softhddevice-drm.git
and is aimed to merge all upstream commits back again - if possible.

The target of this version are embedded devices, see supported hardware.

The difference to the original fork is, that it adds some additional features like
resized video, hardware accelerated OSD rendering, support for Rpi4/5 and Amlogic
devices and a few more things. Read the commit history for detailed info.
There are a bunch of changes in the code, especially the drm handling was re-written and has
changed a bit to fully respect the atomic modesetting API now. Though everything should work
like it does with the original code, it's not guaranteed, that some bugs crept in.

As a principle, this softhddevice version is only dealing with mainline versions and standards,
which means you can (and have to) use mainline (or to be mainlined) kernel, ffmpeg and  mesa version.
This code does not work with vendor provided software or even closed source binaries.

www.LibreELEC.tv is good source to look for what is possible with mainlined media related software.
In the LibreELEC project you can find at least patches all the software, even if some piece of code
isn't able to be mainlined or simply not already there. Because LibreELEC is a distribution to run
kodi on, you'll find everything you need.


How does it internally work?
----------------------------
Video decoding is done with ffmpeg. If your hardware has some hardware decoder
and/or deinterlacer which is supported by ffmpeg, the video is hardware decoded
(depending on the codec), otherwise software decoding/ deinterlacing is done.
For software deinterlacing ffmpeg's bwdif (Bob Weaver Deinterlacing Filter) is
used. This is the highest quality software deinterlacer available in ffmpeg.
The OSD is either composed with OpenGL/ES or with CPU (see below).
Both, video and OSD are rendered directly on seperate drm planes with kms.
When the video is hardware decoded, we don't need much CPU, because everything is done
with a zero-copy approach. That's the same for OpenGL/ES OSD.

[See developer page](DEVELOPER.md)

Supported Hardware:
-------------------

|                 | 576i MPEG2 | 720p H.264 | 1080i H.264 | 1080p HEVC |
| --------------- | ---------- | ---------- | ----------- | ---------- |
| Allwinner       | Not tested | Not tested | Not tested  | Not tested |
| Amlogic         | Not tested | Not tested | Not tested  | Not tested |
| Raspberry Pi 2  | ❌          | SW         | SW          | SW         |
| Raspberry Pi 3  | Not tested | Not tested | Not tested  | Not tested |
| Raspberry Pi 4  | SW         | ✅          | ✅           | ✅          |
| Raspberry Pi 5  | SW         | SW         | SW          | ✅          |
| Rockchip RK3399 | ✅          | ✅          | ✅           | ✅          |

✅= Hardware decoding  
SW = Device does not support hardware decoding. Software decoding is used.  
❌= Device supports hardware decoding, but it is not implemented in this plugin.  

In general, any device that provides a DRM/KMS output and is supported by FFmpeg should work.

Known Bugs/ TODO:
-----------------
- attach/detach isn't implemented
- amlogic trickspeed is broken
- rpi avcodec_flush_buffers is broken in ffmpeg, use a workaround for now
- see https://github.com/rellla/vdr-plugin-softhddevice-drm-gles/issues


Install:
--------

	git clone https://github.com/rellla/vdr-plugin-softhddevice-drm-gles.git
	cd vdr-plugin-softhddevice-drm-gles
	make
	make install


OpenGL/ES:
----------
OpenGL/ES support is based on the work of Stefan Braun
(https://github.com/louisbraun/softhddevice-openglosd)

This enables GPU accelerated OSD rendering.
OpenGL/ES support is enabled, if gles2, egl and gbm are found on the system
To disable OpenGL/ES support (if autodetected), simply build with

	GLES=0 make

In this case, VDR is using CPU based OSD rendering.


Resolution:
----------
Setting the display resolution is possible with the -d switch (see below).
If no specific resolution is stated, the plugin searches for resolutions
with a refresh rate of 50Hz. If one is found, the highest possbile is used.
If no resolution at 50Hz is found, the plugin searches for resolutions at
60Hz and uses the highest possbile. If neither 50Hz or 60Hz can be found,
the plugin uses the highest possbile resolution with a refresh rate which is
first discovered.

When the plugin is unable to detect any resolution (e.g. if there is no TV/Monitor
attached, the plugin will not start. You can workaorund this by submitting
a fixed EDID to your config (see Documentation, way differs from architecture)

When using the -d switch, make sure you use the syntax of "widthXheight@refreshrate",
so for example: "-d 3840x2160@50" for 4K at 50Hz.


Raspberry Pi 4/5:
----------
It is highly recommended to use the kms driver with Raspberry Pi 4/5 and not the outdated
fkms driver. Be sure you have the following dtoverlay activated in your /boot/config.txt:

	dtoverlay=vc4-kms-v3d,cma-512

vc4-kms-v3d enables the new kms driver, cma-512 sets a custom cma size of 512MB.
This is the recommended size. Only change it when you exactly know what you are doing.

While using the outdated fkms driver still works with this plugin, there might be
some problems when viewing interlaced material and displaying the OSD.

To enable 4k resolution at 50Hz or 60Hz you have to set "hdmi_enable_4kp60=1" in
/boot/config.txt. Otherwise the Raspberry Pi is only capable of displaying 4K at
30Hz and the plugin will use 1080p at 50Hz according to the algorithm described above.

In case of permission problems under Raspberry Pi OS 5 with VDR installed via apt,
you might need to add the vdr user to the audio and render groups:

	usermod -a -G audio vdr
	usermod -a -G render vdr


Requirements:
-------------

- No running X!

- vdr (version >=2.6.6)

	Video Disk Recorder - turns a pc into a powerful set top box
	for DVB (http://www.tvdr.de/).

- ffmpeg

	Patched ffmpeg version needed (WIP LE version)

	Have a look at https://github.com/LibreELEC/LibreELEC.tv/tree/master/packages/multimedia/ffmpeg
	and choose the ffmpeg source and patches which matches your platform.
	Most of them have not yet been upstreamed, so you probably need to build ffmpeg on your own.
	LibreELEC supports Rockchip, Allwinner, Raspberry PI and (kind of) Amlogic.
	Have a look at https://github.com/LibreELEC/LibreELEC.tv/tree/master/projects to find platform
	specific patches.

- kernel

	Patched kernel needed (WIP LE version)

	Have a look at https://github.com/LibreELEC/LibreELEC.tv/tree/master/packages/linux
	and choose the linux source and patches which matches your platform.
	Some of the needed patches have not yet been upstreamed, so you probably need to build the kernel on your own.
	LibreELEC supports Rockchip, Allwinner, Raspberry PI and (kind of) Amlogic.
	Have a look at https://github.com/LibreELEC/LibreELEC.tv/tree/master/projects to find platform
	specific patches.

- alsa-lib

	Advanced Linux Sound Architecture Library
	http://www.alsa-project.org

- For OpenGL/ES support:

	- gles2 (Mesa)
	- egl (Mesa)
	- gbm (Mesa)
	- freetype2
	- glm - OpenGL Mathematics (GLM)
	- libpng (to write debug OSD pngs)


Commandline arguments:
----------------------
Use vdr -h to see the command line arguments supported by the plugin.

	-a audio_device
	-p device for pass-through
	-c audio mixer channel name
	-d display resolution (e.g. 1920x1080@50)
	-w workarounds
		disable-ogl-osd (to disable HW accelerated OSD)


Setup:	environment
-------------------

	ALSA_DEVICE=default
		alsa PCM device name
	ALSA_PASSTHROUGH_DEVICE=
		alsa pass-though (AC-3,E-AC-3,DTS,...) device name
	ALSA_MIXER=default
		alsa control device name
	ALSA_MIXER_CHANNEL=PCM
		alsa control channel name


Setup: /etc/vdr/setup.conf
--------------------------

	softhddevice-drm-gles.HideMainMenuEntry = 0
		0 = show softhddevice main menu entry, 1 = hide entry

	softhddevice-drm-gles.MaxSizeGPUImageCache = 128
		how many GPU memory should be used for image caching

	softhddevíce-drm-gles.WritePngs = 0
		0 = do nothing, 1 = write osd on every flush to /tmp
		this is only for debugging purposes

	softhddevice-drm-gles.AudioDelay = 0
		+n or -n ms
		delay audio or delay video

	softhddevice-drm-gles.AudioPassthrough = 0
		0 = none, 1 = PCM, 2 = MPA, 4 = AC-3, 8 = EAC-3, -X disable

		for PCM/AC-3/EAC-3 the pass-through device is used and the audio
		stream is passed undecoded to the output device.
		z.b. 12 = AC-3+EAC-3, 13 = PCM+AC-3+EAC-3
		note: MPA/DTS/TrueHD/... aren't supported yet
		negative values disable passthrough

	softhddevice-drm-gles.AudioDownmix = 0
		0 = none, 1 = downmix
		Use ffmpeg downmix of AC-3/EAC-3 audio to stereo.

	softhddevice-drm-gles.AudioSoftvol = 0
		0 = off, use hardware volume control
		1 = on, use software volume control

	softhddevice-drm-gles.AudioNormalize = 0
		0 = off, 1 = enable audio normalize

	softhddevice-drm-gles.AudioMaxNormalize = 0
		maximal volume factor/1000 of the normalize filter

	softhddevice-drm-gles.AudioCompression = 0
		0 = off, 1 = enable audio compression

	softhddevice-drm-gles.AudioMaxCompression = 0
		maximal volume factor/1000 of the compression filter

	softhddevice-drm-gles.AudioStereoDescent = 0
		reduce volume level (/1000) for stereo sources

	softhddevice-drm-gles.AudioBufferTime = 0
		0 = default (600 ms)
		1 - 1000 = size of the buffer in ms


SVDRP:
------

	PLAY Url    Play the media from the given url.
		Tested extension: *.mp3, *.mp4, *.m3u, *.m3u8

	Play a local file:
		svdrpsend plug softhddevice-drm-gles PLAY /path_to_file/media_file.mp4

	Play a playlist inside ConfigDirectory:
		svdrpsend plug softhddevice-drm-gles PLAY playlist_name.m3u

	Play a media file from web:
		svdrpsend plug softhddevice-drm-gles PLAY http://www.media-server/path_to_file/media_file.mp4


Copyright:
----------

Copyright (c) 2011 - 2013 by Johns.  All Rights Reserved.  
Copyright (c) 2018 - 2021 by zillevdr.  All Rights Reserved.  
Copyright (c) 2020 - 2025 by rellla.  All Rights Reserved.  


License:
--------

**AGPLv3**  

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU Affero General Public License as
published by the Free Software Foundation, either version 3 of the
License.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU Affero General Public License for more details.
