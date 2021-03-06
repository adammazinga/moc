/*
 * MOC - music on console
 * Copyright (C) 2005 Damian Pietras <daper@daper.net>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 */

/* The code is based on libxmms-flac written by Josh Coalson. */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <string.h>
#include <FLAC/all.h>
#include <stdlib.h>
#include <strings.h>

/*#define DEBUG*/

#include "common.h"
#include "audio.h"
#include "decoder.h"
#include "server.h"
#include "log.h"
#include "io.h"

/* by LEGACY_FLAC we mean pre-1.1.3, before FLAC__SeekableStreamDecoder was merged into FLAC__StreamDecoder */
#if !defined(FLAC_API_VERSION_CURRENT) || FLAC_API_VERSION_CURRENT < 8
#define LEGACY_FLAC
#else
#undef LEGACY_FLAC
#endif

#define MAX_SUPPORTED_CHANNELS		2

#define SAMPLES_PER_WRITE		512
#define SAMPLE_BUFFER_SIZE ((FLAC__MAX_BLOCK_SIZE + SAMPLES_PER_WRITE) * MAX_SUPPORTED_CHANNELS * (32/8))

struct flac_data
{
#ifdef LEGACY_FLAC
	FLAC__SeekableStreamDecoder *decoder;
#else
	FLAC__StreamDecoder *decoder;
#endif
	struct io_stream *stream;
	int bitrate;
	int avg_bitrate;
	int abort; /* abort playing (due to an error) */

	unsigned length;
	unsigned total_samples;

	FLAC__byte sample_buffer[SAMPLE_BUFFER_SIZE];
	unsigned sample_buffer_fill;

	/* sound parameters */
	unsigned bits_per_sample;
	unsigned sample_rate;
	unsigned channels;

	FLAC__uint64 last_decode_position;

	int ok; /* was this stream successfully opened? */
	struct decoder_error error;
};

/* Convert FLAC big-endian data into PCM little-endian. */
static size_t pack_pcm_signed (FLAC__byte *data,
		const FLAC__int32 * const input[], unsigned wide_samples,
		unsigned channels, unsigned bps)
{
	FLAC__byte * const start = data;
	FLAC__int32 sample;
	const FLAC__int32 *input_;
	unsigned samples, channel;
	unsigned bytes_per_sample;
	unsigned incr;

	if (bps == 24)
		bps = 32; /* we encode to 32-bit words */
	bytes_per_sample = bps / 8;
	incr = bytes_per_sample * channels;

	for (channel = 0; channel < channels; channel++) {
		samples = wide_samples;
		data = start + bytes_per_sample * channel;
		input_ = input[channel];

		while(samples--) {
			sample = *input_++;

			switch(bps) {
				case 8:
					data[0] = sample;
					break;
				case 16:
					data[1] = (FLAC__byte)(sample >> 8);
					data[0] = (FLAC__byte)sample;
					break;
				case 32:
					data[3] = (FLAC__byte)(sample >> 16);
					data[2] = (FLAC__byte)(sample >> 8);
					data[1] = (FLAC__byte)sample;
					data[0] = 0;
					break;
			}

			data += incr;
		}
	}

	debug ("Converted %d bytes",
			wide_samples * channels * bytes_per_sample);

	return wide_samples * channels * bytes_per_sample;
}

static FLAC__StreamDecoderWriteStatus write_callback (
#ifdef LEGACY_FLAC
		const FLAC__SeekableStreamDecoder *decoder ATTR_UNUSED,
#else
		const FLAC__StreamDecoder *decoder ATTR_UNUSED,
#endif
		const FLAC__Frame *frame,
		const FLAC__int32 * const buffer[], void *client_data)
{
	struct flac_data *data = (struct flac_data *)client_data;
	const unsigned wide_samples = frame->header.blocksize;

	if (data->abort)
		return FLAC__STREAM_DECODER_WRITE_STATUS_ABORT;

	data->sample_buffer_fill = pack_pcm_signed (
			data->sample_buffer, buffer, wide_samples,
			data->channels, data->bits_per_sample);

	return FLAC__STREAM_DECODER_WRITE_STATUS_CONTINUE;
}

