/**************************************************************************/
/*  audio_stream_wav.cpp                                                  */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/
/* Copyright (c) 2014-present Godot Engine contributors (see AUTHORS.md). */
/* Copyright (c) 2007-2014 Juan Linietsky, Ariel Manzur.                  */
/*                                                                        */
/* Permission is hereby granted, free of charge, to any person obtaining  */
/* a copy of this software and associated documentation files (the        */
/* "Software"), to deal in the Software without restriction, including    */
/* without limitation the rights to use, copy, modify, merge, publish,    */
/* distribute, sublicense, and/or sell copies of the Software, and to     */
/* permit persons to whom the Software is furnished to do so, subject to  */
/* the following conditions:                                              */
/*                                                                        */
/* The above copyright notice and this permission notice shall be         */
/* included in all copies or substantial portions of the Software.        */
/*                                                                        */
/* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,        */
/* EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF     */
/* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. */
/* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY   */
/* CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,   */
/* TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE      */
/* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.                 */
/**************************************************************************/

#include "audio_stream_wav.h"

#include "core/io/file_access.h"
#include "core/io/marshalls.h"

void AudioStreamPlaybackWAV::start(double p_from_pos) {
	if (base->format == AudioStreamWAV::FORMAT_IMA_ADPCM) { // No seeking in IMA ADPCM
		for (int i = 0; i < 2; i++) {
			ima_adpcm[i].step_index = 0;
			ima_adpcm[i].predictor = 0;
			ima_adpcm[i].loop_step_index = 0;
			ima_adpcm[i].loop_predictor = 0;
			ima_adpcm[i].last_nibble = -1;
			ima_adpcm[i].loop_pos = 0x7FFFFFFF;
			ima_adpcm[i].window_ofs = 0;
		}

		frames_mixed = 0;
	} else {
		seek(p_from_pos);
	}

	sign = 1;
	active = true;
}

void AudioStreamPlaybackWAV::stop() {
	active = false;
}

bool AudioStreamPlaybackWAV::is_playing() const {
	return active;
}

int AudioStreamPlaybackWAV::get_loop_count() const {
	return 0;
}

double AudioStreamPlaybackWAV::get_playback_position() const {
	return float(frames_mixed) / base->mix_rate;
}

void AudioStreamPlaybackWAV::seek(double p_time) {
	if (base->format == AudioStreamWAV::FORMAT_IMA_ADPCM) {
		return; // No seeking in IMA ADPCM
	}

	double max = base->get_length();
	if (p_time < 0) {
		p_time = 0;
	} else if (p_time >= max) {
		p_time = max - 0.001;
	}

	frames_mixed = uint64_t(p_time * base->mix_rate);
}

