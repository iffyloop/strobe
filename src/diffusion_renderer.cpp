#include "pch.h"

#include "diffusion_renderer.h"
#include "io_utils.h"

namespace {

constexpr bool k_diffusion_disable_metal_shared_buffers = false;
constexpr bool k_diffusion_disable_metal_residency_sets = false;
constexpr bool k_diffusion_disable_metal_concurrency = false;
constexpr bool k_diffusion_disable_metal_graph_optimize = false;
constexpr bool k_diffusion_keep_clip_on_cpu = false;
constexpr bool k_diffusion_enable_controlnet = true;
constexpr bool k_diffusion_verbose_logs = false;
constexpr bool k_diffusion_fuse_lora_on_load = true;
constexpr bool k_diffusion_enable_diffusion_flash_attn = true;

void diffusion_log_callback(enum sd_log_level_t level, const char* text, void* data) {
	UNUSED(data);
	if (text == nullptr) {
		return;
	}
	if constexpr (!k_diffusion_verbose_logs) {
		if (level == SD_LOG_DEBUG) {
			return;
		}
	}

	char const* prefix = "[sd][info]";
	switch (level) {
	case SD_LOG_DEBUG:
		prefix = "[sd][debug]";
		break;
	case SD_LOG_INFO:
		prefix = "[sd][info]";
		break;
	case SD_LOG_WARN:
		prefix = "[sd][warn]";
		break;
	case SD_LOG_ERROR:
		prefix = "[sd][error]";
		break;
	}

	std::cerr << prefix << ' ' << text << std::endl;
}

std::string resolve_model_path(char const* filename) {
	std::array<std::filesystem::path, 4> const roots = {
		std::filesystem::path("models"),
		std::filesystem::path("../models"),
		std::filesystem::path("../../models"),
		std::filesystem::path(get_resources_prefix()) / "models",
	};

	for (std::filesystem::path const& root : roots) {
		std::filesystem::path const candidate = root / filename;
		if (std::filesystem::exists(candidate)) {
			return candidate.lexically_normal().string();
		}
	}

	return {};
}

std::array<std::filesystem::path, 4> model_roots() {
	return {
		std::filesystem::path("models"),
		std::filesystem::path("../models"),
		std::filesystem::path("../../models"),
		std::filesystem::path(get_resources_prefix()) / "models",
	};
}

std::string resolve_model_path_prefix(char const* prefix) {
	auto roots = model_roots();
	for (std::filesystem::path const& root : roots) {
		if (!std::filesystem::exists(root) || !std::filesystem::is_directory(root)) {
			continue;
		}

		std::vector<std::filesystem::path> matches;
		for (auto const& entry : std::filesystem::directory_iterator(root)) {
			if (!entry.is_regular_file()) {
				continue;
			}
			auto const filename = entry.path().filename().string();
			if (filename.rfind(prefix, 0) == 0 && entry.path().extension() == ".safetensors") {
				matches.push_back(entry.path());
			}
		}

		if (!matches.empty()) {
			std::sort(matches.begin(), matches.end());
			return matches.front().lexically_normal().string();
		}
	}

	return {};
}

std::string resolve_first_existing(
	std::initializer_list<char const*> candidates, std::initializer_list<char const*> prefixes = {}) {
	for (char const* candidate : candidates) {
		auto path = resolve_model_path(candidate);
		if (!path.empty()) {
			return path;
		}
	}

	for (char const* prefix : prefixes) {
		auto path = resolve_model_path_prefix(prefix);
		if (!path.empty()) {
			return path;
		}
	}

	return {};
}

bool validate_required_models(diffusion_renderer_t& renderer, diffusion_model_t const model) {
	renderer.model_path.clear();
	renderer.hyper_lora_path.clear();
	renderer.taesd_decoder_path.clear();
	renderer.controlnet_path.clear();

	if (model == diffusion_model_t::SD15) {
		renderer.model_path = resolve_first_existing(
			{
				"v1-5-pruned-emaonly.safetensors",
				"sd-v1-5.safetensors",
				"sd15.safetensors",
			},
			{"v1-5", "sd-v1-5", "sd15", "stable-diffusion-v1-5"});

		renderer.hyper_lora_path = resolve_first_existing(
			{
				"Hyper-SD15-1step-lora.safetensors",
				"Hyper-SD15-1step-LoRA.safetensors",
				"Hyper-SD15-1step.safetensors",
			},
			{"Hyper-SD15-1step", "Hyper-SD15", "hyper-sd15"});

		renderer.taesd_decoder_path = resolve_first_existing(
			{
				"taesd_decoder.safetensors",
				"taesd.sd_decoder.safetensors",
			},
			{"taesd_decoder", "taesd.sd_decoder", "taesd"});

		renderer.controlnet_path = resolve_first_existing(
			{
				"sd15_controlnet_depth.safetensors",
				"control_sd15_depth.safetensors",
			},
			{"sd15_controlnet_depth", "control_sd15_depth"});

		if (renderer.model_path.empty()) {
			std::cerr << "Missing SD 1.5 base model (expected prefix like v1-5... or sd15...)" << std::endl;
			return false;
		}
		if (renderer.hyper_lora_path.empty()) {
			std::cerr << "Optional model missing: Hyper-SD15..." << std::endl;
		}
		if (renderer.taesd_decoder_path.empty()) {
			std::cerr << "Missing TAESD decoder for SD 1.5 (expected prefix like taesd_decoder...)"
				  << std::endl;
			return false;
		}
		if (renderer.controlnet_path.empty()) {
			std::cerr << "Missing SD 1.5 depth ControlNet (expected prefix like sd15_controlnet_depth...)"
				  << std::endl;
			return false;
		}
		return true;
	}

	renderer.model_path = resolve_first_existing(
		{
			"sd_xl_base_1.0.safetensors",
			"sdxl_base_1.0.safetensors",
		},
		{"sd_xl_base_1.0", "sdxl_base_1.0", "sdxl_base"});

	renderer.hyper_lora_path = resolve_first_existing(
		{
			"Hyper-SDXL-1step-lora.safetensors",
			"Hyper-SDXL-1step.safetensors",
		},
		{"Hyper-SDXL"});

	renderer.taesd_decoder_path = resolve_first_existing(
		{
			"taesdxl_diffusion_pytorch_model.safetensors",
			"taesdxl_decoder.safetensors",
		},
		{"taesdxl_diffusion", "taesdxl"});

	renderer.controlnet_path = resolve_first_existing(
		{
			"sdxl_depth_controlnet.safetensors",
			"sdxl_depth_controlnet.fp32.safetensors",
		},
		{"sdxl_depth_controlnet", "sdxl_controlnet_depth"});

	if (renderer.model_path.empty()) {
		std::cerr << "Missing SDXL base model (expected prefix like sd_xl_base_1.0...)" << std::endl;
		return false;
	}
	if (renderer.hyper_lora_path.empty()) {
		std::cerr << "Optional model missing: Hyper-SDXL..." << std::endl;
	}
	if (renderer.taesd_decoder_path.empty()) {
		std::cerr << "Missing TAESDXL model (expected prefix like taesdxl_diffusion...)" << std::endl;
		return false;
	}
	if (renderer.controlnet_path.empty()) {
		std::cerr << "Missing SDXL depth ControlNet (expected prefix like sdxl_depth_controlnet...)"
			  << std::endl;
		return false;
	}

	return true;
}

} // namespace