static void metadata_callback (
#ifdef LEGACY_FLAC
		const FLAC__SeekableStreamDecoder *decoder ATTR_UNUSED,
#else
		const FLAC__StreamDecoder *decoder ATTR_UNUSED,
#endif
		const FLAC__StreamMetadata *metadata, void *client_data)
{
	struct flac_data *data = (struct flac_data *)client_data;

	if (metadata->type == FLAC__METADATA_TYPE_STREAMINFO) {
		debug ("Got metadata info");

		data->total_samples =
			(unsigned)(metadata->data.stream_info.total_samples
				   & 0xffffffff);
		data->bits_per_sample =
			metadata->data.stream_info.bits_per_sample;
		data->channels = metadata->data.stream_info.channels;
		data->sample_rate = metadata->data.stream_info.sample_rate;
		data->length = data->total_samples / data->sample_rate;
	}
}

static void error_callback (
#ifdef LEGACY_FLAC
		const FLAC__SeekableStreamDecoder *decoder ATTR_UNUSED,
#else
		const FLAC__StreamDecoder *decoder ATTR_UNUSED,
#endif
		FLAC__StreamDecoderErrorStatus status, void *client_data)
{
	struct flac_data *data = (struct flac_data *)client_data;

	if (status != FLAC__STREAM_DECODER_ERROR_STATUS_LOST_SYNC) {
		debug ("Aborting due to error");
		data->abort = 1;
	}
	else
		decoder_error (&data->error, ERROR_FATAL, 0, "FLAC: lost sync");
}

#ifdef LEGACY_FLAC
static FLAC__SeekableStreamDecoderReadStatus read_callback (
		const FLAC__SeekableStreamDecoder *decoder ATTR_UNUSED,
		FLAC__byte buffer[], unsigned *bytes, void *client_data)
#else
static FLAC__StreamDecoderReadStatus read_callback (
		const FLAC__StreamDecoder *decoder ATTR_UNUSED,
		FLAC__byte buffer[], size_t *bytes, void *client_data)
#endif
{
	struct flac_data *data = (struct flac_data *)client_data;
	ssize_t res;

	res = io_read (data->stream, buffer, *bytes);

	if (res > 0) {
		*bytes = res;
#ifdef LEGACY_FLAC
		return FLAC__SEEKABLE_STREAM_DECODER_READ_STATUS_OK;
#else
		return FLAC__STREAM_DECODER_READ_STATUS_CONTINUE;
#endif
	}

	if (res == 0) {
		*bytes = 0;
		/* not sure why this works, but if it ain't broke... */
		return FLAC__STREAM_DECODER_READ_STATUS_END_OF_STREAM;
	}

	error ("read error: %s", io_strerror(data->stream));

#ifdef LEGACY_FLAC
	return FLAC__SEEKABLE_STREAM_DECODER_READ_STATUS_ERROR;
#else
	return FLAC__STREAM_DECODER_READ_STATUS_ABORT;
#endif
}

#ifdef LEGACY_FLAC
static FLAC__SeekableStreamDecoderSeekStatus seek_callback (
		const FLAC__SeekableStreamDecoder *decoder ATTR_UNUSED,
		FLAC__uint64 absolute_byte_offset, void *client_data)
#else
static FLAC__StreamDecoderSeekStatus seek_callback (
		const FLAC__StreamDecoder *decoder ATTR_UNUSED,
		FLAC__uint64 absolute_byte_offset, void *client_data)
