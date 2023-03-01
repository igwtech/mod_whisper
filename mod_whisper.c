/*
 * FreeSWITCH Modular Media Switching Software Library / Soft-Switch Application
 * Copyright (C) 2005-2021, Anthony Minessale II <anthm@freeswitch.org>
 *
 * Version: MPL 1.1
 *
 * The contents of this file are subject to the Mozilla Public License Version
 * 1.1 (the "License"); you may not use this file except in compliance with
 * the License. You may obtain a copy of the License at
 * http://www.mozilla.org/MPL/
 *
 * Software distributed under the License is distributed on an "AS IS" basis,
 * WITHOUT WARRANTY OF ANY KIND, either express or implied. See the License
 * for the specific language governing rights and limitations under the
 * License.
 *
 * The Original Code is FreeSWITCH Modular Media Switching Software Library / Soft-Switch Application
 *
 * The Initial Developer of the Original Code is
 * Anthony Minessale II <anthm@freeswitch.org>
 * Portions created by the Initial Developer are Copyright (C)
 * the Initial Developer. All Rights Reserved.
 *
 * Contributor(s):
 * Chris Rienzo <chris@signalwire.com>
 * Seven Du <dujinfang@gmail.com>
 * Ghulam Mustafa <mustafa.pk@gmail.com>
 *
 *
 * mod_whisper.c -- a general purpose module for trancribing audio using websockets
 *
 */

#include <switch.h>
#include <netinet/tcp.h>
#include <libks/ks.h>

#define AUDIO_BLOCK_SIZE 3200

SWITCH_MODULE_LOAD_FUNCTION(mod_whisper_load);
SWITCH_MODULE_SHUTDOWN_FUNCTION(mod_whisper_shutdown);
SWITCH_MODULE_RUNTIME_FUNCTION(mod_whisper_runtime);
SWITCH_MODULE_DEFINITION(mod_whisper, mod_whisper_load, mod_whisper_shutdown, mod_whisper_runtime);


typedef enum {
	ASRFLAG_READY = (1 << 0),
	ASRFLAG_INPUT_TIMERS = (1 << 1),
	ASRFLAG_START_OF_SPEECH = (1 << 2),
	ASRFLAG_RETURNED_START_OF_SPEECH = (1 << 3),
	ASRFLAG_NOINPUT_TIMEOUT = (1 << 4),
	ASRFLAG_RESULT = (1 << 5),
	ASRFLAG_RETURNED_RESULT = (1 << 6),
	ASRFLAG_TIMEOUT = (1 << 7)
} whisper_flag_t;

typedef struct {
	uint32_t flags;
	char *result_text;
	double result_confidence;
	uint32_t thresh;
	uint32_t silence_ms;
	uint32_t voice_ms;
	int no_input_timeout;
	int speech_timeout;
	switch_bool_t start_input_timers;
	switch_time_t no_input_time;
	switch_time_t speech_time;
	char *grammar;
	char *channel_uuid;
	switch_vad_t *vad;
	switch_buffer_t *audio_buffer;
	switch_mutex_t *mutex;
	kws_t *ws;
	int partial;
} whisper_t;

static switch_mutex_t *MUTEX = NULL;
static switch_event_node_t *NODE = NULL;


static struct {
	char *server_url;
	int return_json;

	int auto_reload;
	switch_memory_pool_t *pool;
	ks_pool_t *ks_pool;
} globals;

static void whisper_reset(whisper_t *context)
{
	if (context->vad) {
		switch_vad_reset(context->vad);
	}
	context->flags = 0;
	context->result_text = "agent";
	context->result_confidence = 87.3;
	switch_set_flag(context, ASRFLAG_READY);
	context->no_input_time = switch_micro_time_now();
	if (context->start_input_timers) {
		switch_set_flag(context, ASRFLAG_INPUT_TIMERS);
	}
}

