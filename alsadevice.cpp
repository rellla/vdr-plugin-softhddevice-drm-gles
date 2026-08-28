// SPDX-License-Identifier: AGPL-3.0-or-later

/**
 * @file alsadevice.cpp
 * AlSA output device
 *
 * cAlsaDevice handles the Alsa output device
 *
 * @copyright 2009 - 2014 by Johns.  All Rights Reserved.
 * @copyright 2018 by zille.  All Rights Reserved.
 * @copyright 2025 - 2026 by Andreas Baierl. All Rights Reserved.
 *
 * @license{AGPL-3.0-or-later}
 */

#include <string>
#include <vector>

#include <alsa/asoundlib.h>

#include "alsadevice.h"
#include "config.h"
#include "logger.h"

/**
 * Empty log callback
 *
 * @ingroup audio
 */
static void AlsaNoopCallback( __attribute__ ((unused))
	const char *file, __attribute__ ((unused))
	int line, __attribute__ ((unused))
	const char *function, __attribute__ ((unused))
	int err, __attribute__ ((unused))
	const char *fmt, ...)
{
}

cAlsaDevice::cAlsaDevice(cSoftHdConfig *m_pConfig)
	: m_pPCMDevice(m_pConfig->ConfigAudioPCMDevice),
	  m_pMixerChannel(m_pConfig->ConfigAudioMixerChannel),
	  m_appendAES(m_pConfig->ConfigAudioAutoAES),
	  m_passthroughMask(m_pConfig->ConfigAudioPassthroughState ? m_pConfig->ConfigAudioPassthroughMask : 0),
	  m_downmix(m_pConfig->ConfigAudioDownmix)
{
}

/**
 * Initialize the ALSA audio output module
 *
 * @return true, if device init was successful
 */
bool cAlsaDevice::Init(void)
{
#ifdef ALSA_DEBUG
	(void)AlsaNoopCallback;
#else
	// disable display of alsa error messages
	snd_lib_error_set_handler(AlsaNoopCallback);
#endif

	if (!InitDevice())
		return false;

	InitMixer();

	return true;
}

/**
 * Cleanup the ALSA audio output module
 */
void cAlsaDevice::Exit(void)
{
	if (m_pPCMHandle) {
		snd_pcm_close(m_pPCMHandle);
		m_pPCMHandle = NULL;
	}
	if (m_pMixer) {
		snd_mixer_close(m_pMixer);
		m_pMixer = NULL;
		m_pMixerElem = NULL;
	}
}

/**
 * Open an ALSA device
 *
 * @param device      alsa device to be opened
 *
 * @return            the alsa device if successful, NULL otherwise
 */
char *cAlsaDevice::OpenDevice(const char *device)
{
	int err;
	char prefix[40];
	char tmp[80];

	if (!device)
		return NULL;

	LOGDEBUG2(L_SOUND, "audio: %s: try opening device '%s'", __FUNCTION__, device);

	if (ShouldAppendAES()) {
		if (!(strchr(device, ':')))
			snprintf(prefix, sizeof(prefix), "%s:", device);
		else
			snprintf(prefix, sizeof(prefix), "%s,", device);

		snprintf(tmp, sizeof(tmp), "%sAES0=%d,AES1=%d,AES2=0,AES3=%d",
			prefix,
			IEC958_AES0_NONAUDIO | IEC958_AES0_PRO_EMPHASIS_NONE,
			IEC958_AES1_CON_ORIGINAL | IEC958_AES1_CON_PCM_CODER,
			IEC958_AES3_CON_FS_48000);

		LOGDEBUG2(L_SOUND, "audio: %s: auto append AES: %s -> %s", __FUNCTION__, device, tmp);
	} else {
		snprintf(tmp, sizeof(tmp), "%s", device);
	}

	// open none blocking; if device is already used, we don't want wait
	if ((err = snd_pcm_open(&m_pPCMHandle, tmp, SND_PCM_STREAM_PLAYBACK, SND_PCM_NONBLOCK)) < 0) {
		LOGWARNING("audio: %s: could not open device '%s' error: %s", __FUNCTION__, device, snd_strerror(err));
		return NULL;
	}

	LOGDEBUG2(L_SOUND, "audio: %s: opened device '%s'", __FUNCTION__, device);

	return (char *)device;
}