template <typename Depth, bool is_stereo, bool is_ima_adpcm, bool is_qoa>
void AudioStreamPlaybackWAV::decode_samples(const Depth *p_src, AudioFrame *p_dst, int32_t &p_offset, int32_t &p_increment, uint32_t p_amount, IMA_ADPCM_State *p_ima_adpcm, QOA_State *p_qoa) {
	// This function will be compiled branchless by any decent compiler

	int final = 0, final_r = 0;
	while (p_amount) {
		p_amount--;
		int pos = p_offset << (is_stereo && !is_ima_adpcm && !is_qoa ? 1 : 0);

		if (is_ima_adpcm) {
			int64_t sample_pos = pos + p_ima_adpcm[0].window_ofs;

			while (sample_pos > p_ima_adpcm[0].last_nibble) {
				static const int16_t _ima_adpcm_step_table[89] = {
					7, 8, 9, 10, 11, 12, 13, 14, 16, 17,
					19, 21, 23, 25, 28, 31, 34, 37, 41, 45,
					50, 55, 60, 66, 73, 80, 88, 97, 107, 118,
					130, 143, 157, 173, 190, 209, 230, 253, 279, 307,
					337, 371, 408, 449, 494, 544, 598, 658, 724, 796,
					876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066,
					2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358,
					5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899,
					15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767
				};

				static const int8_t _ima_adpcm_index_table[16] = {
					-1, -1, -1, -1, 2, 4, 6, 8,
					-1, -1, -1, -1, 2, 4, 6, 8
				};

				for (int i = 0; i < (is_stereo ? 2 : 1); i++) {
					int16_t nibble, diff, step;

					p_ima_adpcm[i].last_nibble++;

					uint8_t nbb = p_src[(p_ima_adpcm[i].last_nibble >> 1) * (is_stereo ? 2 : 1) + i];
					nibble = (p_ima_adpcm[i].last_nibble & 1) ? (nbb >> 4) : (nbb & 0xF);
					step = _ima_adpcm_step_table[p_ima_adpcm[i].step_index];

					p_ima_adpcm[i].step_index += _ima_adpcm_index_table[nibble];
					if (p_ima_adpcm[i].step_index < 0) {
						p_ima_adpcm[i].step_index = 0;
					}
					if (p_ima_adpcm[i].step_index > 88) {
						p_ima_adpcm[i].step_index = 88;
					}

					diff = step >> 3;
					if (nibble & 1) {
						diff += step >> 2;
					}
					if (nibble & 2) {
						diff += step >> 1;
					}
					if (nibble & 4) {
						diff += step;
					}
					if (nibble & 8) {
						diff = -diff;
					}

					p_ima_adpcm[i].predictor += diff;
					if (p_ima_adpcm[i].predictor < -0x8000) {
						p_ima_adpcm[i].predictor = -0x8000;
					} else if (p_ima_adpcm[i].predictor > 0x7FFF) {
						p_ima_adpcm[i].predictor = 0x7FFF;
					}

					// Store loop if there
					if (p_ima_adpcm[i].last_nibble == p_ima_adpcm[i].loop_pos) {
						p_ima_adpcm[i].loop_step_index = p_ima_adpcm[i].step_index;
						p_ima_adpcm[i].loop_predictor = p_ima_adpcm[i].predictor;
					}
				}
			}

			final = p_ima_adpcm[0].predictor;
			if (is_stereo) {
				final_r = p_ima_adpcm[1].predictor;
			}

		} else if (is_qoa) {
			uint32_t new_data_ofs = 8 + pos / QOA_FRAME_LEN * p_qoa->frame_len;

			if (p_qoa->data_ofs != new_data_ofs) {
				p_qoa->data_ofs = new_data_ofs;
				const uint8_t *ofs_src = (uint8_t *)p_src + p_qoa->data_ofs;
				qoa_decode_frame(ofs_src, p_qoa->frame_len, &p_qoa->desc, p_qoa->dec.ptr(), &p_qoa->dec_len);
			}

			uint32_t dec_idx = pos % QOA_FRAME_LEN << (is_stereo ? 1 : 0);

			final = p_qoa->dec[dec_idx];
			if (is_stereo) {
				final_r = p_qoa->dec[dec_idx + 1];
			}

		} else {
			final = p_src[pos];
			if (is_stereo) {
				final_r = p_src[pos + 1];
			}

			if constexpr (sizeof(Depth) == 1) { // Conditions will not exist anymore when compiled
				final <<= 8;
				if (is_stereo) {
					final_r <<= 8;
				}
			}
		}

		if (!is_stereo) {
			final_r = final;
		}

		p_dst->left = final / 32767.0;
		p_dst->right = final_r / 32767.0;
		p_dst++;

		p_offset += p_increment;
	}
}