static switch_status_t whisper_open(switch_asr_handle_t *ah, const char *codec, int rate, const char *dest, switch_asr_flag_t *flags)
{
	whisper_t *context;
	ks_json_t *req = ks_json_create_object();

//
//	char request_string[464] = "{\"context\": {\"protocol_version\": 6003, \"timestamp\": 0.0, \"buffer_tokens\": []"
//			", \"buffer_mel\": null, \"nosoeech_skip_count\": null, \"temperatures\": [0.0, 0.2, 0.4, 0.6, 0.8, 1.0]"
//			", \"patience\": null, \"compression_ratio_threshold\": 2.4, \"logprob_threshold\": -1.0, \"no_captions_threshold\": 0.6"
//			", \"best_of\": 5, \"beam_size\": 5, \"no_speech_threshold\": 0.6, \"buffer_threshold\": 0.5, \"vad_threshold\": 0.0, \"max_nospeech_skip\": 16"
//			", \"mel_frame_min_num\": 1, \"data_type\": \"float32\"}}";
//
//
//


	ks_json_add_string_to_object(req, "url", (dest ? dest : globals.server_url));


	if (switch_test_flag(ah, SWITCH_ASR_FLAG_CLOSED)) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "asr_open attempt on CLOSED asr handle\n");
		return SWITCH_STATUS_FALSE;
	}

	if (!(context = (whisper_t *) switch_core_alloc(ah->memory_pool, sizeof(*context)))) {
		return SWITCH_STATUS_MEMERR;
	}

	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "codec = %s, rate = %d, dest = %s\n", codec, rate, dest);

	ah->private_info = context;
	codec = "L16";
	ah->codec = switch_core_strdup(ah->memory_pool, codec);

	if (rate > 16000) {
		ah->native_rate = 16000;
	}

	context->thresh = 400;
	context->silence_ms = 700;
	context->voice_ms = 60;
	context->start_input_timers = 1;
	context->no_input_timeout = 5000;
	context->speech_timeout = 10000;

	context->vad = switch_vad_init(ah->native_rate, 1);
	switch_vad_set_mode(context->vad, -1);
	switch_vad_set_param(context->vad, "thresh", context->thresh);
	switch_vad_set_param(context->vad, "silence_ms", context->silence_ms);
	switch_vad_set_param(context->vad, "voice_ms", context->voice_ms);
	switch_vad_set_param(context->vad, "debug", 1);



	switch_mutex_init(&context->mutex, SWITCH_MUTEX_NESTED, ah->memory_pool);

	if (switch_buffer_create_dynamic(&context->audio_buffer, AUDIO_BLOCK_SIZE, AUDIO_BLOCK_SIZE, 0) != SWITCH_STATUS_SUCCESS) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Buffer create failed\n");
		return SWITCH_STATUS_MEMERR;
	}

	if (kws_connect_ex(&context->ws, req, KWS_BLOCK | KWS_CLOSE_SOCK, globals.ks_pool, NULL, 30000) != KS_STATUS_SUCCESS) {
		ks_json_delete(&req);
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Websocket connect to %s failed\n", globals.server_url);
		return SWITCH_STATUS_GENERR;
	}
	ks_json_delete(&req);


	// send parameters

//	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "%s\n", request_string);
//
//	if (kws_write_frame(context->ws, WSOC_TEXT, request_string, sizeof request_string) < 0) {
//		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Unable to send INIT string");
//
//		switch_mutex_unlock(context->mutex);
//		return SWITCH_STATUS_BREAK;
//	}

	//end send parameters


//	if (context) {
//		switch_core_session_t *session = switch_core_session_locate(context->channel_uuid);
//		switch_channel_t *channel = switch_core_session_get_channel(session);
//		const char *callerid = switch_channel_get_variable(channel, "destination_number");
//		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "caller destination is %s", callerid);
//
//	}

	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "ASR opened\n");

	whisper_reset(context);

	return SWITCH_STATUS_SUCCESS;
}

static switch_status_t whisper_load_grammar(switch_asr_handle_t *ah, const char *grammar, const char *name)
{
	whisper_t *context = (whisper_t *)ah->private_info;

	if (switch_test_flag(ah, SWITCH_ASR_FLAG_CLOSED)) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "asr_open attempt on CLOSED asr handle\n");
		return SWITCH_STATUS_FALSE;
	}

	switch_log_printf(SWITCH_CHANNEL_UUID_LOG(context->channel_uuid), SWITCH_LOG_DEBUG, "load grammar %s\n", grammar);
	context->grammar = switch_core_strdup(ah->memory_pool, grammar);
	return SWITCH_STATUS_SUCCESS;
}

static switch_status_t whisper_unload_grammar(switch_asr_handle_t *ah, const char *name)
{
	return SWITCH_STATUS_SUCCESS;
}