/**
 * Find alsa device giving some search hints
 *
 * @param devname          interface identification (e.g. "pcm")
 * @param hint             string to compare with device name hints
 *
 * @return   an opened alsa device name if successful, NULL otherwise
 *           NOTE: Returned string is allocated and must be freed by caller
 */
char *cAlsaDevice::FindDevice(const char *devname, const char *hint)
{
	char **hints;
	int err;
	char **n;
	char *name;

	err = snd_device_name_hint(-1, devname, (void ***)&hints);
	if (err != 0) {
		LOGWARNING("audio: %s: Cannot get device names for %s!", __FUNCTION__, hint);
		return NULL;
	}

	n = hints;
	while (*n != NULL) {
		name = snd_device_name_get_hint(*n, "NAME");

		if (name && strstr(name, hint)) {
			if (OpenDevice(name)) {
				snd_device_name_free_hint((void **)hints);
				return name;
			}
		}

		if (name)
			free(name);
		n++;
	}

	snd_device_name_free_hint((void **)hints);
	return NULL;
}

/**
 * Search for an alsa pcm device and open it
 *
 * @return true, if a device could be opened
 */
bool cAlsaDevice::InitDevice(void)
{
	char *device = NULL;
	bool freeDevice = false;  // track if device needs to be freed
	int err;
	LOGDEBUG2(L_SOUND, "audio: %s", __FUNCTION__);

	// try user set device
	device = OpenDevice(getenv("ALSA_DEVICE"));
	if (!device)
		device = OpenDevice(m_pPCMDevice);

	// walkthrough hdmi: devices
	if (!device) {
		LOGDEBUG2(L_SOUND, "audio: %s: Try hdmi: devices...", __FUNCTION__);
		device = FindDevice("pcm", "hdmi:");
		freeDevice = (device != NULL);  // FindAlsaDevice allocates memory
	}

	// Rockchip mainline kernel
	if (!device) {
		LOGDEBUG2(L_SOUND, "audio: %s: Try default:CARD=hdmisound devices...", __FUNCTION__);
		device = FindDevice("pcm", "default:CARD=hdmisound");
		freeDevice = (device != NULL);  // FindAlsaDevice allocates memory
	}

	// walkthrough default: devices
	if (!device) {
		LOGDEBUG2(L_SOUND, "audio: %s: Try default: devices...", __FUNCTION__);
		device = FindDevice("pcm", "default:");
		freeDevice = (device != NULL);  // FindAlsaDevice allocates memory
	}

	// try default device
	if (!device) {
		LOGDEBUG2(L_SOUND, "audio: %s: Try default device...", __FUNCTION__);
		device = OpenDevice("default");
	}

	// use null device
	if (!device) {
		LOGDEBUG2(L_SOUND, "audio: %s: Try null device...", __FUNCTION__);
		device = OpenDevice("null");
	}

	if (!device) {
		LOGERROR("audio: %s: could not open any device!", __FUNCTION__);
		return false;
	}

	if (!strcmp(device, "null"))
		LOGWARNING("audio: %s: using device '%s'", __FUNCTION__, device);
	else
		LOGINFO("audio: using device '%s'", device);

	// Free device string if it was allocated by FindAlsaDevice
	if (freeDevice)
		free(device);

	if ((err = snd_pcm_nonblock(m_pPCMHandle, 0)) < 0) {
		LOGERROR("audio: %s: can't set block mode: %s", __FUNCTION__, snd_strerror(err));
	}

	return true;
}

/**
 * Initialize the ALSA mixer
 */
