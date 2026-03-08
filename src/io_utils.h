#pragma once

#include "pch.h"

template <typename T>
void flip_image_y(std::vector<T>& out, std::vector<T> const& img, s32 const width, s32 const height, s32 const chans) {
	out.resize(width * height * chans);
	for (s32 j = 0; j < height; ++j) {
		for (s32 i = 0; i < width * chans; ++i) {
			out[(height - 1 - j) * width * chans + i] = img[j * width * chans + i];
		}
	}
}

template <typename CONTAINER> void load_file(CONTAINER& container, std::string const& filename) {
	std::ifstream in_file(filename, std::ios::binary | std::ios::ate);
	if (!in_file) {
		assert_release(false);
	}
	std::streampos const num_bytes = in_file.tellg();
	container.clear();
	container.resize(num_bytes / sizeof(typename CONTAINER::value_type));
	in_file.seekg(0, std::ios::beg);
	in_file.read(reinterpret_cast<char*>(container.data()), num_bytes);
}

template <typename CONTAINER> void write_file(std::string const& filename, CONTAINER const& container) {
	std::ofstream out_file(filename, std::ios::binary);
	if (!out_file) {
		assert_release(false);
	}
	out_file.write(reinterpret_cast<char const*>(container.data()),
		container.size() * sizeof(typename CONTAINER::value_type));
}

void load_image(std::vector<u8>& out, u32& out_w, u32& out_h, std::string const& filename, s32 const desired_chans);
void write_image(
	std::string const& filename, s32 const w, s32 const h, s32 const chans, u8 const* data, s32 const stride = 0);
void write_image_upload_item(
	std::string& output, s32 const w, s32 const h, s32 const chans, u8 const* data, s32 const stride = 0);
void split_rgba_to_rgb_mask(std::vector<u8>& rgb, std::vector<u8>& mask, std::vector<u8> const& rgba);
u32 random_u32();

std::string get_resources_prefix();