bool diffusion_renderer_setup(
	diffusion_renderer_t& renderer, diffusion_model_t const model, s32 target_width, s32 target_height) {
	diffusion_renderer_destroy(renderer);

	renderer.target_width = target_width;
	renderer.target_height = target_height;

	if (!validate_required_models(renderer, model)) {
		return false;
	}

	sd_ctx_params_t ctx_params = {};
	sd_ctx_params_init(&ctx_params);
	if constexpr (k_diffusion_disable_metal_shared_buffers) {
		setenv("GGML_METAL_SHARED_BUFFERS_DISABLE", "1", 1);
	}
	if constexpr (k_diffusion_disable_metal_residency_sets) {
		setenv("GGML_METAL_NO_RESIDENCY", "1", 1);
	}
	if constexpr (k_diffusion_disable_metal_concurrency) {
		setenv("GGML_METAL_CONCURRENCY_DISABLE", "1", 1);
	}
	if constexpr (k_diffusion_disable_metal_graph_optimize) {
		setenv("GGML_METAL_GRAPH_OPTIMIZE_DISABLE", "1", 1);
	}
	sd_set_log_callback(diffusion_log_callback, nullptr);
	ctx_params.model_path = renderer.model_path.c_str();
	s32 const physical_cores = std::max(1, sd_get_num_physical_cores());
	ctx_params.n_threads = std::max(1, std::min(8, physical_cores - 2));
	ctx_params.wtype = SD_TYPE_F16;
	ctx_params.enable_mmap = true;
	ctx_params.offload_params_to_cpu = false;
	ctx_params.keep_clip_on_cpu = k_diffusion_keep_clip_on_cpu;
	ctx_params.keep_control_net_on_cpu = false;
	ctx_params.keep_vae_on_cpu = false;
	ctx_params.flash_attn = false;
	ctx_params.diffusion_flash_attn = k_diffusion_enable_diffusion_flash_attn;
	ctx_params.vae_decode_only = true;
	ctx_params.free_params_immediately = false;
	ctx_params.lora_apply_mode = k_diffusion_fuse_lora_on_load ? LORA_APPLY_IMMEDIATELY : LORA_APPLY_AT_RUNTIME;

	ctx_params.control_net_path = k_diffusion_enable_controlnet ? renderer.controlnet_path.c_str() : nullptr;
	ctx_params.taesd_path = renderer.taesd_decoder_path.c_str();
	std::cerr << "Using model: " << renderer.model_path << std::endl;
	std::cerr << "Using TAESD: " << renderer.taesd_decoder_path << std::endl;
	if (k_diffusion_enable_controlnet) {
		std::cerr << "Using ControlNet: " << renderer.controlnet_path << std::endl;
	} else {
		std::cerr << "ControlNet disabled for isolate" << std::endl;
	}
	std::cerr << "Diffusion triage config:"
		  << " clip_on_cpu=" << (ctx_params.keep_clip_on_cpu ? 1 : 0) << " lora_mode="
		  << (ctx_params.lora_apply_mode == LORA_APPLY_IMMEDIATELY ? "immediate-fuse" : "runtime-adapter")
		  << " shared_buffers_disabled=" << (k_diffusion_disable_metal_shared_buffers ? 1 : 0)
		  << " residency_disabled=" << (k_diffusion_disable_metal_residency_sets ? 1 : 0)
		  << " concurrency_disabled=" << (k_diffusion_disable_metal_concurrency ? 1 : 0)
		  << " graph_opt_disabled=" << (k_diffusion_disable_metal_graph_optimize ? 1 : 0) << std::endl;
	renderer.sd_ctx = new_sd_ctx(&ctx_params);
	renderer.controlnet_enabled = renderer.sd_ctx != nullptr;
	renderer.taesd_enabled = renderer.sd_ctx != nullptr;

	if (renderer.sd_ctx == nullptr) {
		std::cerr << "Failed to initialize stable-diffusion.cpp context" << std::endl;
		return false;
	}

	if (renderer.use_hyper_lora && !renderer.hyper_lora_path.empty()) {
		sd_lora_t hyper_lora = {};
		hyper_lora.path = renderer.hyper_lora_path.c_str();
		hyper_lora.multiplier = 1.0f;
		hyper_lora.is_high_noise = false;
		sd_apply_loras(renderer.sd_ctx, &hyper_lora, 1);
		std::cerr << "Applied Hyper LoRA: " << renderer.hyper_lora_path << std::endl;
	}

	sd_sample_params_init(&renderer.sample_params);
	renderer.sample_params.sample_method = TCD_SAMPLE_METHOD;
	renderer.sample_params.scheduler = SCHEDULER_COUNT;
	renderer.sample_params.sample_steps = 1;
	renderer.sample_params.guidance.txt_cfg = 1.0f;
	renderer.sample_params.eta = 1.0f;
	renderer.sample_params.shifted_timestep = 0;

	sd_cache_params_init(&renderer.cache_params);
	renderer.next_seed = static_cast<s64>(std::chrono::high_resolution_clock::now().time_since_epoch().count());

	return true;
}