void cAlsaDevice::InitMixer(void)
{
	const char *device;
	const char *channel;
	snd_mixer_t *alsaMixer;
	snd_mixer_elem_t *alsaMixerElem;
	long alsaMixerElemMin;
	long alsaMixerElemMax;

	if (!(device = getenv("ALSA_MIXER"))) {
		if (!(device = m_pMixerDevice)) {
			device = "default";
		}
	}
	if (!(channel = getenv("ALSA_MIXER_CHANNEL"))) {
		if (!(channel = m_pMixerChannel)) {
			channel = "PCM";
		}
	}
	LOGDEBUG2(L_SOUND, "audio: %s: mixer %s - %s open", __FUNCTION__, device, channel);
	snd_mixer_open(&alsaMixer, 0);
	if (alsaMixer && snd_mixer_attach(alsaMixer, device) >= 0
		&& snd_mixer_selem_register(alsaMixer, NULL, NULL) >= 0
		&& snd_mixer_load(alsaMixer) >= 0) {

		const char *const alsaMixerElem_name = channel;

		alsaMixerElem = snd_mixer_first_elem(alsaMixer);
		while (alsaMixerElem) {
			const char *name;

			name = snd_mixer_selem_get_name(alsaMixerElem);
			if (!strcasecmp(name, alsaMixerElem_name)) {
				snd_mixer_selem_get_playback_volume_range(alsaMixerElem, &alsaMixerElemMin, &alsaMixerElemMax);
				m_ratio = 1000 * (alsaMixerElemMax - alsaMixerElemMin);
				LOGDEBUG2(L_SOUND, "audio: %s: %s mixer found %ld - %ld ratio %d", __FUNCTION__, channel, alsaMixerElemMin, alsaMixerElemMax, m_ratio);
				break;
			}

			alsaMixerElem = snd_mixer_elem_next(alsaMixerElem);
		}

		m_pMixer = alsaMixer;
		m_pMixerElem = alsaMixerElem;
	} else {
		LOGERROR("audio: %s: can't open mixer '%s'", __FUNCTION__, device);
	}
}

/**
 * Setup ALSA audio for requested format
 *
 * @param channels      Channels requested
 * @param sample_rate   SampleRate requested
 * @param passthrough   use pass-through (AC-3, ...) device
 *
 * @retval 0            everything ok
 * @retval -1           something gone wrong
 */
