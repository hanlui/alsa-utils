/*
 * Copyright (C) 2013-2015 Intel Corporation
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <math.h>
#include <stdint.h>
#include <pthread.h>

#include <alsa/asoundlib.h>

#include "aconfig.h"
#include "gettext.h"

#include "common.h"
#include "alsa.h"

struct snd_pcm_container {
	snd_pcm_t *handle;
	snd_pcm_uframes_t period_size;
	snd_pcm_uframes_t buffer_size;
	snd_pcm_format_t format;
	unsigned short channels;
	size_t period_bytes;
	size_t sample_bits;
	size_t frame_bits;
	char *buffer;
};

static int set_snd_pcm_params(struct bat *bat, struct snd_pcm_container *sndpcm)
{
	snd_pcm_format_t format;
	snd_pcm_hw_params_t *params;
	unsigned int buffer_time = 0;
	unsigned int period_time = 0;
	unsigned int rate;
	int err;
	const char *device_name = snd_pcm_name(sndpcm->handle);

	/* Allocate a hardware parameters object. */
	snd_pcm_hw_params_alloca(&params);

	/* Fill it in with default values. */
	err = snd_pcm_hw_params_any(sndpcm->handle, params);
	if (err < 0) {
		fprintf(bat->err, _(E_MSG_SETDEV MSG_DEFAULT "%s: %s(%d)\n"),
				device_name, snd_strerror(err), err);
		return err;
	}

	/* Set access mode */
	err = snd_pcm_hw_params_set_access(sndpcm->handle, params,
			SND_PCM_ACCESS_RW_INTERLEAVED);
	if (err < 0) {
		fprintf(bat->err, _(E_MSG_SETDEV MSG_ACCESS "%s: %s(%d)\n"),
				device_name, snd_strerror(err), err);
		return err;
	}

	/* Set format */
	switch (bat->sample_size) {
	case 1:
		format = SND_PCM_FORMAT_S8;
		break;
	case 2:
		format = SND_PCM_FORMAT_S16_LE;
		break;
	case 3:
		format = SND_PCM_FORMAT_S24_3LE;
		break;
	case 4:
		format = SND_PCM_FORMAT_S32_LE;
		break;
	default:
		fprintf(bat->err, _(E_MSG_PARAMS MSG_PCMFORMAT "size=%d\n"),
				bat->sample_size);
		return -EINVAL;
	}
	err = snd_pcm_hw_params_set_format(sndpcm->handle, params, format);
	if (err < 0) {
		fprintf(bat->err, _(E_MSG_SETDEV MSG_PCMFORMAT
					"%d %s: %s(%d)\n"),
				format,
				device_name, snd_strerror(err), err);
		return err;
	}

	/* Set channels */
	err = snd_pcm_hw_params_set_channels(sndpcm->handle,
			params, bat->channels);
	if (err < 0) {
		fprintf(bat->err, _(E_MSG_SETDEV MSG_CHANNELS
					"%d %s: %s(%d)\n"),
				bat->channels,
				device_name, snd_strerror(err), err);
		return err;
	}

	/* Set sampling rate */
	rate = bat->rate;
	err = snd_pcm_hw_params_set_rate_near(sndpcm->handle,
			params, &bat->rate,
			0);
	if (err < 0) {
		fprintf(bat->err, _(E_MSG_SETDEV MSG_SAMPLERATE
					"%d %s: %s(%d)\n"),
				bat->rate,
				device_name, snd_strerror(err), err);
		return err;
	}
	if ((float) rate * (1 + RATE_RANGE) < bat->rate
			|| (float) rate * (1 - RATE_RANGE) > bat->rate) {
		fprintf(bat->err, _(E_MSG_PARAMS MSG_SAMPLERATE
					"requested %dHz, got %dHz\n"),
				rate, bat->rate);
		return -EINVAL;
	}

	if (snd_pcm_hw_params_get_buffer_time_max(params,
			&buffer_time, 0) < 0) {
		fprintf(bat->err, _(E_MSG_GETDEV MSG_BUFFERTIME
					"%d %s: %s(%d)\n"),
				buffer_time,
				device_name, snd_strerror(err), err);
		return -EINVAL;
	}

	if (buffer_time > MAX_BUFFERTIME)
		buffer_time = MAX_BUFFERTIME;

	period_time = buffer_time / DIV_BUFFERTIME;

	/* Set buffer time and period time */
	err = snd_pcm_hw_params_set_buffer_time_near(sndpcm->handle, params,
			&buffer_time, 0);
	if (err < 0) {
		fprintf(bat->err, _(E_MSG_SETDEV MSG_BUFFERTIME
					"%d %s: %s(%d)\n"),
				buffer_time,
				device_name, snd_strerror(err), err);
		return err;
	}

	err = snd_pcm_hw_params_set_period_time_near(sndpcm->handle, params,
			&period_time, 0);
	if (err < 0) {
		fprintf(bat->err, _(E_MSG_SETDEV MSG_PERIODTIME
					"%d %s: %s(%d)\n"),
				period_time,
				device_name, snd_strerror(err), err);
		return err;
	}

	/* Write the parameters to the driver */
	if (snd_pcm_hw_params(sndpcm->handle, params) < 0) {
		fprintf(bat->err, _(E_MSG_SETDEV MSG_HWPARAMS "%s: %s(%d)\n"),
				device_name, snd_strerror(err), err);
		return -EINVAL;
	}

	err = snd_pcm_hw_params_get_period_size(params,
			&sndpcm->period_size, 0);
	if (err < 0) {
		fprintf(bat->err, _(E_MSG_GETDEV MSG_PERIODSIZE
					"%zd %s: %s(%d)\n"),
				sndpcm->period_size,
				device_name, snd_strerror(err), err);
		return err;
	}

	err = snd_pcm_hw_params_get_buffer_size(params, &sndpcm->buffer_size);
	if (err < 0) {
		fprintf(bat->err, _(E_MSG_GETDEV MSG_BUFFERSIZE
					"%zd %s: %s(%d)\n"),
				sndpcm->buffer_size,
				device_name, snd_strerror(err), err);
		return err;
	}

	if (sndpcm->period_size == sndpcm->buffer_size) {
		fprintf(bat->err, _(E_MSG_PARAMS "can't use period"));
		fprintf(bat->err, _(" equal to buffer size (%zd)\n"),
				sndpcm->period_size);
		return -EINVAL;
	}

	err = snd_pcm_format_physical_width(format);
	if (err < 0) {
		fprintf(bat->err, _(E_MSG_PARAMS
					"snd_pcm_format_physical_width: %d\n"),
				err);
		return err;
	}
	sndpcm->sample_bits = err;

	sndpcm->frame_bits = sndpcm->sample_bits * bat->channels;

	/* Calculate the period bytes */
	sndpcm->period_bytes = sndpcm->period_size * sndpcm->frame_bits / 8;
	sndpcm->buffer = (char *) malloc(sndpcm->period_bytes);
	if (sndpcm->buffer == NULL) {
		fprintf(bat->err, _(E_MSG_MALLOC "size=%zd\n"),
				sndpcm->period_bytes);
		return -EINVAL;
	}

	return 0;
}