bool diffusion_renderer_setup(diffusion_renderer_t& renderer, s32 target_width, s32 target_height) {
	return diffusion_renderer_setup(renderer, diffusion_model_t::SDXL, target_width, target_height);
}

bool diffusion_renderer_render(diffusion_renderer_t& renderer, std::span<float const> depth_map, s32 depth_width,
	s32 depth_height, std::string const& prompt, std::string const& negative_prompt, s64 seed,
	std::vector<u8>& out_pixels, s32& out_width, s32& out_height, s32& out_channels) {
	if (renderer.sd_ctx == nullptr) {
		std::cerr << "diffusion_renderer_setup must be called before rendering" << std::endl;
		return false;
	}

	if (depth_width <= 0 || depth_height <= 0) {
		std::cerr << "invalid depth map dimensions" << std::endl;
		return false;
	}

	size_t const expected_depth_size = static_cast<size_t>(depth_width) * static_cast<size_t>(depth_height);
	if (depth_map.size() != expected_depth_size) {
		std::cerr << "depth map size mismatch" << std::endl;
		return false;
	}

	s32 const target_width = renderer.target_width;
	s32 const target_height = renderer.target_height;
	if (target_width <= 0 || target_height <= 0) {
		std::cerr << "invalid diffusion target dimensions" << std::endl;
		return false;
	}

	size_t const control_size = static_cast<size_t>(target_width) * static_cast<size_t>(target_height) * 3;
	if (renderer.control_pixels.size() != control_size) {
		renderer.control_pixels.resize(control_size);
	}
	u8* control_pixels = renderer.control_pixels.data();
	for (s32 y = 0; y < target_height; ++y) {
		f32 const src_yf = ((static_cast<f32>(y) + 0.5f) / static_cast<f32>(target_height)) *
			static_cast<f32>(depth_height);
		s32 const src_y = std::clamp(static_cast<s32>(src_yf), 0, depth_height - 1);
		for (s32 x = 0; x < target_width; ++x) {
			f32 const src_xf = ((static_cast<f32>(x) + 0.5f) / static_cast<f32>(target_width)) *
				static_cast<f32>(depth_width);
			s32 const src_x = std::clamp(static_cast<s32>(src_xf), 0, depth_width - 1);
			size_t const src_index = static_cast<size_t>(src_y) * static_cast<size_t>(depth_width) +
				static_cast<size_t>(src_x);
			f32 const depth_value = std::clamp(depth_map[src_index], 0.0f, 1.0f);
			u8 const depth_u8 = static_cast<u8>(depth_value * 255.0f + 0.5f);
			size_t const dst_index =
				(static_cast<size_t>(y) * static_cast<size_t>(target_width) + static_cast<size_t>(x)) *
				3;
			control_pixels[dst_index + 0] = depth_u8;
			control_pixels[dst_index + 1] = depth_u8;
			control_pixels[dst_index + 2] = depth_u8;
		}
	}

	sd_img_gen_params_t img_params = {};
	sd_img_gen_params_init(&img_params);
	img_params.prompt = prompt.c_str();
	img_params.negative_prompt = negative_prompt.c_str();
	img_params.width = target_width;
	img_params.height = target_height;
	img_params.sample_params = renderer.sample_params;
	img_params.seed = seed;
	img_params.batch_count = 1;
	img_params.control_image = {
		static_cast<u32>(target_width),
		static_cast<u32>(target_height),
		3,
		control_pixels,
	};
	img_params.control_strength = renderer.control_strength;
	img_params.cache = renderer.cache_params;
	if constexpr (k_diffusion_verbose_logs) {
		std::cerr << "[diffusion] generate seed=" << img_params.seed << " lora_count=" << img_params.lora_count
			  << " control=" << depth_width << "x" << depth_height << " target=" << target_width << "x"
			  << target_height << std::endl;
	}

	sd_image_t* images = generate_image(renderer.sd_ctx, &img_params);
	if (images == nullptr || images[0].data == nullptr) {
		std::cerr << "stable-diffusion.cpp image generation failed" << std::endl;
		if (images != nullptr) {
			free(images);
		}
		return false;
	}

	out_width = static_cast<s32>(images[0].width);
	out_height = static_cast<s32>(images[0].height);
	out_channels = static_cast<s32>(images[0].channel);
	out_pixels.assign(images[0].data, images[0].data + out_width * out_height * out_channels);

	free(images[0].data);
	free(images);

	return true;
}