int cAlsaDevice::Setup(int channels, int sample_rate, bool passthrough, int downmix)
{
	int err;
	m_downmix = downmix;
	m_useMmap = false;

	// fill hw params
	snd_pcm_hw_params_t *hwparams;
	snd_pcm_hw_params_alloca(&hwparams);
	if ((err = snd_pcm_hw_params_any(m_pPCMHandle, hwparams)) < 0) {
		LOGERROR("audio: %s: Read HW config failed (%s)", __FUNCTION__, snd_strerror(err));
		return -1;
	}

	// pre-test mmap access
	if (!snd_pcm_hw_params_test_access(m_pPCMHandle, hwparams, SND_PCM_ACCESS_MMAP_INTERLEAVED))
		m_useMmap = true;

	// pre-test, if sample rate could be set near requested rate
	m_hwSampleRate = sample_rate;
	if ((err = snd_pcm_hw_params_set_rate_near(m_pPCMHandle, hwparams, &m_hwSampleRate, 0)) < 0) {
		LOGERROR("audio: %s: SampleRate %d not supported (%s)", __FUNCTION__, sample_rate, snd_strerror(err));
		return -1;
	}
	if ((int)m_hwSampleRate != sample_rate)
		LOGDEBUG2(L_SOUND, "audio: %s: sample_rate %d m_hwSampleRate %d", __FUNCTION__, sample_rate, m_hwSampleRate);

	// pre-test, if channels could be set near requested channels or if a donwmix is necessary
	m_hwNumChannels = channels;
	if ((err = snd_pcm_hw_params_set_channels_near(m_pPCMHandle, hwparams, &m_hwNumChannels)) < 0)
		LOGWARNING("audio: %s: %d channels not supported! %s", __FUNCTION__, m_hwNumChannels, snd_strerror(err));
	// force downmix without respect to the setup menu entry
	if ((int)m_hwNumChannels != channels && !passthrough)
		m_downmix = 1;

	// pre-test setting buffer time
	unsigned bufferTimeUs = 100'000;
	if ((err = snd_pcm_hw_params_set_buffer_time_near(m_pPCMHandle, hwparams, &bufferTimeUs, NULL)) < 0)
		LOGWARNING("audio: %s: bufferTime %d not supported! %s", __FUNCTION__, bufferTimeUs, snd_strerror(err));

	// set params
	if ((err = snd_pcm_set_params(m_pPCMHandle, SND_PCM_FORMAT_S16,
		m_useMmap ? SND_PCM_ACCESS_MMAP_INTERLEAVED : SND_PCM_ACCESS_RW_INTERLEAVED,
		m_hwNumChannels, m_hwSampleRate, 1, bufferTimeUs))) {

		snd_pcm_state_t state = snd_pcm_state(m_pPCMHandle);
		LOGERROR("audio: %s: set params error: %s\n"
			"           Requested: Channels %d SampleRate %d\n"
			"           Try to set: HWChannels %d HWSampleRate %d\n"
			"           Format %s , use mmap: %s\n"
			"           AlsaBufferTime %dms pcm state: %s",
			__FUNCTION__, snd_strerror(err),
			channels, sample_rate,
			m_hwNumChannels, m_hwSampleRate,
			snd_pcm_format_name(SND_PCM_FORMAT_S16), m_useMmap ? "yes" : "no",
			bufferTimeUs / 1000, snd_pcm_state_name(state));
		return -1;
	}

	// get the currently set hw params
	if ((err = snd_pcm_hw_params_current(m_pPCMHandle, hwparams)) < 0) {
		LOGERROR("audio: %s: Reading current HW config failed (%s)", __FUNCTION__, snd_strerror(err));
		return -1;
	}

	snd_pcm_hw_params_get_rate(hwparams, &m_hwSampleRate, 0);
	snd_pcm_hw_params_get_channels(hwparams, &m_hwNumChannels);

	snd_pcm_uframes_t periodSize;
	snd_pcm_uframes_t bufferSize;
	snd_pcm_get_params(m_pPCMHandle, &bufferSize, &periodSize);
	snd_pcm_hw_params_get_buffer_time(hwparams, &bufferTimeUs, 0);

	m_bufferSizeFrames = bufferSize;

	auto alsaMap = GetChannelLayoutAsArray();
	std::string channelMapString;
	for (size_t i = 0; i < alsaMap.size(); i++) {
		channelMapString += alsaMap[i];
		if (i < alsaMap.size() - 1)
			channelMapString += " ";
	}

	m_passthroughActive = passthrough;

	snd_pcm_state_t state = snd_pcm_state(m_pPCMHandle);
	LOGINFO("audio: %s:\n"
		"           Requested: Channels %d (%s) SampleRate %d%s\n"
		"           Set: HWChannels %d HWSampleRate %d\n"
		"           Format %s, use mmap: %s\n"
		"           AlsaBufferTime %dms, pcm state: %s\n"
		"           periodSize %d frames, bufferSize %d frames",
		__FUNCTION__,
		channels, channelMapString.c_str(), sample_rate, passthrough ? " -> passthrough" : " -> PCM",
		m_hwNumChannels, m_hwSampleRate,
		snd_pcm_format_name(SND_PCM_FORMAT_S16), m_useMmap ? "yes" : "no",
		bufferTimeUs / 1000, snd_pcm_state_name(state),
		periodSize, m_bufferSizeFrames);

	return 0;
}

/**
 * Wait until data can be written or read to/from the device
 * (Timeout is 150ms currently)
 *
 * @return 0, if timeout occured
 *         1, if device is ready
 *         negative error code, in case of an error
 */