/*
 * Generate buffer to be played either from input file or from generated data
 * Return value
 * <0 error
 * 0 ok
 * >0 break
 */
static int generate_input_data(struct snd_pcm_container *sndpcm, int bytes,
		struct bat *bat)
{
	int err;
	static int load;
	void *buf;
	int max;

	if (bat->playback.file != NULL) {
		/* From input file */
		load = 0;

		while (1) {
			err = fread(sndpcm->buffer + load, 1,
					bytes - load, bat->fp);
			if (0 == err) {
				if (feof(bat->fp)) {
					fprintf(bat->log,
							_("End of playing.\n"));
					return 1;
				}
			} else if (err < bytes - load) {
				if (ferror(bat->fp)) {
					fprintf(bat->err, _(E_MSG_READFILE));
					fprintf(bat->err, _("%d\n"), err);
					return -EIO;
				}
				load += err;
			} else {
				break;
			}
		}
	} else {
		/* Generate sine wave */
		if ((bat->sinus_duration) && (load > bat->sinus_duration))
			return 1;

		switch (bat->sample_size) {
		case 1:
			buf = (int8_t *) sndpcm->buffer;
			max = INT8_MAX - ROUND_MARGIN;
			break;
		case 2:
			buf = (int16_t *) sndpcm->buffer;
			max = INT16_MAX - ROUND_MARGIN;
			break;
		case 3:
			buf = (int8_t *) sndpcm->buffer;
			max = (1 << 23) - 1 - ROUND_MARGIN;
			break;
		case 4:
			buf = (int32_t *) sndpcm->buffer;
			max = INT32_MAX - ROUND_MARGIN;
			break;
		default:
			fprintf(bat->err, _(E_MSG_PARAMS MSG_PCMFORMAT
						"size=%d\n"),
					bat->sample_size);
			return -EINVAL;
		}

		generate_sine_wave(bat, bytes * 8 / sndpcm->frame_bits,
				buf, max);

		load += (bytes * 8 / sndpcm->frame_bits);
	}