static switch_status_t whisper_close(switch_asr_handle_t *ah, switch_asr_flag_t *flags)
{
	whisper_t *context = (whisper_t *)ah->private_info;
	switch_status_t status = SWITCH_STATUS_SUCCESS;

	switch_mutex_lock(context->mutex);

	/** FIXME: websockets server still expects us to read the close confirmation and only then close
	    libks library doens't implement it yet. */
	kws_close(context->ws, KWS_CLOSE_SOCK);
	kws_destroy(&context->ws);

	switch_set_flag(ah, SWITCH_ASR_FLAG_CLOSED);
	switch_buffer_destroy(&context->audio_buffer);
	switch_mutex_unlock(context->mutex);


	if (switch_test_flag(ah, SWITCH_ASR_FLAG_CLOSED)) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "Double ASR close!\n");
		return SWITCH_STATUS_FALSE;
	}

	switch_log_printf(SWITCH_CHANNEL_UUID_LOG(context->channel_uuid), SWITCH_LOG_DEBUG, "ASR WS func exiting ...\n");

	if (context->vad) {
		switch_vad_destroy(&context->vad);
	}

	switch_set_flag(ah, SWITCH_ASR_FLAG_CLOSED);
	return status;
}

static switch_status_t whisper_send_final_bit(whisper_t *context)
{
	int poll_result;
	kws_opcode_t oc;
	uint8_t *rdata;
	int rlen;
	char req_string[50] = "";
	ks_json_t *req = ks_json_create_object();

	ks_json_add_string_to_object(req, "eof", "true");

	strcpy(req_string, ks_json_print_unformatted(req));

	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "Sending stop talking bit %s\n", req_string);

	if (kws_write_frame(context->ws, WSOC_TEXT, req_string, sizeof req_string) < 0) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Unable to send stop talking bit");

		switch_mutex_unlock(context->mutex);
		return SWITCH_STATUS_BREAK;
	}

	ks_json_delete(&req);

	poll_result = kws_wait_sock(context->ws, 60000, KS_POLL_READ | KS_POLL_ERROR);

	if (poll_result != KS_POLL_READ) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Unable to poll for final message");
		switch_mutex_unlock(context->mutex);
		return SWITCH_STATUS_BREAK;
	}

	rlen = kws_read_frame(context->ws, &oc, &rdata);

	if (rlen < 0) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Final message length is not acceptable");
		switch_mutex_unlock(context->mutex);
		return SWITCH_STATUS_BREAK;
	}

	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "Final response is %d bytes:%s\n", rlen, rdata);

	context->result_text = switch_safe_strdup((const char *)rdata);

	switch_mutex_unlock(context->mutex);

	return SWITCH_STATUS_SUCCESS;
}
static switch_status_t whisper_feed(switch_asr_handle_t *ah, void *data, unsigned int len, switch_asr_flag_t *flags)
{
	whisper_t *context = (whisper_t *) ah->private_info;
	switch_status_t status = SWITCH_STATUS_SUCCESS;
	switch_vad_state_t vad_state;

	int poll_result;
	kws_opcode_t oc;
	uint8_t *rdata;
	int rlen;
	
	if (switch_test_flag(ah, SWITCH_ASR_FLAG_CLOSED)) {
		return SWITCH_STATUS_BREAK;
	}

	if (switch_test_flag(context, ASRFLAG_RETURNED_RESULT) && switch_test_flag(ah, SWITCH_ASR_FLAG_AUTO_RESUME)) {
		switch_log_printf(SWITCH_CHANNEL_UUID_LOG(context->channel_uuid), SWITCH_LOG_DEBUG, "Auto Resuming\n");
		whisper_reset(context);
	}

	if (switch_test_flag(context, ASRFLAG_TIMEOUT)) {

		switch_status_t ws_status;

		ws_status = whisper_send_final_bit(context);

		if (ws_status != SWITCH_STATUS_SUCCESS) {
			return SWITCH_STATUS_BREAK;
		}

		//set vad flags to stop detection
		switch_set_flag(context, ASRFLAG_RESULT);
		switch_vad_reset(context->vad);
		switch_clear_flag(context, ASRFLAG_TIMEOUT);

	}

	if (switch_test_flag(context, ASRFLAG_READY)) {

		vad_state = switch_vad_process(context->vad, (int16_t *)data, len / sizeof(uint16_t));


		if (vad_state == SWITCH_VAD_STATE_TALKING) {

			char buf[AUDIO_BLOCK_SIZE];
			switch_mutex_lock(context->mutex);
			switch_buffer_write(context->audio_buffer, data, len);


			if (switch_buffer_inuse(context->audio_buffer) > AUDIO_BLOCK_SIZE) {
				rlen = switch_buffer_read(context->audio_buffer, buf, AUDIO_BLOCK_SIZE);
				switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "Sending data %d\n", rlen);

				if (kws_write_frame(context->ws, WSOC_BINARY, buf, rlen) < 0) {
					switch_mutex_unlock(context->mutex);
					return SWITCH_STATUS_BREAK;
				}
			} 

			poll_result = kws_wait_sock(context->ws, 5, KS_POLL_READ | KS_POLL_ERROR);

			if (poll_result != KS_POLL_READ) {
				switch_mutex_unlock(context->mutex);
				return SWITCH_STATUS_SUCCESS;
			}

			rlen = kws_read_frame(context->ws, &oc, &rdata);
			if (rlen < 0) {
				switch_mutex_unlock(context->mutex);
				return SWITCH_STATUS_BREAK;
			}

			if (oc == WSOC_PING) {
				switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "Received ping\n");
				kws_write_frame(context->ws, WSOC_PONG, rdata, rlen);
				switch_mutex_unlock(context->mutex);
				return SWITCH_STATUS_SUCCESS;
			}

			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "Recieved %d bytes:%s\n", rlen, rdata);
			context->result_text = switch_safe_strdup((const char *)rdata);
			//switch_mutex_unlock(context->mutex);

		}

		if (vad_state == SWITCH_VAD_STATE_STOP_TALKING) {
			switch_status_t ws_status;

			ws_status = whisper_send_final_bit(context);

			if (ws_status != SWITCH_STATUS_SUCCESS) {
				return SWITCH_STATUS_BREAK;
			}

			//set vad flags to stop detection
			switch_set_flag(context, ASRFLAG_RESULT);
			switch_vad_reset(context->vad);
			switch_clear_flag(context, ASRFLAG_READY);

		} else if (vad_state == SWITCH_VAD_STATE_START_TALKING) {

			switch_set_flag(context, ASRFLAG_START_OF_SPEECH);
			context->speech_time = switch_micro_time_now();
		}
	}


	return status;
}