int cAlsaDevice::WaitUntilReady(void)
{
	// check, if the alsa device is ready for input
	int ret = snd_pcm_wait(m_pPCMHandle, 150);
	if (ret < 0)
		LOGDEBUG2(L_SOUND, "audio: %s: Handle error in wait", __FUNCTION__);
	else if (ret == 0) {
		snd_pcm_state_t state = snd_pcm_state(m_pPCMHandle);
		LOGERROR("audio: %s: snd_pcm_wait() timeout (state %s)", __FUNCTION__, snd_pcm_state_name(state));
		if (state == SND_PCM_STATE_PREPARED) {
			LOGDEBUG2(L_SOUND, "audio: %s: force start", __FUNCTION__);
			snd_pcm_start(m_pPCMHandle);
		}
	}

	return ret;
}

/**
 * Write data to the output device
 *
 * @return number of written frames, or negative error code in case of an error
 */
int cAlsaDevice::Write(const void *data, int framesToWrite)
{
	if (m_useMmap)
		return snd_pcm_mmap_writei(m_pPCMHandle, data, framesToWrite);

	return snd_pcm_writei(m_pPCMHandle, data, framesToWrite);
}

/**
 * Check, if all frames have been written
 *
 * @retval true, if all frames have been written or we should try again later
 * @retval false, if not all frames have been written or an error occured
 */
bool cAlsaDevice::CheckWrittenFrames(int framesWritten, int framesToWrite)
{
	if (framesWritten == -EAGAIN) {
		return true;
	} else if (framesWritten < 0) {
		LOGWARNING("audio: %s: writei failed: %s", __FUNCTION__, snd_strerror(framesWritten));
		if (snd_pcm_recover(m_pPCMHandle, framesWritten, 0) < 0)
			LOGERROR("audio: %s: failed to recover from writei: %s", __FUNCTION__, snd_strerror(framesWritten));

		return false;
	} else if (framesWritten != framesToWrite) {
		LOGWARNING("audio: %s: not all frames written", __FUNCTION__);

		return false;
	}

//	LOGDEBUG2(L_SOUND, "audio: %s: %d frames (%dms) written", __FUNCTION__, framesWritten, m_alsa.FramesToMs(framesWritten));
	return true;
}

/**
 * Flush ALSA buffers internally
 *
 * @param drop       force a snd_pcm_drop of the audio frames already in the kernel
 */
void cAlsaDevice::FlushBuffers(bool drop)
{
	snd_pcm_state_t state = snd_pcm_state(m_pPCMHandle);
	if (state == SND_PCM_STATE_OPEN)
		return;

	LOGDEBUG2(L_SOUND, "audio: %s entered in pcm state %s", __FUNCTION__, snd_pcm_state_name(state));

	int err;
	if (m_passthroughActive && !drop) {
		switch (state) {
			case SND_PCM_STATE_SETUP:
			case SND_PCM_STATE_XRUN:
			case SND_PCM_STATE_DRAINING:
				err = snd_pcm_prepare(m_pPCMHandle);
				if (err < 0)
					LOGERROR("audio: %s: snd_pcm_prepare(): %s", __FUNCTION__, snd_strerror(err));
				break;
			default:
				break;
		}
	} else {
		err = snd_pcm_drop(m_pPCMHandle);
		if (err < 0)
			LOGERROR("audio: %s: snd_pcm_drop(): %s", __FUNCTION__, snd_strerror(err));
		err = snd_pcm_prepare(m_pPCMHandle);
		if (err < 0)
			LOGERROR("audio: %s: snd_pcm_prepare(): %s", __FUNCTION__, snd_strerror(err));
	}

	state = snd_pcm_state(m_pPCMHandle);
	LOGDEBUG2(L_SOUND, "audio: %s left in pcm state %s", __FUNCTION__, snd_pcm_state_name(state));
}

/**
 * Return the current hardware audio delay in frames
 */
int cAlsaDevice::GetHwDelayFrames(void)
{
	snd_pcm_sframes_t delayFrames;

	if (snd_pcm_delay(m_pPCMHandle, &delayFrames) < 0)
		delayFrames = 0L;

	return delayFrames;
}

/**
 * Handle an alsa error
 *
 * @return true, if an audio underrun happened, false otherwise
 */
