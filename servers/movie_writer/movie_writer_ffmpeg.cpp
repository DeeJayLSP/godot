/**************************************************************************/
/*  movie_writer_ffmpeg.cpp                                               */
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

#include "movie_writer_ffmpeg.h"

#include "core/config/project_settings.h"
#include "core/os/os.h"

uint32_t MovieWriterFFmpeg::get_audio_mix_rate() const {
	return mix_rate;
}

AuSE::SpeakerMode MovieWriterFFmpeg::get_audio_speaker_mode() const {
	return speaker_mode;
}

void MovieWriterFFmpeg::get_supported_extensions(List<String> *r_extensions) const {
	r_extensions->push_back("mkv");
}

bool MovieWriterFFmpeg::handles_file(const String &p_path) const {
	return p_path.has_extension("mkv");
}

Error MovieWriterFFmpeg::write_begin(const Size2i &p_movie_size, uint32_t p_fps, const String &p_base_path) {
	ERR_FAIL_COND_V_MSG(!FileAccess::exists(binary_path), ERR_FILE_NOT_FOUND, "Invalid FFmpeg binary.");

	base_path = p_base_path.get_basename();
	if (base_path.is_relative_path()) { // FFmpeg doesn't handle res://, so it needs to be global.
		base_path = ProjectSettings::get_singleton()->globalize_path(base_path);
	}

	fps = p_fps;

	List<String> varg = {
		"-y",
		"-f", "rawvideo",
		"-pixel_format", "rgba",
		"-video_size", itos(p_movie_size.width) + "x" + itos(p_movie_size.height),
		"-r", itos(fps),
		"-i", "-"
	};

	const PackedStringArray video_args_split = video_args.split(" ");

	for (const String &s: video_args_split){
		varg.push_back(s);
	}
	varg.push_back(base_path + ".mkv");

	Dictionary video_dict = OS::get_singleton()->execute_with_pipe(binary_path, varg/*, false */);
	ERR_FAIL_COND_V_MSG(video_dict.size() == 0, ERR_CANT_CREATE, "Could not execute FFmpeg for video writing.");
	video_pipe = video_dict["stdio"];
	video_stderr = video_dict["stderr"];
	video_pid = video_dict["pid"];

	uint32_t channels = 2;
	switch (speaker_mode) {
		case AuSE::SPEAKER_MODE_STEREO:
			channels = 2;
			break;
		case AuSE::SPEAKER_SURROUND_31:
			channels = 4;
			break;
		case AuSE::SPEAKER_SURROUND_51:
			channels = 6;
			break;
		case AuSE::SPEAKER_SURROUND_71:
			channels = 8;
			break;
	}

	List<String> aarg = {
		"-y",
		"-f", "s32le",
		"-ar", itos(mix_rate),
		"-ac", itos(channels),
		"-i", "-"
	};

	const PackedStringArray audio_args_split = audio_args.split(" ");

	for (const String &s: audio_args_split){
		aarg.push_back(s);
	}
	aarg.push_back(base_path + ".mka");

	Dictionary audio_dict = OS::get_singleton()->execute_with_pipe(binary_path, aarg/*, false */);
	ERR_FAIL_COND_V_MSG(audio_dict.size() == 0, ERR_CANT_CREATE, "Could not execute FFmpeg for audio writing.");
	audio_pipe = audio_dict["stdio"];
	audio_stderr = audio_dict["stderr"];
	audio_pid = audio_dict["pid"];

	const int blockalign = 4 * channels;
	audio_block_size = (mix_rate / fps) * blockalign;

	return OK;
}

Error MovieWriterFFmpeg::write_frame(const Ref<Image> &p_image, const int32_t *p_audio_data) {
	const Vector<uint8_t> image_data = p_image->get_data();
	ERR_FAIL_COND_V_MSG(!video_pipe->store_buffer(image_data.ptr(), image_data.size()), ERR_FILE_CANT_WRITE, video_stderr->get_as_text());
	ERR_FAIL_COND_V_MSG(!audio_pipe->store_buffer((const uint8_t*)p_audio_data, audio_block_size), ERR_FILE_CANT_WRITE, audio_stderr->get_as_text());
	return OK;
}

void MovieWriterFFmpeg::write_end() {
	video_pipe->close();
	audio_pipe->close();
}

MovieWriterFFmpeg::MovieWriterFFmpeg() {
	binary_path = GLOBAL_GET("editor/movie_writer/ffmpeg/binary_path");
	mix_rate = GLOBAL_GET("editor/movie_writer/mix_rate");
	speaker_mode = AuSE::SpeakerMode(int(GLOBAL_GET("editor/movie_writer/speaker_mode")));
	video_args = GLOBAL_GET("editor/movie_writer/ffmpeg/video_arguments");
	audio_args = GLOBAL_GET("editor/movie_writer/ffmpeg/audio_arguments");
}