#endif
{
	struct flac_data *data = (struct flac_data *)client_data;

#ifdef LEGACY_FLAC
	return io_seek(data->stream, absolute_byte_offset, SEEK_SET) >= 0
		? FLAC__SEEKABLE_STREAM_DECODER_SEEK_STATUS_OK
		: FLAC__SEEKABLE_STREAM_DECODER_SEEK_STATUS_ERROR;
#else
	return io_seek(data->stream, absolute_byte_offset, SEEK_SET) >= 0
		? FLAC__STREAM_DECODER_SEEK_STATUS_OK
		: FLAC__STREAM_DECODER_SEEK_STATUS_ERROR;
#endif
}

#ifdef LEGACY_FLAC
static FLAC__SeekableStreamDecoderTellStatus tell_callback (
		const FLAC__SeekableStreamDecoder *decoder ATTR_UNUSED,
		FLAC__uint64 *absolute_byte_offset, void *client_data)
#else
static FLAC__StreamDecoderTellStatus tell_callback (
		const FLAC__StreamDecoder *decoder ATTR_UNUSED,
		FLAC__uint64 *absolute_byte_offset, void *client_data)
#endif
{
	struct flac_data *data = (struct flac_data *)client_data;

	*absolute_byte_offset = io_tell (data->stream);
#ifdef LEGACY_FLAC
	return FLAC__SEEKABLE_STREAM_DECODER_TELL_STATUS_OK;
#else
	return FLAC__STREAM_DECODER_TELL_STATUS_OK;
#endif
}

#ifdef LEGACY_FLAC
static FLAC__SeekableStreamDecoderLengthStatus length_callback (
		const FLAC__SeekableStreamDecoder *decoder ATTR_UNUSED,
		FLAC__uint64 *stream_length, void *client_data)
#else
static FLAC__StreamDecoderLengthStatus length_callback (
		const FLAC__StreamDecoder *decoder ATTR_UNUSED,
		FLAC__uint64 *stream_length, void *client_data)
#endif
{
	struct flac_data *data = (struct flac_data *)client_data;

	*stream_length = io_file_size (data->stream);
#ifdef LEGACY_FLAC
	return FLAC__SEEKABLE_STREAM_DECODER_LENGTH_STATUS_OK;
#else
	return FLAC__STREAM_DECODER_LENGTH_STATUS_OK;
#endif
}

static FLAC__bool eof_callback (
#ifdef LEGACY_FLAC
		const FLAC__SeekableStreamDecoder *decoder ATTR_UNUSED,
#else
		const FLAC__StreamDecoder *decoder ATTR_UNUSED,
#endif
		void *client_data)
{
	struct flac_data *data = (struct flac_data *)client_data;

	return io_eof (data->stream);
}

