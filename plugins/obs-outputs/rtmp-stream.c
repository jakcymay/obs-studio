/******************************************************************************
    Copyright (C) 2014 by Hugh Bailey <obs.jim@gmail.com>

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
******************************************************************************/

#include <obs-module.h>
#include <obs-avc.h>
#include <util/platform.h>
#include <util/circlebuf.h>
#include <util/dstr.h>
#include <util/threading.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>


#define do_log(level, format, ...) \
	blog(level, "[rtmp stream: '%s'] " format, \
			obs_output_get_name(stream->output), ##__VA_ARGS__)

#define warn(format, ...)  do_log(LOG_WARNING, format, ##__VA_ARGS__)
#define info(format, ...)  do_log(LOG_INFO,    format, ##__VA_ARGS__)
#define debug(format, ...) do_log(LOG_DEBUG,   format, ##__VA_ARGS__)

#define OPT_DROP_THRESHOLD "drop_threshold_ms"
#define OPT_PFRAME_DROP_THRESHOLD "pframe_drop_threshold_ms"
#define OPT_MAX_SHUTDOWN_TIME_SEC "max_shutdown_time_sec"
#define OPT_BIND_IP "bind_ip"

//#define TEST_FRAMEDROPS

#ifdef TEST_FRAMEDROPS

#define DROPTEST_MAX_KBPS 3000
#define DROPTEST_MAX_BYTES (DROPTEST_MAX_KBPS * 1000 / 8)

struct droptest_info {
	uint64_t ts;
	size_t size;
};
#endif

struct ffmpeg_cfg {
	const char         *url;
	const char         *format_name;
	const char         *format_mime_type;
	const char         *muxer_settings;
	int                video_bitrate;
	int                audio_bitrate;
	const char         *video_encoder;
	int                video_encoder_id;
	const char         *audio_encoder;
	int                audio_encoder_id;
	const char         *video_settings;
	const char         *audio_settings;
	enum AVPixelFormat format;
	enum AVColorRange  color_range;
	enum AVColorSpace  color_space;
	int                scale_width;
	int                scale_height;
	int                width;
	int                height;
};

struct ffmpeg_data {
	AVStream           *video;
	AVStream           *audio;
	AVCodec            *acodec;
	AVCodec            *vcodec;
	AVFormatContext    *output;
	struct SwsContext  *swscale;

	int64_t            total_frames;
	AVPicture          dst_picture;
	AVFrame            *vframe;
	int                frame_size;

	uint64_t           start_timestamp;

	int64_t            total_samples;
	uint32_t           audio_samplerate;
	enum audio_format  audio_format;
	size_t             audio_planes;
	size_t             audio_size;
	struct circlebuf   excess_frames[MAX_AV_PLANES];
	uint8_t            *samples[MAX_AV_PLANES];
	AVFrame            *aframe;

	struct ffmpeg_cfg  config;

	bool               initialized;
};

struct rtmp_stream {
	obs_output_t     *output;

	pthread_mutex_t  packets_mutex;
	struct circlebuf packets;
	bool             sent_headers;

	volatile bool    connecting;
	pthread_t        connect_thread;

	volatile bool    active;
	volatile bool    disconnected;
	pthread_t        send_thread;

	int              max_shutdown_time_sec;

	os_sem_t         *send_sem;
	os_event_t       *stop_event;
	uint64_t         stop_ts;
	uint64_t         shutdown_timeout_ts;

	struct dstr      path, key;
	struct dstr      username, password;
	struct dstr      encoder_name;
	struct dstr      bind_ip;

	/* frame drop variables */
	int64_t          drop_threshold_usec;
	int64_t          min_drop_dts_usec;
	int64_t          pframe_drop_threshold_usec;
	int64_t          pframe_min_drop_dts_usec;
	int              min_priority;

	int64_t          last_dts_usec;

	uint64_t         total_bytes_sent;
	int              dropped_frames;

#ifdef TEST_FRAMEDROPS
	struct circlebuf droptest_info;
	size_t           droptest_size;
#endif
	struct ffmpeg_data ff_data;
};

static const char *rtmp_stream_getname(void *unused)
{
	UNUSED_PARAMETER(unused);
	return obs_module_text("RTMPStream");
}

static inline size_t num_buffered_packets(struct rtmp_stream *stream);

static inline void free_packets(struct rtmp_stream *stream)
{
	size_t num_packets;

	pthread_mutex_lock(&stream->packets_mutex);

	num_packets = num_buffered_packets(stream);
	if (num_packets)
		info("Freeing %d remaining packets", (int)num_packets);

	while (stream->packets.size) {
		struct encoder_packet packet;
		circlebuf_pop_front(&stream->packets, &packet, sizeof(packet));
		obs_encoder_packet_release(&packet);
	}
	pthread_mutex_unlock(&stream->packets_mutex);
}

static inline bool stopping(struct rtmp_stream *stream)
{
	return os_event_try(stream->stop_event) != EAGAIN;
}

static inline bool connecting(struct rtmp_stream *stream)
{
	return os_atomic_load_bool(&stream->connecting);
}

static inline bool active(struct rtmp_stream *stream)
{
	return os_atomic_load_bool(&stream->active);
}

static inline bool disconnected(struct rtmp_stream *stream)
{
	return os_atomic_load_bool(&stream->disconnected);
}

static void rtmp_stream_destroy(void *data)
{
	struct rtmp_stream *stream = data;

	if (stopping(stream) && !connecting(stream)) {
		pthread_join(stream->send_thread, NULL);

	} else if (connecting(stream) || active(stream)) {
		if (stream->connecting)
			pthread_join(stream->connect_thread, NULL);

		stream->stop_ts = 0;
		os_event_signal(stream->stop_event);

		if (active(stream)) {
			os_sem_post(stream->send_sem);
			obs_output_end_data_capture(stream->output);
			pthread_join(stream->send_thread, NULL);
		}
	}

	if (stream) {
		free_packets(stream);
		dstr_free(&stream->path);
		dstr_free(&stream->key);
		dstr_free(&stream->username);
		dstr_free(&stream->password);
		dstr_free(&stream->encoder_name);
		dstr_free(&stream->bind_ip);
		os_event_destroy(stream->stop_event);
		os_sem_destroy(stream->send_sem);
		pthread_mutex_destroy(&stream->packets_mutex);
		circlebuf_free(&stream->packets);
#ifdef TEST_FRAMEDROPS
		circlebuf_free(&stream->droptest_info);
#endif
		bfree(stream);
	}
}

static void *rtmp_stream_create(obs_data_t *settings, obs_output_t *output)
{
	struct rtmp_stream *stream = bzalloc(sizeof(struct rtmp_stream));
	stream->output = output;
	pthread_mutex_init_value(&stream->packets_mutex);

// 	RTMP_Init(&stream->rtmp);
// 	RTMP_LogSetCallback(log_rtmp);
// 	RTMP_LogSetLevel(RTMP_LOGWARNING);

	if (pthread_mutex_init(&stream->packets_mutex, NULL) != 0)
		goto fail;
	if (os_event_init(&stream->stop_event, OS_EVENT_TYPE_MANUAL) != 0)
		goto fail;

	UNUSED_PARAMETER(settings);
	return stream;

fail:
	rtmp_stream_destroy(stream);
	return NULL;
}

static void rtmp_stream_stop(void *data, uint64_t ts)
{
	struct rtmp_stream *stream = data;

	if (stopping(stream) && ts != 0)
		return;

	if (connecting(stream))
		pthread_join(stream->connect_thread, NULL);

	stream->stop_ts = ts / 1000ULL;
	os_event_signal(stream->stop_event);

	if (ts)
		stream->shutdown_timeout_ts = ts +
			(uint64_t)stream->max_shutdown_time_sec * 1000000000ULL;

	if (active(stream)) {
		if (stream->stop_ts == 0)
			os_sem_post(stream->send_sem);
	}
}

static inline bool get_next_packet(struct rtmp_stream *stream,
		struct encoder_packet *packet)
{
	bool new_packet = false;

	pthread_mutex_lock(&stream->packets_mutex);
	if (stream->packets.size) {
		circlebuf_pop_front(&stream->packets, packet,
				sizeof(struct encoder_packet));
		new_packet = true;
	}
	pthread_mutex_unlock(&stream->packets_mutex);

	return new_packet;
}

#ifdef TEST_FRAMEDROPS
static void droptest_cap_data_rate(struct rtmp_stream *stream, size_t size)
{
	uint64_t ts = os_gettime_ns();
	struct droptest_info info;

	info.ts = ts;
	info.size = size;

	circlebuf_push_back(&stream->droptest_info, &info, sizeof(info));
	stream->droptest_size += size;

	if (stream->droptest_info.size) {
		circlebuf_peek_front(&stream->droptest_info,
				&info, sizeof(info));

		if (stream->droptest_size > DROPTEST_MAX_BYTES) {
			uint64_t elapsed = ts - info.ts;

			if (elapsed < 1000000000ULL) {
				elapsed = 1000000000ULL - elapsed;
				os_sleepto_ns(ts + elapsed);
			}

			while (stream->droptest_size > DROPTEST_MAX_BYTES) {
				circlebuf_pop_front(&stream->droptest_info,
						&info, sizeof(info));
				stream->droptest_size -= info.size;
			}
		}
	}
}
#endif

static int send_packet(struct rtmp_stream *stream,
		struct encoder_packet *packet, bool is_header, size_t idx)
{
	int ret;
	uint8_t *data;
	size_t  size;

	/* add process packet*/

	obs_encoder_packet_release(packet);

	stream->total_bytes_sent += size;
	return ret;
}

static inline bool can_shutdown_stream(struct rtmp_stream *stream,
		struct encoder_packet *packet)
{
	uint64_t cur_time = os_gettime_ns();
	bool timeout = cur_time >= stream->shutdown_timeout_ts;

	if (timeout)
		info("Stream shutdown timeout reached (%d second(s))",
				stream->max_shutdown_time_sec);

	return timeout || packet->sys_dts_usec >= (int64_t)stream->stop_ts;
}

// static int process_packet(struct ffmpeg_output *output)
// {
// 	AVPacket packet;
// 	bool new_packet = false;
// 	int ret;
// 
// 	pthread_mutex_lock(&output->write_mutex);
// 	if (output->packets.num) {
// 		packet = output->packets.array[0];
// 		da_erase(output->packets, 0);
// 		new_packet = true;
// 	}
// 	pthread_mutex_unlock(&output->write_mutex);
// 
// 	if (!new_packet)
// 		return 0;
// 
// 	/*blog(LOG_DEBUG, "size = %d, flags = %lX, stream = %d, "
// 	"packets queued: %lu",
// 	packet.size, packet.flags,
// 	packet.stream_index, output->packets.num);*/
// 
// 	if (stopping(output)) {
// 		uint64_t sys_ts = get_packet_sys_dts(output, &packet);
// 		if (sys_ts >= output->stop_ts) {
// 			ffmpeg_output_full_stop(output);
// 			return 0;
// 		}
// 	}
// 
// 	ret = av_interleaved_write_frame(output->ff_data.output, &packet);
// 	if (ret < 0) {
// 		av_free_packet(&packet);
// 		blog(LOG_WARNING, "receive_audio: Error writing packet: %s",
// 			av_err2str(ret));
// 		return ret;
// 	}
// 
// 	return 0;
// }
static void *send_thread(void *data)
{
	struct rtmp_stream *stream = data;

	os_set_thread_name("rtmp-stream: send_thread");

	while (os_sem_wait(stream->send_sem) == 0) {
		struct encoder_packet packet;

		if (stopping(stream) && stream->stop_ts == 0) {
			break;
		}

		if (!get_next_packet(stream, &packet))
			continue;

		if (stopping(stream)) {
			if (can_shutdown_stream(stream, &packet)) {
				obs_encoder_packet_release(&packet);
				break;
			}
		}

		if (send_packet(stream, &packet, false, packet.track_idx) < 0) {
			os_atomic_set_bool(&stream->disconnected, true);
			break;
		}
	}

	if (disconnected(stream)) {
		info("Disconnected from %s", stream->path.array);
	} else {
		info("User stopped the stream");
	}

// 	RTMP_Close(&stream->rtmp);

	if (!stopping(stream)) {
		pthread_detach(stream->send_thread);
		obs_output_signal_stop(stream->output, OBS_OUTPUT_DISCONNECTED);
	} else {
		obs_output_end_data_capture(stream->output);
	}

	free_packets(stream);
	os_event_reset(stream->stop_event);
	os_atomic_set_bool(&stream->active, false);
	stream->sent_headers = false;
	return NULL;
}

static inline bool reset_semaphore(struct rtmp_stream *stream)
{
	os_sem_destroy(stream->send_sem);
	return os_sem_init(&stream->send_sem, 0) == 0;
}


static inline bool open_output_file(struct ffmpeg_data *data)
{
	AVOutputFormat *format = data->output->oformat;
	int ret;

	if ((format->flags & AVFMT_NOFILE) == 0) {
		ret = avio_open(&data->output->pb, data->config.url,
			AVIO_FLAG_WRITE);
		if (ret < 0) {
			blog(LOG_WARNING, "Couldn't open '%s', %s",
				data->config.url, av_err2str(ret));
			return false;
		}
	}

	strncpy(data->output->filename, data->config.url,
		sizeof(data->output->filename));
	data->output->filename[sizeof(data->output->filename) - 1] = 0;

	AVDictionary *dict = NULL;
	if ((ret = av_dict_parse_string(&dict, data->config.muxer_settings,
		"=", " ", 0))) {
		blog(LOG_WARNING, "Failed to parse muxer settings: %s\n%s",
			av_err2str(ret), data->config.muxer_settings);

		av_dict_free(&dict);
		return false;
	}

	if (av_dict_count(dict) > 0) {
		struct dstr str = { 0 };

		AVDictionaryEntry *entry = NULL;
		while ((entry = av_dict_get(dict, "", entry,
			AV_DICT_IGNORE_SUFFIX)))
			dstr_catf(&str, "\n\t%s=%s", entry->key, entry->value);

		blog(LOG_INFO, "Using muxer settings:%s", str.array);
		dstr_free(&str);
	}

	ret = avformat_write_header(data->output, &dict);
	if (ret < 0) {
		blog(LOG_WARNING, "Error opening '%s': %s",
			data->config.url, av_err2str(ret));
		return false;
	}

	av_dict_free(&dict);

	return true;
}

static int init_send(struct rtmp_stream *stream)
{
	int ret;
	size_t idx = 0;
	bool next = true;

	reset_semaphore(stream);

	if (!open_output_file(&stream->ff_data))
		return OBS_OUTPUT_ERROR;

	ret = pthread_create(&stream->send_thread, NULL, send_thread, stream);
	if (ret != 0) {
		/*RTMP_Close(&stream->rtmp);*/
		warn("Failed to create send thread");
		return OBS_OUTPUT_ERROR;
	}

	os_atomic_set_bool(&stream->active, true);
	obs_output_begin_data_capture(stream->output, 0);

	return OBS_OUTPUT_SUCCESS;
}

static bool ffmpeg_data_init(struct ffmpeg_data *data,
	struct ffmpeg_cfg *config)
{
	bool is_rtmp = false;

	memset(data, 0, sizeof(struct ffmpeg_data));
	data->config = *config;

	if (!config->url || !*config->url)
		return false;

	av_register_all();
	avformat_network_init();

	is_rtmp = (astrcmpi_n(config->url, "rtmp://", 7) == 0);

	AVOutputFormat *output_format = av_guess_format(
		is_rtmp ? "flv" : data->config.format_name,
		data->config.url,
		is_rtmp ? NULL : data->config.format_mime_type);

	if (output_format == NULL) {
		blog(LOG_WARNING, "Couldn't find matching output format with "
			" parameters: name=%s, url=%s, mime=%s",
			safe_str(is_rtmp ?
			"flv" : data->config.format_name),
			safe_str(data->config.url),
			safe_str(is_rtmp ?
		NULL : data->config.format_mime_type));
		goto fail;
	}

	avformat_alloc_output_context2(&data->output, output_format,
		NULL, NULL);

	if (is_rtmp) {
		data->output->oformat->video_codec = AV_CODEC_ID_H264;
		data->output->oformat->audio_codec = AV_CODEC_ID_AAC;
	}
	else {
		if (data->config.format_name)
			set_encoder_ids(data);
	}

	if (!data->output) {
		blog(LOG_WARNING, "Couldn't create avformat context");
		goto fail;
	}

	av_dump_format(data->output, 0, NULL, 1);

	data->initialized = true;
	return true;

fail:
	blog(LOG_WARNING, "ffmpeg_data_init failed");
	ffmpeg_data_free(data);
	return false;
}

static int try_connect(struct rtmp_stream *stream)
{
	bool success;

	if (dstr_is_empty(&stream->path)) {
		warn("URL is empty");
		return OBS_OUTPUT_BAD_PATH;
	}

	info("Connecting to RTMP URL %s...", stream->path.array);

	struct ffmpeg_cfg config;
	success = ffmpeg_data_init(&stream->ff_data, &config);

	if (!success)
		return OBS_OUTPUT_INVALID_STREAM;

	info("Connection to %s successful", stream->path.array);

	return init_send(stream);
}

static bool init_connect(struct rtmp_stream *stream)
{
	obs_service_t *service;
	obs_data_t *settings;
	const char *bind_ip;
	int64_t drop_p;
	int64_t drop_b;

	if (stopping(stream))
		pthread_join(stream->send_thread, NULL);

	free_packets(stream);

	service = obs_output_get_service(stream->output);
	if (!service)
		return false;

	os_atomic_set_bool(&stream->disconnected, false);
	stream->total_bytes_sent = 0;
	stream->dropped_frames   = 0;
	stream->min_drop_dts_usec= 0;
	stream->min_priority     = 0;

	settings = obs_output_get_settings(stream->output);
	dstr_copy(&stream->path,     obs_service_get_url(service));
	dstr_copy(&stream->key,      obs_service_get_key(service));
	dstr_copy(&stream->username, obs_service_get_username(service));
	dstr_copy(&stream->password, obs_service_get_password(service));
	dstr_depad(&stream->path);
	dstr_depad(&stream->key);
	drop_b = (int64_t)obs_data_get_int(settings, OPT_DROP_THRESHOLD);
	drop_p = (int64_t)obs_data_get_int(settings, OPT_PFRAME_DROP_THRESHOLD);
	stream->max_shutdown_time_sec =
		(int)obs_data_get_int(settings, OPT_MAX_SHUTDOWN_TIME_SEC);

	if (drop_p < (drop_b + 200))
		drop_p = drop_b + 200;

	stream->drop_threshold_usec = 1000 * drop_b;
	stream->pframe_drop_threshold_usec = 1000 * drop_p;

	bind_ip = obs_data_get_string(settings, OPT_BIND_IP);
	dstr_copy(&stream->bind_ip, bind_ip);

	obs_data_release(settings);
	return true;
}

static void *connect_thread(void *data)
{
	struct rtmp_stream *stream = data;
	int ret;

	os_set_thread_name("rtmp-stream: connect_thread");

	if (!init_connect(stream)) {
		obs_output_signal_stop(stream->output, OBS_OUTPUT_BAD_PATH);
		return NULL;
	}

	ret = try_connect(stream);

	if (ret != OBS_OUTPUT_SUCCESS) {
		obs_output_signal_stop(stream->output, ret);
		info("Connection to %s failed: %d", stream->path.array, ret);
	}

	if (!stopping(stream))
		pthread_detach(stream->connect_thread);

	os_atomic_set_bool(&stream->connecting, false);
	return NULL;
}

static bool rtmp_stream_start(void *data)
{
	struct rtmp_stream *stream = data;

	if (!obs_output_can_begin_data_capture(stream->output, 0))
		return false;
	if (!obs_output_initialize_encoders(stream->output, 0))
		return false;

	os_atomic_set_bool(&stream->connecting, true);
	return pthread_create(&stream->connect_thread, NULL, connect_thread,
			stream) == 0;
}

static inline bool add_packet(struct rtmp_stream *stream,
		struct encoder_packet *packet)
{
	circlebuf_push_back(&stream->packets, packet,
			sizeof(struct encoder_packet));
	stream->last_dts_usec = packet->dts_usec;
	return true;
}

static inline size_t num_buffered_packets(struct rtmp_stream *stream)
{
	return stream->packets.size / sizeof(struct encoder_packet);
}

static void drop_frames(struct rtmp_stream *stream, const char *name,
		int highest_priority, int64_t *p_min_dts_usec)
{
	struct circlebuf new_buf            = {0};
	uint64_t         last_drop_dts_usec = 0;
	int              num_frames_dropped = 0;

#ifdef _DEBUG
	int start_packets = (int)num_buffered_packets(stream);
#else
	UNUSED_PARAMETER(name);
#endif

	circlebuf_reserve(&new_buf, sizeof(struct encoder_packet) * 8);

	while (stream->packets.size) {
		struct encoder_packet packet;
		circlebuf_pop_front(&stream->packets, &packet, sizeof(packet));

		last_drop_dts_usec = packet.dts_usec;

		/* do not drop audio data or video keyframes */
		if (packet.type          == OBS_ENCODER_AUDIO ||
		    packet.drop_priority >= highest_priority) {
			circlebuf_push_back(&new_buf, &packet, sizeof(packet));

		} else {
			num_frames_dropped++;
			obs_encoder_packet_release(&packet);
		}
	}

	circlebuf_free(&stream->packets);
	stream->packets = new_buf;

	if (stream->min_priority < highest_priority)
		stream->min_priority = highest_priority;

	*p_min_dts_usec = last_drop_dts_usec;

	stream->dropped_frames += num_frames_dropped;
#ifdef _DEBUG
	debug("Dropped %s, prev packet count: %d, new packet count: %d",
			name,
			start_packets,
			(int)num_buffered_packets(stream));
#endif
}

static void check_to_drop_frames(struct rtmp_stream *stream, bool pframes)
{
	struct encoder_packet first;
	int64_t buffer_duration_usec;
	size_t num_packets = num_buffered_packets(stream);
	const char *name = pframes ? "p-frames" : "b-frames";
	int priority = pframes ?
		OBS_NAL_PRIORITY_HIGHEST : OBS_NAL_PRIORITY_HIGH;
	int64_t *p_min_dts_usec = pframes ?
		&stream->pframe_min_drop_dts_usec :
		&stream->min_drop_dts_usec;
	int64_t drop_threshold = pframes ?
		stream->pframe_drop_threshold_usec :
		stream->drop_threshold_usec;

	if (num_packets < 5)
		return;

	circlebuf_peek_front(&stream->packets, &first, sizeof(first));

	/* do not drop frames if frames were just dropped within this time */
	if (first.dts_usec < *p_min_dts_usec)
		return;

	/* if the amount of time stored in the buffered packets waiting to be
	 * sent is higher than threshold, drop frames */
	buffer_duration_usec = stream->last_dts_usec - first.dts_usec;

	if (buffer_duration_usec > drop_threshold) {
		debug("buffer_duration_usec: %" PRId64, buffer_duration_usec);
		drop_frames(stream, name, priority, p_min_dts_usec);
	}
}

static bool add_video_packet(struct rtmp_stream *stream,
		struct encoder_packet *packet)
{
	check_to_drop_frames(stream, false);
	check_to_drop_frames(stream, true);

	/* if currently dropping frames, drop packets until it reaches the
	 * desired priority */
	if (packet->drop_priority < stream->min_priority) {
		stream->dropped_frames++;
		return false;
	} else {
		stream->min_priority = 0;
	}

	return add_packet(stream, packet);
}

static void rtmp_stream_data(void *data, struct encoder_packet *packet)
{
	struct rtmp_stream    *stream = data;
	struct encoder_packet new_packet;
	bool                  added_packet = false;

	if (disconnected(stream) || !active(stream))
		return;

	if (packet->type == OBS_ENCODER_VIDEO)
		obs_parse_avc_packet(&new_packet, packet);
	else
		obs_encoder_packet_ref(&new_packet, packet);

	pthread_mutex_lock(&stream->packets_mutex);

	if (!disconnected(stream)) {
		added_packet = (packet->type == OBS_ENCODER_VIDEO) ?
			add_video_packet(stream, &new_packet) :
			add_packet(stream, &new_packet);
	}

	pthread_mutex_unlock(&stream->packets_mutex);

	if (added_packet)
		os_sem_post(stream->send_sem);
	else
		obs_encoder_packet_release(&new_packet);
}

static void rtmp_stream_defaults(obs_data_t *defaults)
{
	obs_data_set_default_int(defaults, OPT_DROP_THRESHOLD, 500);
	obs_data_set_default_int(defaults, OPT_PFRAME_DROP_THRESHOLD, 800);
	obs_data_set_default_int(defaults, OPT_MAX_SHUTDOWN_TIME_SEC, 30);
	obs_data_set_default_string(defaults, OPT_BIND_IP, "default");
}

static obs_properties_t *rtmp_stream_properties(void *unused)
{
	UNUSED_PARAMETER(unused);

	obs_properties_t *props = obs_properties_create();
// 	struct netif_saddr_data addrs = {0};
// 	obs_property_t *p;
// 
// 	obs_properties_add_int(props, OPT_DROP_THRESHOLD,
// 			obs_module_text("RTMPStream.DropThreshold"),
// 			200, 10000, 100);
// 
// 	p = obs_properties_add_list(props, OPT_BIND_IP,
// 			obs_module_text("RTMPStream.BindIP"),
// 			OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
// 
// 	obs_property_list_add_string(p, obs_module_text("Default"), "default");
// 
// 	netif_get_addrs(&addrs);
// 	for (size_t i = 0; i < addrs.addrs.num; i++) {
// 		struct netif_saddr_item item = addrs.addrs.array[i];
// 		obs_property_list_add_string(p, item.name, item.addr);
// 	}
// 	netif_saddr_data_free(&addrs);

	return props;
}

static uint64_t rtmp_stream_total_bytes_sent(void *data)
{
	struct rtmp_stream *stream = data;
	return stream->total_bytes_sent;
}

static int rtmp_stream_dropped_frames(void *data)
{
	struct rtmp_stream *stream = data;
	return stream->dropped_frames;
}

struct obs_output_info rtmp_output_info = {
	.id                 = "rtmp_output",
	.flags              = OBS_OUTPUT_AV |
	                      OBS_OUTPUT_ENCODED |
	                      OBS_OUTPUT_SERVICE |
	                      OBS_OUTPUT_MULTI_TRACK,
	.get_name           = rtmp_stream_getname,
	.create             = rtmp_stream_create,
	.destroy            = rtmp_stream_destroy,
	.start              = rtmp_stream_start,
	.stop               = rtmp_stream_stop,
	.encoded_packet     = rtmp_stream_data,
	.get_defaults       = rtmp_stream_defaults,
	.get_properties     = rtmp_stream_properties,
	.get_total_bytes    = rtmp_stream_total_bytes_sent,
	.get_dropped_frames = rtmp_stream_dropped_frames
};