bool cAlsaDevice::HandleError(int error)
{
	bool underrunHappened = snd_pcm_state(m_pPCMHandle) == SND_PCM_STATE_XRUN && !m_passthroughActive;

	int err = snd_pcm_recover(m_pPCMHandle, error, 0);

	if (err < 0) {
		LOGERROR("audio: %s: Cannot recover from %d (%s), state=%s",
			__FUNCTION__, error, snd_strerror(error), snd_pcm_state_name(snd_pcm_state(m_pPCMHandle)));

		return underrunHappened;
	}

	LOGDEBUG2(L_SOUND, "audio: %s: recovered %d (%s), state=%s",
			__FUNCTION__, error, snd_strerror(error), snd_pcm_state_name(snd_pcm_state(m_pPCMHandle)));

	return underrunHappened;
}

/**
 * Get the number of frames that could be written to the device
 *
 * @param sync     synchronize with hardware first
 *
 * @return number of frames, that could be written or
 *         negative error code in case of an error
 */
int cAlsaDevice::GetAvailableBufferFrames(bool sync)
{
	snd_pcm_state_t state = snd_pcm_state(m_pPCMHandle);
	if (state != SND_PCM_STATE_RUNNING &&
	    state != SND_PCM_STATE_PREPARED &&
	    state != SND_PCM_STATE_PAUSED) {
		LOGWARNING("audio: %s: invalid PCM state %s", __FUNCTION__, snd_pcm_state_name(state));

		return -EBADFD;
	}

	// query available space in alsa buffer
	int availableFrames = sync
		? snd_pcm_avail(m_pPCMHandle)
		: snd_pcm_avail_update(m_pPCMHandle);

	if (availableFrames == -EAGAIN)
		LOGDEBUG2(L_SOUND, "audio: %s: -EAGAIN", __FUNCTION__);
	else if (availableFrames < 0)
		LOGWARNING("audio: %s: snd_pcm_avail%s() failed: %s", __FUNCTION__, sync ? "" : "_update", snd_strerror(availableFrames));

	return availableFrames;
}

/**
 * Set alsa mixer volume (0-1000)
 *
 * @param volume      volume (0 .. 1000)
 */
void cAlsaDevice::SetVolume(int volume)
{
	int v;
	if (m_pMixer && m_pMixerElem) {
		v = (volume * m_ratio) / (1000 * 1000);
		snd_mixer_selem_set_playback_volume(m_pMixerElem, SND_MIXER_SCHN_FRONT_LEFT, v);
		snd_mixer_selem_set_playback_volume(m_pMixerElem, SND_MIXER_SCHN_FRONT_RIGHT, v);
	}
}

/**
 * FFmpeg does not have channels called "RL" or "RR"
 * So "rename" Alsas RL (rear left) and RR (rear right) to
 * FFmpegs BL (back left) and BR (back right) to make the channelmap
 * filter parser happy.
 *
 * @ingroup audio
 */
static const char *alsaToFFmpegChannel(const char *alsaName)
{
	if (!strcmp(alsaName, "RL")) return "BL";
	if (!strcmp(alsaName, "RR")) return "BR";

	return alsaName;
}

/**
 * Put ALSA channel layout in a dynamic array of strings
 *
 * @param pcmHandle      current Alsa PCM handle
 *
 * @return ALSA channel layout as an array of strings
 *
 * @ingroup audio
 */
std::vector<std::string> cAlsaDevice::GetChannelLayoutAsArray(void)
{
	std::vector<std::string> layout;
	snd_pcm_chmap_t *map = snd_pcm_get_chmap(m_pPCMHandle);
	if (!map)
		return layout;

	for (unsigned int i = 0; i < map->channels; i++) {
		const char *name = alsaToFFmpegChannel(snd_pcm_chmap_name(static_cast<snd_pcm_chmap_position>(map->pos[i])));
		if (!name)
			continue;
		layout.push_back(std::string(name));
	}
	free(map);
	return layout;
}