static void *flac_open_internal (const char *file, const int buffered)
{
	struct flac_data *data;

	data = (struct flac_data *)xmalloc (sizeof(struct flac_data));
	decoder_error_init (&data->error);

	data->decoder = NULL;
	data->bitrate = -1;
	data->avg_bitrate = -1;
	data->abort = 0;
	data->sample_buffer_fill = 0;
	data->last_decode_position = 0;
	data->ok = 0;

	data->stream = io_open (file, buffered);
	if (!io_ok(data->stream)) {
		decoder_error (&data->error, ERROR_FATAL, 0,
				"Can't load file: %s",
				io_strerror(data->stream));
		return data;
	}

#ifdef LEGACY_FLAC
	if (!(data->decoder = FLAC__seekable_stream_decoder_new())) {
		decoder_error (&data->error, ERROR_FATAL, 0,
				"FLAC__seekable_stream_decoder_new() failed");
		return data;
	}

	FLAC__seekable_stream_decoder_set_md5_checking (data->decoder, false);

	FLAC__seekable_stream_decoder_set_metadata_ignore_all (data->decoder);
	FLAC__seekable_stream_decoder_set_metadata_respond (data->decoder,
			FLAC__METADATA_TYPE_STREAMINFO);
	FLAC__seekable_stream_decoder_set_client_data (data->decoder, data);
	FLAC__seekable_stream_decoder_set_metadata_callback (data->decoder,
			metadata_callback);
	FLAC__seekable_stream_decoder_set_write_callback (data->decoder,
			write_callback);
	FLAC__seekable_stream_decoder_set_error_callback (data->decoder,
			error_callback);
	FLAC__seekable_stream_decoder_set_read_callback (data->decoder,
			read_callback);
	FLAC__seekable_stream_decoder_set_seek_callback (data->decoder,
			seek_callback);
	FLAC__seekable_stream_decoder_set_tell_callback (data->decoder,
			tell_callback);
	FLAC__seekable_stream_decoder_set_length_callback (data->decoder,
			length_callback);
	FLAC__seekable_stream_decoder_set_eof_callback (data->decoder,
			eof_callback);

	if (FLAC__seekable_stream_decoder_init(data->decoder)
			!= FLAC__SEEKABLE_STREAM_DECODER_OK) {
		decoder_error (&data->error, ERROR_FATAL, 0,
				"FLAC__seekable_stream_decoder_init() failed");
		return data;
	}

	if (!FLAC__seekable_stream_decoder_process_until_end_of_metadata(
				data->decoder)) {
		decoder_error (&data->error, ERROR_FATAL, 0,
				"FLAC__seekable_stream_decoder_process_until_end_of_metadata()"
				" failed.");
		return data;
	}
#else
	if (!(data->decoder = FLAC__stream_decoder_new())) {
		decoder_error (&data->error, ERROR_FATAL, 0,
				"FLAC__stream_decoder_new() failed");
		return data;
	}

	FLAC__stream_decoder_set_md5_checking (data->decoder, false);

	FLAC__stream_decoder_set_metadata_ignore_all (data->decoder);
	FLAC__stream_decoder_set_metadata_respond (data->decoder,
			FLAC__METADATA_TYPE_STREAMINFO);

	if (FLAC__stream_decoder_init_stream(data->decoder, read_callback, seek_callback, tell_callback, length_callback, eof_callback, write_callback, metadata_callback, error_callback, data)
			!= FLAC__STREAM_DECODER_INIT_STATUS_OK) {
		decoder_error (&data->error, ERROR_FATAL, 0,
				"FLAC__stream_decoder_init() failed");
		return data;
	}

	if (!FLAC__stream_decoder_process_until_end_of_metadata(data->decoder)) {
		decoder_error (&data->error, ERROR_FATAL, 0,
				"FLAC__stream_decoder_process_until_end_of_metadata()"
				" failed.");
		return data;
	}
#endif

	data->ok = 1;
	data->avg_bitrate = (data->bits_per_sample) * data->sample_rate;

	return data;
}

static void *flac_open (const char *file)
{
	return flac_open_internal (file, 1);
}

static void flac_close (void *void_data)
{
	struct flac_data *data = (struct flac_data *)void_data;

	if (data->decoder) {
#ifdef LEGACY_FLAC
		FLAC__seekable_stream_decoder_finish (data->decoder);
		FLAC__seekable_stream_decoder_delete (data->decoder);
#else
		FLAC__stream_decoder_finish (data->decoder);
		FLAC__stream_decoder_delete (data->decoder);
#endif
	}

	io_close (data->stream);
	decoder_error_clear (&data->error);
	free (data);
}

static void fill_tag (FLAC__StreamMetadata_VorbisComment_Entry *comm,
		struct file_tags *tags)
{
	char *name, *value;
	FLAC__byte *eq;
	int value_length;

	eq = memchr (comm->entry, '=', comm->length);
	if (!eq)
		return;

	name = (char *)xmalloc (sizeof(char) * (eq - comm->entry + 1));
	strncpy (name, (char *)comm->entry, eq - comm->entry);
	name[eq - comm->entry] = 0;
	value_length = comm->length - (eq - comm->entry + 1);

