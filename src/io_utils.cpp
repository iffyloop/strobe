#include "pch.h"

#include "io_utils.h"

void load_image(std::vector<u8>& out, u32& out_w, u32& out_h, std::string const& filename, s32 const desired_chans) {
	s32 actual_chans = 0;
	s32 w = 0, h = 0;
	u8* decoded = stbi_load(filename.c_str(), &w, &h, &actual_chans, desired_chans);
	out_w = w;
	out_h = h;
	if (!decoded || !out_w || !out_h) {
		assert_release(false);
	}
	out.assign(decoded, decoded + out_w * out_h * desired_chans);
	stbi_image_free(decoded);
}

void write_image(
	std::string const& filename, s32 const w, s32 const h, s32 const chans, u8 const* data, s32 const stride) {
	stbi_write_png(filename.data(), w, h, chans, data, stride);
}

static void write_to_memory_for_stbi(void* context, void* data, int size) {
	std::string* out = reinterpret_cast<std::string*>(context);
	out->clear();
	out->append(reinterpret_cast<const char*>(data), size);
}

void write_image_upload_item(
	std::string& output, s32 const w, s32 const h, s32 const chans, u8 const* data, s32 const stride) {
	stbi_write_png_to_func(write_to_memory_for_stbi, &output, w, h, chans, data, stride);
}

void split_rgba_to_rgb_mask(std::vector<u8>& rgb, std::vector<u8>& mask, std::vector<u8> const& rgba) {
	u32 const numPixels = static_cast<u32>(rgba.size() / 4);
	rgb.resize(numPixels * 3);
	mask.resize(numPixels);
	for (u32 i = 0; i < numPixels; ++i) {
		rgb[3 * i + 0] = rgba[4 * i + 0];
		rgb[3 * i + 1] = rgba[4 * i + 1];
		rgb[3 * i + 2] = rgba[4 * i + 2];
		mask[i] = 255 - rgba[4 * i + 3]; // Invert alpha to get mask
	}
}

u32 random_u32() {
	static std::mt19937 rng(std::random_device{}());
	return rng();
}

std::string get_resources_prefix() {
#ifdef __APPLE__
#ifndef DEBUG
	return "../Resources/";
#endif
#endif

	return "";
}
