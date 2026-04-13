// SPDX-License-Identifier: AGPL-3.0-or-later

/**
 * @file hardwaredevice.h
 * Describes a hardware device
 *
 * @copyright 2018 - 2019 by zille.  All Rights Reserved.
 * @copyright 2025 - 2026 by Andreas Baierl. All Rights Reserved.
 *
 * @license{AGPL-3.0-or-later}
 */

#ifndef __HARDWAREDEVICE_H
#define __HARDWAREDEVICE_H

#include <cstdio>
#include <cstdint>
#include <string>

#include "logger.h"

/**
 * Hardware dependent quirks
 *
 * @ingroup misc
 */
enum HardwareQuirks {
	QUIRK_NO_HW_DEINT                 = 1 << 0,     ///< set, if no hw deinterlacer available
	QUIRK_CODEC_FLUSH_WORKAROUND      = 1 << 1,     ///< set, if we have to close and reopen the codec instead of avcodec_flush_buffers (rpi)
	QUIRK_CODEC_NEEDS_DIMENSION_PARSE = 1 << 2,     ///< set, if codec needs some infos for init (coded_width and coded_height)
	QUIRK_CODEC_SKIP_FIRST_FRAMES     = 1 << 3,     ///< set, if codec should skip first I-Frames
	QUIRK_CODEC_SKIP_NUM_FRAMES       = 2     ,     ///< skip QUIRK_CODEC_SKIP_NUM_FRAMES, in case QUIRK_CODEC_SKIP_FIRST_FRAMES is set
};

/**
 * Helper function to read a line from a given file
 *
 * @param[out] buf           pointer to the data
 * @param[out] size          size of the data at buf
 * @param[in] file           the filepointer to be read on
 *
 * @return the number of characters read
 *
 * @ingroup misc
 */
static inline size_t ReadLineFromFile(char *buf, size_t size, const char * file) {
	FILE *fd = NULL;
	size_t character;

	fd = fopen(file, "r");
	if (fd == NULL) {
		LOGERROR("%s: Can't open %s", __FUNCTION__, file);
		return 0;
	}

	character = getline(&buf, &size, fd);

	fclose(fd);

	return character;
}

/**
 * Hardware device
 *
 * @ingroup misc
 */
class cHardwareDevice
{
public:
	/** Create a new hardware device */
	cHardwareDevice(void) {
		char *txt_buf;
		char *read_ptr;
		size_t bufsize = 128;
		size_t read_size;

		txt_buf = (char *) calloc(bufsize, sizeof(char));

		read_size = ReadLineFromFile(txt_buf, bufsize, "/sys/firmware/devicetree/base/compatible");
		if (!read_size) {
			free((void *)txt_buf);
			LOGERROR("could not from read /sys/firmware/devicetree/base/compatible, no quirks set");
			return;
		}

		read_ptr = txt_buf;
		// be aware: device tree string can contain \x0 bytes, so every C-string function
		// thinks, we already reached the string's terminating null bytes
		// so copy the string into a temporary string without the "\0"
		char *_txt_buf = (char *) calloc(bufsize, sizeof(char));
		char *_read_ptr = _txt_buf;
		for (size_t i = 0; i < bufsize; i++) {
			if (memcmp(read_ptr, "\0", sizeof(char))) {
				memcpy(_read_ptr, read_ptr, sizeof(char));
				_read_ptr++;
			}
			read_ptr++;
		}

		read_ptr = txt_buf;
		while(read_size) {
			if (strstr(read_ptr, "bcm2836")) {
				m_deviceName = "bcm2836 (Raspberry Pi 2 Model B)";
				m_quirks |= QUIRK_CODEC_FLUSH_WORKAROUND;
				break;
			}
			if (strstr(read_ptr, "bcm2837")) {
				m_deviceName = "bcm2837 (Raspberry Pi 2 Model B v1.2/ 3 Model B, Raspberry Pi 3 Compute Module 3)";
				m_quirks |= QUIRK_CODEC_FLUSH_WORKAROUND;
				break;
			}
			if (strstr(read_ptr, "bcm2711")) {
				m_deviceName = "bcm2711 (Raspberry Pi 4 Model B, Compute Module 4, Pi 400)";
				m_quirks |= QUIRK_CODEC_FLUSH_WORKAROUND;
				break;
			}
			if (strstr(read_ptr, "bcm2712")) {
				m_deviceName = "bcm2712 (Raspberry Pi 5, Compute Module 5, Pi 500)";
				m_quirks |= QUIRK_CODEC_FLUSH_WORKAROUND;
				break;
			}
			if (strstr(read_ptr, "amlogic")) {
				m_deviceName = "amlogic";
				m_quirks |= QUIRK_CODEC_NEEDS_DIMENSION_PARSE
				         |  QUIRK_CODEC_SKIP_FIRST_FRAMES
				         |  QUIRK_NO_HW_DEINT;
				break;
			}

			read_size -= strlen(read_ptr) + 1;
			read_ptr = (char *)&read_ptr[(strlen(read_ptr) + 1)];
		}

		if (m_deviceName)
			LOGDEBUG("%s found%s%s", m_deviceName,
			    m_quirks & QUIRK_NO_HW_DEINT ?                 ", hw deinterlacer disabled" : "",
			    m_quirks & QUIRK_CODEC_FLUSH_WORKAROUND ?      ", flush workaround" : "",
			    m_quirks & QUIRK_CODEC_NEEDS_DIMENSION_PARSE ? ", parse H.264 dimensions" : "",
			    m_quirks & QUIRK_CODEC_SKIP_FIRST_FRAMES ?     ", skip first I-Frames" : "");
		else
			LOGDEBUG("%s found, no quirks set", txt_buf);

		free((void *)_txt_buf);
		free((void *)txt_buf);
	}

	/** Get Hardware Quirks */
	int GetQuirks(void) { return m_quirks; };
	/** Get Hardware Name (currently unused) */
	const char *GetName(void) { return m_deviceName; };

private:
	const char *m_deviceName = nullptr;  ///< device name
	int m_quirks = 0;                    ///< hardware dependent quirks for codec and and display
};

#endif