static switch_status_t whisper_pause(switch_asr_handle_t *ah)
{
	whisper_t *context = (whisper_t *) ah->private_info;

	if (switch_test_flag(ah, SWITCH_ASR_FLAG_CLOSED)) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "asr_pause attempt on CLOSED asr handle\n");
		return SWITCH_STATUS_FALSE;
	}

	switch_log_printf(SWITCH_CHANNEL_UUID_LOG(context->channel_uuid), SWITCH_LOG_DEBUG, "Pausing\n");
	context->flags = 0;

	return SWITCH_STATUS_SUCCESS;
}

static switch_status_t whisper_resume(switch_asr_handle_t *ah)
{
	whisper_t *context = (whisper_t *) ah->private_info;

	if (switch_test_flag(ah, SWITCH_ASR_FLAG_CLOSED)) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "asr_resume attempt on CLOSED asr handle\n");
		return SWITCH_STATUS_FALSE;
	}

	switch_log_printf(SWITCH_CHANNEL_UUID_LOG(context->channel_uuid), SWITCH_LOG_DEBUG, "Resuming\n");
	whisper_reset(context);
	
	return SWITCH_STATUS_SUCCESS;
}

static switch_status_t whisper_check_results(switch_asr_handle_t *ah, switch_asr_flag_t *flags)
{
	whisper_t *context = (whisper_t *) ah->private_info;

	if (switch_test_flag(context, ASRFLAG_RETURNED_RESULT) || switch_test_flag(ah, SWITCH_ASR_FLAG_CLOSED)) {
		return SWITCH_STATUS_BREAK;
	}

	if (!switch_test_flag(context, ASRFLAG_RETURNED_START_OF_SPEECH) && switch_test_flag(context, ASRFLAG_START_OF_SPEECH)) {
		return SWITCH_STATUS_SUCCESS;
	}

	if ((!switch_test_flag(context, ASRFLAG_RESULT)) && (!switch_test_flag(context, ASRFLAG_NOINPUT_TIMEOUT))) {
		if (switch_test_flag(context, ASRFLAG_INPUT_TIMERS) && !(switch_test_flag(context, ASRFLAG_START_OF_SPEECH)) &&
				context->no_input_timeout >= 0 &&
				(switch_micro_time_now() - context->no_input_time) / 1000 >= context->no_input_timeout) {
			switch_log_printf(SWITCH_CHANNEL_UUID_LOG(context->channel_uuid), SWITCH_LOG_DEBUG, "NO INPUT TIMEOUT %" SWITCH_TIME_T_FMT "ms\n", (switch_micro_time_now() - context->no_input_time) / 1000);
			switch_set_flag(context, ASRFLAG_NOINPUT_TIMEOUT);
		} else if (!switch_test_flag(context, ASRFLAG_TIMEOUT) && switch_test_flag(context, ASRFLAG_START_OF_SPEECH) && context->speech_timeout > 0 && (switch_micro_time_now() - context->speech_time) / 1000 >= context->speech_timeout) {
			switch_log_printf(SWITCH_CHANNEL_UUID_LOG(context->channel_uuid), SWITCH_LOG_DEBUG, "SPEECH TIMEOUT %" SWITCH_TIME_T_FMT "ms\n", (switch_micro_time_now() - context->speech_time) / 1000);
			if (switch_test_flag(context, ASRFLAG_START_OF_SPEECH)) {
				switch_set_flag(context, ASRFLAG_TIMEOUT);
				return SWITCH_STATUS_FALSE;
				//switch_set_flag(context, ASRFLAG_RESULT);
			} else {
				switch_set_flag(context, ASRFLAG_NOINPUT_TIMEOUT);
			}
		}
	}

	return switch_test_flag(context, ASRFLAG_RESULT) || switch_test_flag(context, ASRFLAG_NOINPUT_TIMEOUT) ? SWITCH_STATUS_SUCCESS : SWITCH_STATUS_BREAK;
}