int AudioStreamPlaybackWAV::_mix_internal(AudioFrame *p_buffer, int p_frames) {
	if (base->data.is_empty() || !active) {
		for (int i = 0; i < p_frames; i++) {
			p_buffer[i] = AudioFrame(0, 0);
		}
		return 0;
	}

	int32_t todo = p_frames;
	AudioFrame *dst_buff = p_buffer;
	int increment = sign;
	int frames_mixed_this_step = p_frames;

	while (todo > 0) {
		// Check loops

		if (increment < 0) { // Backward
			if (base->loop_mode != AudioStreamWAV::LOOP_DISABLED && frames_mixed < base->loop_begin) {
				if (base->loop_mode == AudioStreamWAV::LOOP_PINGPONG) {
					frames_mixed = base->loop_begin + (base->loop_begin - frames_mixed);
					increment = -increment;
					sign *= -1;
				} else {
					frames_mixed = base->loop_end - (base->loop_begin - frames_mixed);
				}
			} else {
				if (frames_mixed < 0) { // Behind the first frame
					active = false;
					break;
				}
			}
		} else { // Forward
			if (base->loop_mode != AudioStreamWAV::LOOP_DISABLED && frames_mixed >= base->loop_end) {
				if (base->loop_mode == AudioStreamWAV::LOOP_PINGPONG) {
					frames_mixed = base->loop_end - (frames_mixed - base->loop_end);
					increment = -increment;
					sign *= -1;
				} else {
					if (base->format == AudioStreamWAV::FORMAT_IMA_ADPCM) {
						for (int i = 0; i < 2; i++) {
							ima_adpcm[i].step_index = ima_adpcm[i].loop_step_index;
							ima_adpcm[i].predictor = ima_adpcm[i].loop_predictor;
							ima_adpcm[i].last_nibble = base->loop_begin;
						}
						frames_mixed = base->loop_begin;
					} else {
						frames_mixed = base->loop_begin + (frames_mixed - base->loop_end);
					}
				}
			} else {
				if (frames_mixed >= length) { // Beyond the last frame
					active = false;
					break;
				}
			}
		}

		// Calculate the amount of frames to mix, stop at one of the limits if too short

		int limit = (increment < 0) ? begin_limit : end_limit;
		int to_limit = (limit - frames_mixed) / increment + 1;
		int target = (to_limit < todo) ? to_limit : todo;

		switch (base->format) {
			case AudioStreamWAV::FORMAT_8_BITS: {
				if (base->stereo) {
					decode_samples<int8_t, true, false, false>((int8_t *)base->data.ptr(), dst_buff, frames_mixed, increment, target, ima_adpcm, &qoa);
				} else {
					decode_samples<int8_t, false, false, false>((int8_t *)base->data.ptr(), dst_buff, frames_mixed, increment, target, ima_adpcm, &qoa);
				}
			} break;
			case AudioStreamWAV::FORMAT_16_BITS: {
				if (base->stereo) {
					decode_samples<int16_t, true, false, false>((int16_t *)base->data.ptr(), dst_buff, frames_mixed, increment, target, ima_adpcm, &qoa);
				} else {
					decode_samples<int16_t, false, false, false>((int16_t *)base->data.ptr(), dst_buff, frames_mixed, increment, target, ima_adpcm, &qoa);
				}

			} break;
			case AudioStreamWAV::FORMAT_IMA_ADPCM: {
				if (base->stereo) {
					decode_samples<int8_t, true, true, false>((int8_t *)base->data.ptr(), dst_buff, frames_mixed, increment, target, ima_adpcm, &qoa);
				} else {
					decode_samples<int8_t, false, true, false>((int8_t *)base->data.ptr(), dst_buff, frames_mixed, increment, target, ima_adpcm, &qoa);
				}

			} break;
			case AudioStreamWAV::FORMAT_QOA: {
				if (base->stereo) {
					decode_samples<uint8_t, true, false, true>((uint8_t *)base->data.ptr(), dst_buff, frames_mixed, increment, target, ima_adpcm, &qoa);
				} else {
					decode_samples<uint8_t, false, false, true>((uint8_t *)base->data.ptr(), dst_buff, frames_mixed, increment, target, ima_adpcm, &qoa);
				}
			} break;
		}

		todo -= target;
		dst_buff += target;
	}

	if (todo) {
		frames_mixed_this_step = p_frames - todo;
		for (int i = frames_mixed_this_step; i < p_frames; i++) {
			p_buffer[i] = AudioFrame(0, 0);
		}
	}
	return frames_mixed_this_step;
}

float AudioStreamPlaybackWAV::get_stream_sampling_rate() {
	return base->mix_rate;
}

void AudioStreamPlaybackWAV::tag_used_streams() {
	base->tag_used(get_playback_position());
}

void AudioStreamPlaybackWAV::set_is_sample(bool p_is_sample) {
	_is_sample = p_is_sample;
}

bool AudioStreamPlaybackWAV::get_is_sample() const {
	return _is_sample;
}

Ref<AudioSamplePlayback> AudioStreamPlaybackWAV::get_sample_playback() const {
	return sample_playback;
}