	bat->periods_played++;

	return 0;
}

static int write_to_pcm(const struct snd_pcm_container *sndpcm,
		int frames, struct bat *bat)
{
	int err;
	int offset = 0;
	int remain = frames;

	while (remain > 0) {
		err = snd_pcm_writei(sndpcm->handle, sndpcm->buffer + offset,
				remain);
		if (err == -EAGAIN || (err >= 0 && err < frames)) {
			snd_pcm_wait(sndpcm->handle, 500);
		} else if (err == -EPIPE) {
			fprintf(bat->err, _(E_MSG_WRITEPCM MSG_UNDERRUN
						"%s(%d)\n"),
					snd_strerror(err), err);
			snd_pcm_prepare(sndpcm->handle);
		} else if (err < 0) {
			fprintf(bat->err, _(E_MSG_WRITEPCM "%s(%d)\n"),
					snd_strerror(err), err);
			return err;
		}

		if (err > 0) {
			remain -= err;
			offset += err * sndpcm->frame_bits / 8;
		}
	}

	return 0;
}

static int write_to_pcm_loop(struct snd_pcm_container *sndpcm, struct bat *bat)
{
	int err;
	int bytes = sndpcm->period_bytes; /* playback buffer size */
	int frames = bytes * 8 / sndpcm->frame_bits; /* frame count */
	FILE *fp = NULL;

	if (bat->debugplay) {
		fp = fopen(bat->debugplay, "w+");
		if (fp == NULL) {
			fprintf(bat->err, _(E_MSG_OPENFILEC "%s %d\n"),
					bat->debugplay, -errno);
			return -errno;
		}
	}

	while (1) {
		err = generate_input_data(sndpcm, bytes, bat);
		if (err < 0)
			return err;
		else if (err > 0)
			break;

		if (bat->debugplay) {
			err = fwrite(sndpcm->buffer, 1, bytes, fp);
			if (err != bytes) {
				fprintf(bat->err, _(E_MSG_WRITEFILE "%s(%d)\n"),
						snd_strerror(err), err);
				return -EIO;
			}
		}

		if (bat->period_limit
				&& bat->periods_played >= bat->periods_total)
			break;

		err = write_to_pcm(sndpcm, frames, bat);
		if (err != 0)
			return err;
	}

	if (bat->debugplay)
		fclose(fp);

	snd_pcm_drain(sndpcm->handle);

	return 0;
}

/**
 * Play
 */
void *playback_alsa(struct bat *bat)
{
	int err = 0;
	struct snd_pcm_container sndpcm;

	fprintf(bat->log, _("Entering playback thread (ALSA).\n"));

	retval_play = 0;
	memset(&sndpcm, 0, sizeof(sndpcm));

	if (bat->playback.device == NULL) {
		fprintf(bat->err, _(E_MSG_NOPCMP "exit\n"));
		retval_play = 1;
		goto exit1;
	}

	err = snd_pcm_open(&sndpcm.handle, bat->playback.device,
			SND_PCM_STREAM_PLAYBACK, 0);
	if (err != 0) {
		fprintf(bat->err, _(E_MSG_OPENPCMP "%s(%d)\n"),
				snd_strerror(err), err);
		retval_play = 1;
		goto exit1;
	}

	err = set_snd_pcm_params(bat, &sndpcm);
	if (err != 0) {
		retval_play = 1;
		goto exit2;
	}

	if (bat->playback.file == NULL) {
		fprintf(bat->log, _("Playing generated audio sine wave"));
		bat->sinus_duration == 0 ?
			fprintf(bat->log, _(" endlessly\n")) :
			fprintf(bat->log, _("\n"));
	} else {
		fprintf(bat->log, _("Playing input audio file: %s\n"),
				bat->playback.file);
		bat->fp = fopen(bat->playback.file, "rb");
		if (bat->fp == NULL) {
			fprintf(bat->err, _(E_MSG_OPENFILEC "%s %d\n"),
					bat->playback.file, -errno);
			retval_play = 1;
			goto exit3;
		}
	}

	err = write_to_pcm_loop(&sndpcm, bat);
	if (err != 0) {
		retval_play = 1;
		goto exit4;
	}

exit4:
	if (bat->playback.file)
		fclose(bat->fp);
exit3:
	free(sndpcm.buffer);
exit2:
	snd_pcm_close(sndpcm.handle);
exit1:
	pthread_exit(&retval_play);
}