static switch_status_t whisper_get_results(switch_asr_handle_t *ah, char **resultstr, switch_asr_flag_t *flags)
{
	whisper_t *context = (whisper_t *) ah->private_info;
	switch_status_t status = SWITCH_STATUS_SUCCESS;

	if (switch_test_flag(context, ASRFLAG_RETURNED_RESULT) || switch_test_flag(ah, SWITCH_ASR_FLAG_CLOSED)) {
		return SWITCH_STATUS_FALSE;
	}

	if (switch_test_flag(context, ASRFLAG_RESULT)) {
		int is_partial = context->partial-- > 0 ? 1 : 0;

		*resultstr = switch_mprintf("{\"grammar\": \"%s\", \"text\": \"%s\", \"confidence\": %f}", context->grammar, context->result_text, context->result_confidence);

		switch_log_printf(SWITCH_CHANNEL_UUID_LOG(context->channel_uuid), SWITCH_LOG_NOTICE, "%sResult: %s\n", is_partial ? "Partial " : "Final ", *resultstr);

		if (is_partial) {
			status = SWITCH_STATUS_MORE_DATA;
		} else {
			status = SWITCH_STATUS_SUCCESS;
		}
	} else if (switch_test_flag(context, ASRFLAG_NOINPUT_TIMEOUT)) {
		switch_log_printf(SWITCH_CHANNEL_UUID_LOG(context->channel_uuid), SWITCH_LOG_DEBUG, "Result: NO INPUT\n");

		*resultstr = switch_mprintf("{\"grammar\": \"%s\", \"text\": \"\", \"confidence\": 0, \"error\": \"no_input\"}", context->grammar);

		status = SWITCH_STATUS_SUCCESS;
	} else if (!switch_test_flag(context, ASRFLAG_RETURNED_START_OF_SPEECH) && switch_test_flag(context, ASRFLAG_START_OF_SPEECH)) {
		switch_set_flag(context, ASRFLAG_RETURNED_START_OF_SPEECH);
		switch_log_printf(SWITCH_CHANNEL_UUID_LOG(context->channel_uuid), SWITCH_LOG_DEBUG, "Result: START OF SPEECH\n");
		status = SWITCH_STATUS_BREAK;
	} else {
		switch_log_printf(SWITCH_CHANNEL_UUID_LOG(context->channel_uuid), SWITCH_LOG_ERROR, "Unexpected call to asr_get_results - no results to return!\n");
		status = SWITCH_STATUS_FALSE;
	}

	if (status == SWITCH_STATUS_SUCCESS) {
		switch_set_flag(context, ASRFLAG_RETURNED_RESULT);
		switch_clear_flag(context, ASRFLAG_READY);
	}

	return status;
}