bool dffusion_renderer_render_unconditional(diffusion_renderer_t& renderer, std::string const& prompt, s64 seed,
	std::vector<u8>& out_pixels, s32& out_width, s32& out_height, s32& out_channels) {
	if (renderer.sd_ctx == nullptr) {
		std::cerr << "diffusion_renderer_setup must be called before rendering" << std::endl;
		return false;
	}

	sd_img_gen_params_t img_params = {};
	sd_img_gen_params_init(&img_params);
	img_params.prompt = prompt.c_str();
	img_params.negative_prompt = "";
	img_params.width = renderer.target_width;
	img_params.height = renderer.target_height;
	img_params.sample_params = renderer.sample_params;
	img_params.seed = seed;
	img_params.batch_count = 1;
	img_params.cache = renderer.cache_params;

	sd_image_t* images = generate_image(renderer.sd_ctx, &img_params);
	if (images == nullptr || images[0].data == nullptr) {
		std::cerr << "stable-diffusion.cpp image generation failed" << std::endl;
		if (images != nullptr) {
			free(images);
		}
		return false;
	}

	out_width = static_cast<s32>(images[0].width);
	out_height = static_cast<s32>(images[0].height);
	out_channels = static_cast<s32>(images[0].channel);
	out_pixels.assign(images[0].data, images[0].data + out_width * out_height * out_channels);

	free(images[0].data);
	free(images);

	return true;
}

void diffusion_renderer_destroy(diffusion_renderer_t& renderer) {
	if (renderer.sd_ctx != nullptr) {
		free_sd_ctx(renderer.sd_ctx);
		renderer.sd_ctx = nullptr;
	}
}
