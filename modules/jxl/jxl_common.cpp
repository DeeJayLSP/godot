/**************************************************************************/
/*  jxl_common.cpp                                                        */
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

#include "jxl_common.h"

#include "core/config/project_settings.h"

#include <jxl/decode.h>
#include <jxl/encode.h>

namespace JXLCommon {
Vector<uint8_t> _jxl_packer(const Ref<Image> &p_image, float p_quality) {
	ERR_FAIL_COND_V(p_image.is_null() || p_image->is_empty(), Vector<uint8_t>());

	int compression_effort = GLOBAL_GET("rendering/textures/jpeg_xl_compression/compression_effort");
	compression_effort = CLAMP(compression_effort, 1, 10);

	Ref<Image> img = p_image->duplicate();
	if (img->is_compressed()) {
		Error error = img->decompress();
		ERR_FAIL_COND_V_MSG(error != OK, Vector<uint8_t>(), "Couldn't decompress image for JXL packing.");
	}
	bool is_alpha = img->detect_alpha() != Image::ALPHA_NONE;
	img->convert(is_alpha ? Image::FORMAT_RGBA8 : Image::FORMAT_RGB8);

	Size2i s(img->get_width(), img->get_height());
	Vector<uint8_t> data = img->get_data();
	Vector<uint8_t> dst;

	JxlEncoder *enc = JxlEncoderCreate(nullptr);

	JxlPixelFormat pixel_format = { is_alpha ? 4u : 3u, JXL_TYPE_UINT8, JXL_NATIVE_ENDIAN, 0 };
	JxlEncoderFrameSettings *frame_settings = JxlEncoderFrameSettingsCreate(enc, nullptr);

	if (frame_settings == nullptr) {
		JxlEncoderDestroy(enc);
		ERR_FAIL_V_MSG(dst, "Failed to create JXL frame settings.");
	}

	float quality = CLAMP(p_quality * 100.0f, 0.0f, 100.0f);
	JxlEncoderSetFrameDistance(frame_settings, JxlEncoderDistanceFromQuality(quality));
	JxlEncoderFrameSettingsSetOption(frame_settings, JXL_ENC_FRAME_SETTING_EFFORT, compression_effort);
	JxlEncoderFrameSettingsSetOption(frame_settings, JXL_ENC_FRAME_SETTING_DECODING_SPEED, 1);

	JxlBasicInfo basic_info;
	JxlEncoderInitBasicInfo(&basic_info);
	basic_info.xsize = s.width;
	basic_info.ysize = s.height;
	basic_info.bits_per_sample = 8;
	basic_info.num_color_channels = 3; // RGB
	basic_info.num_extra_channels = is_alpha ? 1 : 0;
	basic_info.alpha_bits = is_alpha ? 8 : 0;
	basic_info.uses_original_profile = p_quality >= 1.0f ? JXL_TRUE : JXL_FALSE;
	JxlEncoderSetBasicInfo(enc, &basic_info);

	JxlColorEncoding color_encoding;
	JxlColorEncodingSetToSRGB(&color_encoding, JXL_FALSE);
	JxlEncoderSetColorEncoding(enc, &color_encoding);
	JxlEncoderAddImageFrame(frame_settings, &pixel_format, data.ptr(), data.size());
	JxlEncoderCloseInput(enc);

	dst.resize(64);
	uint8_t *dst_ptr = dst.ptrw();
	size_t dst_size = dst.size();

	JxlEncoderStatus status = JXL_ENC_NEED_MORE_OUTPUT;
	while (status == JXL_ENC_NEED_MORE_OUTPUT) {
		status = JxlEncoderProcessOutput(enc, &dst_ptr, &dst_size);
		if (status == JXL_ENC_NEED_MORE_OUTPUT) {
			size_t old_size = dst.size();
			dst.resize(old_size * 2);
			dst_ptr = dst.ptrw() + old_size;
			dst_size = dst.size() - old_size;
		}
	}

	if (status != JXL_ENC_SUCCESS) {
		JxlEncoderDestroy(enc);
		dst.clear();
		ERR_FAIL_V_MSG(dst, "Failed to encode JXL image.");
	}

	JxlEncoderDestroy(enc);
	dst.resize(dst_ptr - dst.ptr());
	return dst;
}

Ref<Image> _jxl_unpack(const Vector<uint8_t> &p_buffer) {
	int size = p_buffer.size();
	ERR_FAIL_COND_V(size <= 0, Ref<Image>());
	const uint8_t *r = p_buffer.ptr();

	JxlDecoder *dec = JxlDecoderCreate(nullptr);
	if (JxlDecoderSubscribeEvents(dec, JXL_DEC_BASIC_INFO | JXL_DEC_FULL_IMAGE) != JXL_DEC_SUCCESS) {
		JxlDecoderDestroy(dec);
		ERR_FAIL_V_MSG(Ref<Image>(), "Failed to subscribe to JXL decoder events.");
	}

	if (JxlDecoderSetInput(dec, r, p_buffer.size()) != JXL_DEC_SUCCESS) {
		JxlDecoderDestroy(dec);
		ERR_FAIL_V_MSG(Ref<Image>(), "Failed to set JXL decoder input.");
	}
	JxlDecoderCloseInput(dec);

	JxlPixelFormat pixel_format = { 4, JXL_TYPE_UINT8, JXL_NATIVE_ENDIAN, 0 };

	Vector<uint8_t> dst_image;
	Size2i image_size;
	int channels = 0;

	while (true) {
		JxlDecoderStatus status = JxlDecoderProcessInput(dec);

		if (status == JXL_DEC_ERROR) {
			JxlDecoderDestroy(dec);
			ERR_FAIL_V_MSG(Ref<Image>(), "Failed to decode JXL image.");
		}

		if (status == JXL_DEC_NEED_MORE_INPUT) {
			JxlDecoderDestroy(dec);
			ERR_FAIL_V_MSG(Ref<Image>(), "JXL decoder needs more input.");
		}

		if (status == JXL_DEC_BASIC_INFO) {
			JxlBasicInfo basic_info;
			JxlDecoderGetBasicInfo(dec, &basic_info);
			image_size.width = basic_info.xsize;
			image_size.height = basic_info.ysize;
			channels = (basic_info.alpha_bits > 0) ? 4 : 3;
			pixel_format.num_channels = channels;

		} else if (status == JXL_DEC_NEED_IMAGE_OUT_BUFFER) {
			size_t buffer_size = 0;
			JxlDecoderImageOutBufferSize(dec, &pixel_format, &buffer_size);
			dst_image.resize(buffer_size);
			JxlDecoderSetImageOutBuffer(dec, &pixel_format, dst_image.ptrw(), dst_image.size());
		} else if (status == JXL_DEC_SUCCESS || status == JXL_DEC_FULL_IMAGE) {
			break;
		} else { // Unknown format?
			JxlDecoderDestroy(dec);
			ERR_FAIL_V_MSG(Ref<Image>(), "Unknown JXL decoder status.");
		}
	}

	JxlDecoderDestroy(dec);
	Ref<Image> img = memnew(Image(image_size.width, image_size.height, false, channels == 4 ? Image::FORMAT_RGBA8 : Image::FORMAT_RGB8, dst_image));
	return img;
}

Error jxl_load_image_from_buffer(Image *p_image, const uint8_t *p_buffer, int p_buffer_len) {
	ERR_FAIL_NULL_V(p_image, ERR_INVALID_PARAMETER);

	JxlDecoder *dec = JxlDecoderCreate(nullptr);
	if (JxlDecoderSubscribeEvents(dec, JXL_DEC_BASIC_INFO | JXL_DEC_FULL_IMAGE) != JXL_DEC_SUCCESS) {
		JxlDecoderDestroy(dec);
		ERR_FAIL_V_MSG(ERR_UNAVAILABLE, "Failed to subscribe to JXL decoder events.");
	}

	if (JxlDecoderSetInput(dec, p_buffer, p_buffer_len) != JXL_DEC_SUCCESS) {
		JxlDecoderDestroy(dec);
		ERR_FAIL_V_MSG(ERR_UNAVAILABLE, "Failed to set JXL decoder input.");
	}
	JxlDecoderCloseInput(dec);

	JxlPixelFormat pixel_format = { 4, JXL_TYPE_UINT8, JXL_NATIVE_ENDIAN, 0 };
	Vector<uint8_t> dst_image;
	Size2 size;
	int channels = 0;

	while (true) {
		JxlDecoderStatus status = JxlDecoderProcessInput(dec);

		if (status == JXL_DEC_ERROR) {
			JxlDecoderDestroy(dec);
			ERR_FAIL_V_MSG(ERR_FILE_CORRUPT, "Failed to decode JXL image.");
		}

		if (status == JXL_DEC_NEED_MORE_INPUT) {
			JxlDecoderDestroy(dec);
			ERR_FAIL_V_MSG(ERR_FILE_EOF, "JXL decoder needs more input.");
		}

		if (status == JXL_DEC_BASIC_INFO) {
			JxlBasicInfo basic_info;
			if (JxlDecoderGetBasicInfo(dec, &basic_info) != JXL_DEC_SUCCESS) {
				JxlDecoderDestroy(dec);
				ERR_FAIL_V_MSG(ERR_FILE_CORRUPT, "Failed to get JXL basic info.");
			}
			size.width = basic_info.xsize;
			size.height = basic_info.ysize;
			channels = (basic_info.alpha_bits > 0) ? 4 : 3;
			pixel_format.num_channels = channels;

		} else if (status == JXL_DEC_NEED_IMAGE_OUT_BUFFER) {
			size_t buffer_size = 0;
			if (JxlDecoderImageOutBufferSize(dec, &pixel_format, &buffer_size) != JXL_DEC_SUCCESS) {
				JxlDecoderDestroy(dec);
				ERR_FAIL_V_MSG(ERR_FILE_CORRUPT, "Failed to get JXL image out buffer size.");
			}

			dst_image.resize(buffer_size);
			if (JxlDecoderSetImageOutBuffer(dec, &pixel_format, dst_image.ptrw(), dst_image.size()) != JXL_DEC_SUCCESS) {
				JxlDecoderDestroy(dec);
				ERR_FAIL_V_MSG(ERR_UNAVAILABLE, "Failed to set JXL image out buffer.");
			}
		} else if (status == JXL_DEC_SUCCESS || status == JXL_DEC_FULL_IMAGE) {
			break;
		} else { // Unknown format?
			JxlDecoderDestroy(dec);
			ERR_FAIL_V_MSG(ERR_FILE_UNRECOGNIZED, "Unknown JXL decoder status.");
		}
	}

	JxlDecoderDestroy(dec);

	p_image->set_data(size.width, size.height, false, channels == 4 ? Image::FORMAT_RGBA8 : Image::FORMAT_RGB8, dst_image);
	return OK;
}
} //namespace JXLCommon