static switch_status_t whisper_start_input_timers(switch_asr_handle_t *ah)
{
	whisper_t *context = (whisper_t *) ah->private_info;

	if (switch_test_flag(ah, SWITCH_ASR_FLAG_CLOSED)) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "asr_start_input_timers attempt on CLOSED asr handle\n");
		return SWITCH_STATUS_FALSE;
	}

	switch_log_printf(SWITCH_CHANNEL_UUID_LOG(context->channel_uuid), SWITCH_LOG_DEBUG, "start_input_timers\n");

	if (!switch_test_flag(context, ASRFLAG_INPUT_TIMERS)) {
		switch_set_flag(context, ASRFLAG_INPUT_TIMERS);
		context->no_input_time = switch_micro_time_now();
	} else {
		switch_log_printf(SWITCH_CHANNEL_UUID_LOG(context->channel_uuid), SWITCH_LOG_INFO, "Input timers already started\n");
	}

	return SWITCH_STATUS_SUCCESS;
}

static void whisper_text_param(switch_asr_handle_t *ah, char *param, const char *val)
{
	whisper_t *context = (whisper_t *) ah->private_info;

	if (!zstr(param) && !zstr(val)) {
		int nval = atoi(val);
		double fval = atof(val);

		if (!strcasecmp("no-input-timeout", param) && switch_is_number(val)) {
			context->no_input_timeout = nval;
			switch_log_printf(SWITCH_CHANNEL_UUID_LOG(context->channel_uuid), SWITCH_LOG_DEBUG, "no-input-timeout = %d\n", context->no_input_timeout);
		} else if (!strcasecmp("speech-timeout", param) && switch_is_number(val)) {
			context->speech_timeout = nval;
			switch_log_printf(SWITCH_CHANNEL_UUID_LOG(context->channel_uuid), SWITCH_LOG_DEBUG, "speech-timeout = %d\n", context->speech_timeout);
		} else if (!strcasecmp("start-input-timers", param)) {
			context->start_input_timers = switch_true(val);
			if (context->start_input_timers) {
				switch_set_flag(context, ASRFLAG_INPUT_TIMERS);
			} else {
				switch_clear_flag(context, ASRFLAG_INPUT_TIMERS);
			}
			switch_log_printf(SWITCH_CHANNEL_UUID_LOG(context->channel_uuid), SWITCH_LOG_DEBUG, "start-input-timers = %d\n", context->start_input_timers);
		} else if (!strcasecmp("vad-mode", param)) {
			switch_log_printf(SWITCH_CHANNEL_UUID_LOG(context->channel_uuid), SWITCH_LOG_DEBUG, "vad-mode = %s\n", val);
			if (context->vad) switch_vad_set_mode(context->vad, nval);
		} else if (!strcasecmp("vad-voice-ms", param) && nval > 0) {
			context->voice_ms = nval;
			switch_vad_set_param(context->vad, "voice_ms", nval);
		} else if (!strcasecmp("vad-silence-ms", param) && nval > 0) {
			context->silence_ms = nval;
			switch_vad_set_param(context->vad, "silence_ms", nval);
		} else if (!strcasecmp("vad-thresh", param) && nval > 0) {
			context->thresh = nval;
			switch_vad_set_param(context->vad, "thresh", nval);
		} else if (!strcasecmp("channel-uuid", param)) {
			context->channel_uuid = switch_core_strdup(ah->memory_pool, val);
			switch_log_printf(SWITCH_CHANNEL_UUID_LOG(context->channel_uuid), SWITCH_LOG_DEBUG, "channel-uuid = %s\n", val);
		} else if (!strcasecmp("result", param)) {
			context->result_text = switch_core_strdup(ah->memory_pool, val);
			switch_log_printf(SWITCH_CHANNEL_UUID_LOG(context->channel_uuid), SWITCH_LOG_DEBUG, "result = %s\n", val);
		} else if (!strcasecmp("confidence", param) && fval >= 0.0) {
			context->result_confidence = fval;
			switch_log_printf(SWITCH_CHANNEL_UUID_LOG(context->channel_uuid), SWITCH_LOG_DEBUG, "confidence = %f\n", fval);
		} else if (!strcasecmp("partial", param) && switch_true(val)) {
			context->partial = 3;
			switch_log_printf(SWITCH_CHANNEL_UUID_LOG(context->channel_uuid), SWITCH_LOG_DEBUG, "partial = %d\n", context->partial);
		}
	}
}


