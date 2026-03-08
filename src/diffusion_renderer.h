#pragma once

#include "pch.h"

#include <strobe-stable-diffusion.cpp/include/stable-diffusion.h>

#include <span>

struct sd_ctx_t;

enum class diffusion_model_t {
	SDXL = 0,
	SD15 = 1,
};

struct diffusion_renderer_t {
	sd_ctx_t* sd_ctx = nullptr;
	std::string model_path;
	std::string hyper_lora_path;
	std::string taesd_decoder_path;
	std::string controlnet_path;
	bool use_hyper_lora = true;
	bool taesd_enabled = false;
	bool controlnet_enabled = false;
	s32 target_width = 1024;
	s32 target_height = 1024;
	f32 control_strength = 0.5f;
	s64 next_seed = 1;
	sd_sample_params_t sample_params = {};
	sd_cache_params_t cache_params = {};
	std::vector<u8> control_pixels;
};

bool diffusion_renderer_setup(diffusion_renderer_t& renderer, s32 target_width, s32 target_height);

bool diffusion_renderer_setup(
	diffusion_renderer_t& renderer, diffusion_model_t model, s32 target_width, s32 target_height);

bool diffusion_renderer_render(diffusion_renderer_t& renderer, std::span<float const> depth_map, s32 depth_width,
	s32 depth_height, std::string const& prompt, std::string const& negative_prompt, s64 seed,
	std::vector<u8>& out_pixels, s32& out_width, s32& out_height, s32& out_channels);

bool dffusion_renderer_render_unconditional(diffusion_renderer_t& renderer, std::string const& prompt, s64 seed,
	std::vector<u8>& out_pixels, s32& out_width, s32& out_height, s32& out_channels);

void diffusion_renderer_destroy(diffusion_renderer_t& renderer);
