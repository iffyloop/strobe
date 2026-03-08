#include "pch.h"

#include "diffusion_renderer.h"
#include "io_utils.h"

int main() {
	constexpr bool k_fresh_context_per_frame = false;

	s32 constexpr control_width = 1024;
	s32 constexpr control_height = 1024;

	std::vector<float> control_depth;
	control_depth.resize(static_cast<size_t>(control_width) * static_cast<size_t>(control_height), 0.0f);

	s32 const square_size = std::min(control_width, control_height) / 3;
	s32 const square_min_x = (control_width - square_size) / 2;
	s32 const square_max_x = square_min_x + square_size;
	s32 const square_min_y = (control_height - square_size) / 2;
	s32 const square_max_y = square_min_y + square_size;

	for (s32 y = square_min_y; y < square_max_y; ++y) {
		for (s32 x = square_min_x; x < square_max_x; ++x) {
			control_depth[static_cast<size_t>(y) * static_cast<size_t>(control_width) + static_cast<size_t>(x)] =
				1.0f;
		}
	}

	std::vector<u8> control_preview;
	control_preview.resize(static_cast<size_t>(control_width) * static_cast<size_t>(control_height) * 3);
	for (size_t i = 0; i < static_cast<size_t>(control_width) * static_cast<size_t>(control_height); ++i) {
		u8 const value = static_cast<u8>(std::clamp(control_depth[i], 0.0f, 1.0f) * 255.0f + 0.5f);
		control_preview[i * 3 + 0] = value;
		control_preview[i * 3 + 1] = value;
		control_preview[i * 3 + 2] = value;
	}
	write_image("output/diffusion_control_dummy.png", control_width, control_height, 3, control_preview.data());

	diffusion_renderer_t shared_renderer;
	if constexpr (!k_fresh_context_per_frame) {
		if (!diffusion_renderer_setup(shared_renderer, control_width, control_height)) {
			std::cerr << "diffusion_renderer_setup failed" << std::endl;
			return 1;
		}
	}

	for (s32 frame_index = 0; frame_index < 3; ++frame_index) {
		diffusion_renderer_t frame_renderer;
		diffusion_renderer_t* renderer_ptr = nullptr;
		if constexpr (k_fresh_context_per_frame) {
			if (!diffusion_renderer_setup(frame_renderer, control_width, control_height)) {
				std::cerr << "diffusion_renderer_setup failed on frame " << frame_index << std::endl;
				return 1;
			}
			renderer_ptr = &frame_renderer;
		} else {
			renderer_ptr = &shared_renderer;
		}

		std::vector<u8> pixels;
		s32 width = 0;
		s32 height = 0;
		s32 channels = 0;
		bool const ok = diffusion_renderer_render(*renderer_ptr, control_depth, control_width, control_height,
			"highly detailed cinematic render of smooth geometric sculpture", "", 1337 + frame_index, pixels, width, height,
			channels);

		if constexpr (k_fresh_context_per_frame) {
			diffusion_renderer_destroy(frame_renderer);
		}

		if (!ok || pixels.empty()) {
			std::cerr << "diffusion_renderer_render failed on frame " << frame_index << std::endl;
			if constexpr (!k_fresh_context_per_frame) {
				diffusion_renderer_destroy(shared_renderer);
			}
			return 1;
		}

		std::string const output_path = "output/diffusion_smoke_test_" + std::to_string(frame_index) + ".png";
		write_image(output_path, width, height, channels, pixels.data());
		std::cout << "Wrote " << output_path << " (" << width << "x" << height << ")" << std::endl;
	}

	if constexpr (!k_fresh_context_per_frame) {
		diffusion_renderer_destroy(shared_renderer);
	}

	std::cout << "Wrote output/diffusion_control_dummy.png (" << control_width << "x" << control_height << ")"
		      << std::endl;
	return 0;
}