	if (value_length == 0) {
		free (name);
		return;
	}

	value = (char *)xmalloc (sizeof(char) * (value_length + 1));
	strncpy (value, (char *)(eq + 1), value_length);
	value[value_length] = 0;

	if (!strcasecmp(name, "title"))
		tags->title = value;
	else if (!strcasecmp(name, "artist"))
		tags->artist = value;
	else if (!strcasecmp(name, "album"))
		tags->album = value;
	else if (!strcasecmp(name, "tracknumber")
			|| !strcasecmp(name, "track")) {
		tags->track = atoi (value);
		free (value);
	}
	else
		free (value);

	free (name);
}

static void get_vorbiscomments (const char *filename, struct file_tags *tags)
{
	FLAC__Metadata_SimpleIterator *iterator
		= FLAC__metadata_simple_iterator_new();
	FLAC__bool got_vorbis_comments = false;

	debug ("Reading comments for %s", filename);

	if (!iterator) {
		logit ("FLAC__metadata_simple_iterator_new() failed.");
		return;
	}

	if (!FLAC__metadata_simple_iterator_init(iterator, filename, true,
				true)) {
		logit ("FLAC__metadata_simple_iterator_init failed.");
		FLAC__metadata_simple_iterator_delete(iterator);
		return;
	}

	do {
		if (FLAC__metadata_simple_iterator_get_block_type(iterator)
				== FLAC__METADATA_TYPE_VORBIS_COMMENT) {
			FLAC__StreamMetadata *block;

			block = FLAC__metadata_simple_iterator_get_block (
					iterator);
			if (block) {
				unsigned i;
				const FLAC__StreamMetadata_VorbisComment *vc
					= &block->data.vorbis_comment;

				for (i = 0; i < vc->num_comments; i++)
					fill_tag (&vc->comments[i], tags);

				FLAC__metadata_object_delete (block);
				got_vorbis_comments = true;
			}
		}
	} while (!got_vorbis_comments
			&& FLAC__metadata_simple_iterator_next(iterator));

	FLAC__metadata_simple_iterator_delete(iterator);
}

static void flac_info (const char *file_name, struct file_tags *info,
		const int tags_sel)
{
	if (tags_sel & TAGS_TIME) {
		struct flac_data *data;

		if ((data = flac_open_internal(file_name, 0))) {
			info->time = data->length;
			flac_close (data);
		}
	}

	if (tags_sel & TAGS_COMMENTS)
		get_vorbiscomments (file_name, info);
}

static int flac_seek (void *void_data, int sec)
{
	struct flac_data *data = (struct flac_data *)void_data;
	FLAC__uint64 target_sample;

	if ((unsigned)sec > data->length)
		return -1;

	target_sample = (FLAC__uint64)((sec/(double)data->length) *
			(double)data->total_samples);

#ifdef LEGACY_FLAC
	if (FLAC__seekable_stream_decoder_seek_absolute(data->decoder,
				target_sample))
#else
	if (FLAC__stream_decoder_seek_absolute(data->decoder, target_sample))
#endif
		return sec;
	else {
#ifdef LEGACY_FLAC
		logit ("FLAC__seekable_stream_decoder_seek_absolute() failed.");
#else
		logit ("FLAC__stream_decoder_seek_absolute() failed.");
#endif
		return -1;
	}
}

static int flac_decode (void *void_data, char *buf, int buf_len,
		struct sound_params *sound_params)
{
	struct flac_data *data = (struct flac_data *)void_data;
	unsigned to_copy;
	int bytes_per_sample;
	FLAC__uint64 decode_position;

	bytes_per_sample = data->bits_per_sample / 8;

	switch (bytes_per_sample) {
		case 1:
			sound_params->fmt = SFMT_S8;
			break;
		case 2:
			sound_params->fmt = SFMT_S16 | SFMT_LE;
			break;
		case 3:
			sound_params->fmt = SFMT_S32 | SFMT_LE;
			break;
	}