static int read_from_pcm(struct snd_pcm_container *sndpcm,
		int frames, struct bat *bat)
{
	int err = 0;
	int offset = 0;
	int remain = frames;

	while (remain > 0) {
		err = snd_pcm_readi(sndpcm->handle,
				sndpcm->buffer + offset, remain);
		if (err == -EAGAIN || (err >= 0 && err < remain)) {
			snd_pcm_wait(sndpcm->handle, 500);
		} else if (err == -EPIPE) {
			snd_pcm_prepare(sndpcm->handle);
			fprintf(bat->err, _(E_MSG_READPCM MSG_OVERRUN
						"%s(%d)\n"),
					snd_strerror(err), err);
		} else if (err < 0) {
			fprintf(bat->err, _(E_MSG_READPCM "%s(%d)\n"),
					snd_strerror(err), err);
			return err;
		}

		if (err > 0) {
			remain -= err;
			offset += err * sndpcm->frame_bits / 8;
		}
	}

	return 0;
}

static int read_from_pcm_loop(FILE *fp, int count,
		struct snd_pcm_container *sndpcm, struct bat *bat)
{
	int err = 0;
	int size, frames;
	int remain = count;

	while (remain > 0) {
		size = (remain <= sndpcm->period_bytes) ?
			remain : sndpcm->period_bytes;
		frames = size * 8 / sndpcm->frame_bits;

		/* read a chunk from pcm device */
		err = read_from_pcm(sndpcm, frames, bat);
		if (err != 0)
			return err;

		/* write the chunk to file */
		err = fwrite(sndpcm->buffer, 1, size, fp);
		if (err != size) {
			fprintf(bat->err, _(E_MSG_WRITEFILE "%s(%d)\n"),
					snd_strerror(err), err);
			return -EIO;
		}
		remain -= size;
		bat->periods_played++;

		if (bat->period_limit
				&& bat->periods_played >= bat->periods_total)
			break;
	}

	return 0;
}

/**
 * Record
 */
void *record_alsa(struct bat *bat)
{
	int err = 0;
	FILE *fp = NULL;
	struct snd_pcm_container sndpcm;
	struct wav_container wav;
	int count;

	pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);

	fprintf(bat->log, _("Entering capture thread (ALSA).\n"));

	retval_record = 0;
	memset(&sndpcm, 0, sizeof(sndpcm));

	if (bat->capture.device == NULL) {
		fprintf(bat->err, _(E_MSG_NOPCMC "exit\n"));
		retval_record = 1;
		goto exit1;
	}

	err = snd_pcm_open(&sndpcm.handle, bat->capture.device,
			SND_PCM_STREAM_CAPTURE, 0);
	if (err != 0) {
		fprintf(bat->err, _(E_MSG_OPENPCMC "%s(%d)\n"),
				snd_strerror(err), err);
		retval_record = 1;
		goto exit1;
	}

	err = set_snd_pcm_params(bat, &sndpcm);
	if (err != 0) {
		retval_record = 1;
		goto exit2;
	}

	remove(bat->capture.file);
	fp = fopen(bat->capture.file, "w+");
	if (fp == NULL) {
		fprintf(bat->err, _(E_MSG_OPENFILEC "%s %d\n"),
				bat->capture.file, -errno);
		retval_record = 1;
		goto exit3;
	}

	prepare_wav_info(&wav, bat);

	pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, NULL);
	pthread_setcanceltype(PTHREAD_CANCEL_DEFERRED, NULL);
	pthread_cleanup_push(snd_pcm_close, sndpcm.handle);
	pthread_cleanup_push(destroy_mem, sndpcm.buffer);
	pthread_cleanup_push((void *)close_file, fp);

	err = write_wav_header(fp, &wav, bat);
	if (err != 0) {
		retval_record = 1;
		goto exit4;
	}

	count = wav.chunk.length;
	fprintf(bat->log, _("Recording ...\n"));
	err = read_from_pcm_loop(fp, count, &sndpcm, bat);
	if (err != 0) {
		retval_record = 1;
		goto exit4;
	}

	/* Normally we will never reach this part of code (before fail_exit) as
		this thread will be cancelled by end of play thread. */
	pthread_cleanup_pop(0);
	pthread_cleanup_pop(0);
	pthread_cleanup_pop(0);

	snd_pcm_drain(sndpcm.handle);

exit4:
	fclose(fp);
exit3:
	free(sndpcm.buffer);
exit2:
	snd_pcm_close(sndpcm.handle);
exit1:
	pthread_exit(&retval_record);
}