void AudioStreamPlaybackWAV::set_sample_playback(const Ref<AudioSamplePlayback> &p_playback) {
	sample_playback = p_playback;
	if (sample_playback.is_valid()) {
		sample_playback->stream_playback = Ref<AudioStreamPlayback>(this);
	}
}

AudioStreamPlaybackWAV::AudioStreamPlaybackWAV() {}

AudioStreamPlaybackWAV::~AudioStreamPlaybackWAV() {}

/////////////////////

void AudioStreamWAV::set_format(Format p_format) {
	format = p_format;
}

AudioStreamWAV::Format AudioStreamWAV::get_format() const {
	return format;
}

void AudioStreamWAV::set_loop_mode(LoopMode p_loop_mode) {
	loop_mode = p_loop_mode;
}

AudioStreamWAV::LoopMode AudioStreamWAV::get_loop_mode() const {
	return loop_mode;
}

void AudioStreamWAV::set_loop_begin(int p_frame) {
	loop_begin = p_frame;
}

int AudioStreamWAV::get_loop_begin() const {
	return loop_begin;
}

void AudioStreamWAV::set_loop_end(int p_frame) {
	loop_end = p_frame;
}

int AudioStreamWAV::get_loop_end() const {
	return loop_end;
}

void AudioStreamWAV::set_mix_rate(int p_hz) {
	ERR_FAIL_COND(p_hz == 0);
	mix_rate = p_hz;
}

int AudioStreamWAV::get_mix_rate() const {
	return mix_rate;
}

void AudioStreamWAV::set_stereo(bool p_enable) {
	stereo = p_enable;
}

bool AudioStreamWAV::is_stereo() const {
	return stereo;
}

double AudioStreamWAV::get_length() const {
	int len = data_len;
	switch (format) {
		case AudioStreamWAV::FORMAT_8_BITS:
			len /= 1;
			break;
		case AudioStreamWAV::FORMAT_16_BITS:
			len /= 2;
			break;
		case AudioStreamWAV::FORMAT_IMA_ADPCM:
			len *= 2;
			break;
		case AudioStreamWAV::FORMAT_QOA:
			qoa_desc desc = {};
			qoa_decode_header(data.ptr(), data_len, &desc);
			len = desc.samples * desc.channels;
			break;
	}

	if (stereo) {
		len /= 2;
	}

	return double(len) / mix_rate;
}

bool AudioStreamWAV::is_monophonic() const {
	return false;
}

void AudioStreamWAV::set_data(const Vector<uint8_t> &p_data) {
	AudioServer::get_singleton()->lock();

	data = p_data;
	data_len = p_data.size();

	AudioServer::get_singleton()->unlock();
}

Vector<uint8_t> AudioStreamWAV::get_data() const {
	return data;
}