	sound_params->rate = data->sample_rate;
	sound_params->channels = data->channels;

	decoder_error_clear (&data->error);

	if (!data->sample_buffer_fill) {
		debug ("decoding...");

#ifdef LEGACY_FLAC
		if (FLAC__seekable_stream_decoder_get_state(data->decoder) == FLAC__SEEKABLE_STREAM_DECODER_END_OF_STREAM)
#else
		if (FLAC__stream_decoder_get_state(data->decoder) == FLAC__STREAM_DECODER_END_OF_STREAM)
#endif
		{
			logit ("EOF");
			return 0;
		}

#ifdef LEGACY_FLAC
		if (!FLAC__seekable_stream_decoder_process_single(data->decoder))
#else
		if (!FLAC__stream_decoder_process_single(data->decoder))
#endif
		{
			decoder_error (&data->error, ERROR_FATAL, 0,
					"Read error processing frame.");
			return 0;
		}

		/* Count the bitrate */
#ifdef LEGACY_FLAC
		if(!FLAC__seekable_stream_decoder_get_decode_position(
					data->decoder, &decode_position))
#else
		if(!FLAC__stream_decoder_get_decode_position(data->decoder, &decode_position))
#endif
			decode_position = 0;
		if (decode_position > data->last_decode_position) {
			int bytes_per_sec = bytes_per_sample * data->sample_rate
				* data->channels;

			data->bitrate = (decode_position
				- data->last_decode_position) * 8.0
				/ (data->sample_buffer_fill
						/ (float)bytes_per_sec)
				/ 1000;
		}

		data->last_decode_position = decode_position;
	}
	else
		debug ("Some date remain in the buffer.");

	debug ("Decoded %d bytes", data->sample_buffer_fill);

	to_copy = MIN((unsigned)buf_len, data->sample_buffer_fill);
	memcpy (buf, data->sample_buffer, to_copy);
	memmove (data->sample_buffer, data->sample_buffer + to_copy,
			data->sample_buffer_fill - to_copy);
	data->sample_buffer_fill -= to_copy;

	return to_copy;
}

static int flac_get_bitrate (void *void_data)
{
	struct flac_data *data = (struct flac_data *)void_data;

	return data->bitrate;
}

static int flac_get_avg_bitrate (void *void_data)
{
	struct flac_data *data = (struct flac_data *)void_data;

	return data->avg_bitrate / 1000;
}

static int flac_get_duration (void *void_data)
{
	struct flac_data *data = (struct flac_data *)void_data;

	return data->length;
}

static void flac_get_name (const char *file ATTR_UNUSED, char buf[4])
{
	strcpy (buf, "FLA");
}

static int flac_our_format_ext (const char *ext)
{
	return !strcasecmp (ext, "flac") || !strcasecmp (ext, "fla");
}

static int flac_our_format_mime (const char *mime)
{
	return !strcasecmp (mime, "audio/flac") ||
	       !strncasecmp (mime, "audio/flac;", 11) ||
	       !strcasecmp (mime, "audio/x-flac") ||
	       !strncasecmp (mime, "audio/x-flac;", 13);
}

static void flac_get_error (void *prv_data, struct decoder_error *error)
{
	struct flac_data *data = (struct flac_data *)prv_data;

	decoder_error_copy (error, &data->error);
}

static struct decoder flac_decoder = {
	DECODER_API_VERSION,
	NULL,
	NULL,
	flac_open,
	NULL,
	NULL,
	flac_close,
	flac_decode,
	flac_seek,
	flac_info,
	flac_get_bitrate,
	flac_get_duration,
	flac_get_error,
	flac_our_format_ext,
	flac_our_format_mime,
	flac_get_name,
	NULL,
	NULL,
	flac_get_avg_bitrate
};

struct decoder *plugin_init ()
{
	return &flac_decoder;
}