static switch_status_t load_config(void)
{
	char *cf = "whisper.conf";
	switch_xml_t cfg, xml = NULL, param, settings;
	switch_status_t status = SWITCH_STATUS_SUCCESS;

	if (!(xml = switch_xml_open_cfg(cf, &cfg, NULL))) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Open of %s failed\n", cf);
		status = SWITCH_STATUS_FALSE;
		goto done;
	}


	if ((settings = switch_xml_child(cfg, "settings"))) {
		for (param = switch_xml_child(settings, "param"); param; param = param->next) {
			char *var = (char *) switch_xml_attr_soft(param, "name");
			char *val = (char *) switch_xml_attr_soft(param, "value");
			if (!strcasecmp(var, "server-url")) {
				globals.server_url = switch_core_strdup(globals.pool, val);
			}
			if (!strcasecmp(var, "return-json")) {
				globals.return_json = atoi(val);
			}
		}
	}

  done:
	if (!globals.server_url) {
		globals.server_url = switch_core_strdup(globals.pool, "ws://127.0.0.1:2700");
	}
	if (xml) {
		switch_xml_free(xml);
	}

	return status;
}

static void do_load(void)
{
	switch_mutex_lock(MUTEX);
	load_config();
	switch_mutex_unlock(MUTEX);
}

static void event_handler(switch_event_t *event)
{
	if (globals.auto_reload) {
		do_load();
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "Whisper Reloaded\n");
	}
}



SWITCH_MODULE_LOAD_FUNCTION(mod_whisper_load)
{
	switch_asr_interface_t *asr_interface;

	switch_mutex_init(&MUTEX, SWITCH_MUTEX_NESTED, pool);

	globals.pool = pool;

	ks_init();

	ks_pool_open(&globals.ks_pool);
	ks_global_set_default_logger(7);

	if ((switch_event_bind_removable(modname, SWITCH_EVENT_RELOADXML, NULL, event_handler, NULL, &NODE) != SWITCH_STATUS_SUCCESS)) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Couldn't bind!\n");
	}

	do_load();

	*module_interface = switch_loadable_module_create_module_interface(pool, modname);

	asr_interface = switch_loadable_module_create_interface(*module_interface, SWITCH_ASR_INTERFACE);
	asr_interface->interface_name = "whisper";
	asr_interface->asr_open = whisper_open;
	asr_interface->asr_load_grammar = whisper_load_grammar;
	asr_interface->asr_unload_grammar = whisper_unload_grammar;
	asr_interface->asr_close = whisper_close;
	asr_interface->asr_feed = whisper_feed;
	asr_interface->asr_resume = whisper_resume;
	asr_interface->asr_pause = whisper_pause;
	asr_interface->asr_check_results = whisper_check_results;
	asr_interface->asr_get_results = whisper_get_results;
	asr_interface->asr_start_input_timers = whisper_start_input_timers;
	asr_interface->asr_text_param = whisper_text_param;

	return SWITCH_STATUS_SUCCESS;
}

SWITCH_MODULE_SHUTDOWN_FUNCTION(mod_whisper_shutdown)
{
	ks_pool_close(&globals.ks_pool);
	ks_shutdown();

	switch_event_unbind(&NODE);
	return SWITCH_STATUS_SUCCESS;
}

SWITCH_MODULE_RUNTIME_FUNCTION(mod_whisper_runtime)
{
	return SWITCH_STATUS_TERM;
}

/* For Emacs:
 * Local Variables:
 * mode:c
 * indent-tabs-mode:t
 * tab-width:4
 * c-basic-offset:4
 * End:
 * For VIM:
 * vim:set softtabstop=4 shiftwidth=4 tabstop=4 noet:
 */