Error AudioStreamWAV::save_to_wav(const String &p_path) {
	if (format == AudioStreamWAV::FORMAT_IMA_ADPCM || format == AudioStreamWAV::FORMAT_QOA) {
		WARN_PRINT("Saving IMA_ADPCM and QOA samples is not supported yet");
		return ERR_UNAVAILABLE;
	}

	int sub_chunk_2_size = data_len; //Subchunk2Size = Size of data in bytes

	// Format code
	// 1:PCM format (for 8 or 16 bit)
	// 3:IEEE float format
	int format_code = (format == FORMAT_IMA_ADPCM) ? 3 : 1;

	int n_channels = stereo ? 2 : 1;

	long sample_rate = mix_rate;

	int byte_pr_sample = 0;
	switch (format) {
		case AudioStreamWAV::FORMAT_8_BITS:
			byte_pr_sample = 1;
			break;
		case AudioStreamWAV::FORMAT_16_BITS:
		case AudioStreamWAV::FORMAT_QOA:
			byte_pr_sample = 2;
			break;
		case AudioStreamWAV::FORMAT_IMA_ADPCM:
			byte_pr_sample = 4;
			break;
	}

	String file_path = p_path;
	if (!(file_path.substr(file_path.length() - 4, 4) == ".wav")) {
		file_path += ".wav";
	}

	Ref<FileAccess> file = FileAccess::open(file_path, FileAccess::WRITE); //Overrides existing file if present

	ERR_FAIL_COND_V(file.is_null(), ERR_FILE_CANT_WRITE);

	// Create WAV Header
	file->store_string("RIFF"); //ChunkID
	file->store_32(sub_chunk_2_size + 36); //ChunkSize = 36 + SubChunk2Size (size of entire file minus the 8 bits for this and previous header)
	file->store_string("WAVE"); //Format
	file->store_string("fmt "); //Subchunk1ID
	file->store_32(16); //Subchunk1Size = 16
	file->store_16(format_code); //AudioFormat
	file->store_16(n_channels); //Number of Channels
	file->store_32(sample_rate); //SampleRate
	file->store_32(sample_rate * n_channels * byte_pr_sample); //ByteRate
	file->store_16(n_channels * byte_pr_sample); //BlockAlign = NumChannels * BytePrSample
	file->store_16(byte_pr_sample * 8); //BitsPerSample
	file->store_string("data"); //Subchunk2ID
	file->store_32(sub_chunk_2_size); //Subchunk2Size

	// Add data
	Vector<uint8_t> stream_data = get_data();
	const uint8_t *read_data = stream_data.ptr();
	switch (format) {
		case AudioStreamWAV::FORMAT_8_BITS:
			for (unsigned int i = 0; i < data_len; i++) {
				uint8_t data_point = (read_data[i] + 128);
				file->store_8(data_point);
			}
			break;
		case AudioStreamWAV::FORMAT_16_BITS:
		case AudioStreamWAV::FORMAT_QOA:
			for (unsigned int i = 0; i < data_len / 2; i++) {
				uint16_t data_point = decode_uint16(&read_data[i * 2]);
				file->store_16(data_point);
			}
			break;
		case AudioStreamWAV::FORMAT_IMA_ADPCM:
			//Unimplemented
			break;
	}

	return OK;
}

Ref<AudioStreamPlayback> AudioStreamWAV::instantiate_playback() {
	Ref<AudioStreamPlaybackWAV> sample;
	sample.instantiate();
	sample->base = Ref<AudioStreamWAV>(this);

	switch (format) {
		case AudioStreamWAV::FORMAT_8_BITS:
			sample->length = data_len;
			break;

		case AudioStreamWAV::FORMAT_16_BITS:
			sample->length = data_len / 2;
			break;

		case AudioStreamWAV::FORMAT_IMA_ADPCM:
			sample->length = data_len * 2;
			if (loop_mode != AudioStreamWAV::LoopMode::LOOP_DISABLED) {
				sample->ima_adpcm[0].loop_pos = loop_begin;
				sample->ima_adpcm[1].loop_pos = loop_begin;
				loop_mode = AudioStreamWAV::LoopMode::LOOP_FORWARD;
			}
			break;

		case AudioStreamWAV::FORMAT_QOA:
			uint32_t ffp = qoa_decode_header(data.ptr(), data_len, &sample->qoa.desc);
			ERR_FAIL_COND_V(ffp != 8, Ref<AudioStreamPlaybackWAV>());
			sample->length = sample->qoa.desc.samples * sample->qoa.desc.channels;
			sample->qoa.frame_len = qoa_max_frame_size(&sample->qoa.desc);
			int samples_len = (sample->qoa.desc.samples > QOA_FRAME_LEN ? QOA_FRAME_LEN : sample->qoa.desc.samples);
			int dec_len = sample->qoa.desc.channels * samples_len;
			sample->qoa.dec.resize(dec_len);
			break;
	}

	if (loop_mode == AudioStreamWAV::LoopMode::LOOP_BACKWARD) {
		sample->sign = -1;
	}

	sample->length /= stereo ? 2 : 1;
	sample->begin_limit = loop_mode != AudioStreamWAV::LoopMode::LOOP_DISABLED ? loop_begin : 0;
	sample->end_limit = loop_mode != AudioStreamWAV::LoopMode::LOOP_DISABLED ? loop_end : sample->length - 1;

	return sample;
}

String AudioStreamWAV::get_stream_name() const {
	return "";
}

Ref<AudioSample> AudioStreamWAV::generate_sample() const {
	Ref<AudioSample> sample;
	sample.instantiate();
	sample->stream = this;
	switch (loop_mode) {
		case AudioStreamWAV::LoopMode::LOOP_DISABLED: {
			sample->loop_mode = AudioSample::LoopMode::LOOP_DISABLED;
		} break;

		case AudioStreamWAV::LoopMode::LOOP_FORWARD: {
			sample->loop_mode = AudioSample::LoopMode::LOOP_FORWARD;
		} break;

		case AudioStreamWAV::LoopMode::LOOP_PINGPONG: {
			sample->loop_mode = AudioSample::LoopMode::LOOP_PINGPONG;
		} break;

		case AudioStreamWAV::LoopMode::LOOP_BACKWARD: {
			sample->loop_mode = AudioSample::LoopMode::LOOP_BACKWARD;
		} break;
	}
	sample->loop_begin = loop_begin;
	sample->loop_end = loop_end;
	sample->sample_rate = mix_rate;
	return sample;
}

void AudioStreamWAV::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_data", "data"), &AudioStreamWAV::set_data);
	ClassDB::bind_method(D_METHOD("get_data"), &AudioStreamWAV::get_data);

	ClassDB::bind_method(D_METHOD("set_format", "format"), &AudioStreamWAV::set_format);
	ClassDB::bind_method(D_METHOD("get_format"), &AudioStreamWAV::get_format);

	ClassDB::bind_method(D_METHOD("set_loop_mode", "loop_mode"), &AudioStreamWAV::set_loop_mode);
	ClassDB::bind_method(D_METHOD("get_loop_mode"), &AudioStreamWAV::get_loop_mode);

	ClassDB::bind_method(D_METHOD("set_loop_begin", "loop_begin"), &AudioStreamWAV::set_loop_begin);
	ClassDB::bind_method(D_METHOD("get_loop_begin"), &AudioStreamWAV::get_loop_begin);

	ClassDB::bind_method(D_METHOD("set_loop_end", "loop_end"), &AudioStreamWAV::set_loop_end);
	ClassDB::bind_method(D_METHOD("get_loop_end"), &AudioStreamWAV::get_loop_end);

	ClassDB::bind_method(D_METHOD("set_mix_rate", "mix_rate"), &AudioStreamWAV::set_mix_rate);
	ClassDB::bind_method(D_METHOD("get_mix_rate"), &AudioStreamWAV::get_mix_rate);

	ClassDB::bind_method(D_METHOD("set_stereo", "stereo"), &AudioStreamWAV::set_stereo);
	ClassDB::bind_method(D_METHOD("is_stereo"), &AudioStreamWAV::is_stereo);

	ClassDB::bind_method(D_METHOD("save_to_wav", "path"), &AudioStreamWAV::save_to_wav);

	ADD_PROPERTY(PropertyInfo(Variant::PACKED_BYTE_ARRAY, "data", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_NO_EDITOR), "set_data", "get_data");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "format", PROPERTY_HINT_ENUM, "8-Bit,16-Bit,IMA ADPCM,Quite OK Audio"), "set_format", "get_format");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "loop_mode", PROPERTY_HINT_ENUM, "Disabled,Forward,Ping-Pong,Backward"), "set_loop_mode", "get_loop_mode");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "loop_begin"), "set_loop_begin", "get_loop_begin");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "loop_end"), "set_loop_end", "get_loop_end");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "mix_rate"), "set_mix_rate", "get_mix_rate");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "stereo"), "set_stereo", "is_stereo");

	BIND_ENUM_CONSTANT(FORMAT_8_BITS);
	BIND_ENUM_CONSTANT(FORMAT_16_BITS);
	BIND_ENUM_CONSTANT(FORMAT_IMA_ADPCM);
	BIND_ENUM_CONSTANT(FORMAT_QOA);

	BIND_ENUM_CONSTANT(LOOP_DISABLED);
	BIND_ENUM_CONSTANT(LOOP_FORWARD);
	BIND_ENUM_CONSTANT(LOOP_PINGPONG);
	BIND_ENUM_CONSTANT(LOOP_BACKWARD);
}

AudioStreamWAV::AudioStreamWAV() {}

AudioStreamWAV::~AudioStreamWAV() {}
