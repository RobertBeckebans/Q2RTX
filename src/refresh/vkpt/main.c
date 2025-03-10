/*
Copyright (C) 2018 Christoph Schied
Copyright (C) 2019, NVIDIA CORPORATION. All rights reserved.

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License along
with this program; if not, write to the Free Software Foundation, Inc.,
51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
*/

#include "shared/shared.h"
#include "common/bsp.h"
#include "common/cmd.h"
#include "common/common.h"
#include "common/cvar.h"
#include "common/files.h"
#include "common/math.h"
#include "client/video.h"
#include "client/client.h"
#include "refresh/refresh.h"
#include "refresh/images.h"
#include "refresh/models.h"
#include "system/hunk.h"
#include "vkpt.h"
#include "material.h"
#include "physical_sky.h"
#include "../../client/client.h"
#include "../../client/ui/ui.h"

#include "shader/vertex_buffer.h"

#include <vulkan/vulkan.h>
#include <SDL.h>
#include <SDL_vulkan.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

cvar_t *cvar_profiler = NULL;
cvar_t *cvar_vsync = NULL;
cvar_t *cvar_pt_caustics = NULL;
cvar_t *cvar_pt_enable_nodraw = NULL;
cvar_t *cvar_pt_enable_surface_lights = NULL;
cvar_t *cvar_pt_enable_surface_lights_warp = NULL;
cvar_t* cvar_pt_surface_lights_fake_emissive_algo = NULL;
cvar_t* cvar_pt_surface_lights_threshold = NULL;
cvar_t *cvar_pt_bsp_radiance_scale = NULL;
cvar_t *cvar_pt_accumulation_rendering = NULL;
cvar_t *cvar_pt_accumulation_rendering_framenum = NULL;
cvar_t *cvar_pt_projection = NULL;
cvar_t *cvar_pt_dof = NULL;
cvar_t* cvar_pt_freecam = NULL;
cvar_t *cvar_pt_nearest = NULL;
cvar_t *cvar_drs_enable = NULL;
cvar_t *cvar_drs_target = NULL;
cvar_t *cvar_drs_minscale = NULL;
cvar_t *cvar_drs_maxscale = NULL;
cvar_t *cvar_drs_adjust_up = NULL;
cvar_t *cvar_drs_adjust_down = NULL;
cvar_t *cvar_drs_gain = NULL;
cvar_t *cvar_tm_blend_enable = NULL;
extern cvar_t *scr_viewsize;
extern cvar_t *cvar_bloom_enable;
extern cvar_t* cvar_flt_taa;
static int drs_current_scale = 0;
static int drs_effective_scale = 0;

cvar_t* cvar_min_driver_version_nvidia = NULL;
cvar_t* cvar_min_driver_version_amd = NULL;
cvar_t *cvar_ray_tracing_api = NULL;
cvar_t *cvar_vk_validation = NULL;

extern uiStatic_t uis;

#ifdef VKPT_DEVICE_GROUPS
cvar_t *cvar_sli = NULL;
#endif

#ifdef VKPT_IMAGE_DUMPS
cvar_t *cvar_dump_image = NULL;
#endif

byte cluster_debug_mask[VIS_MAX_BYTES];
int cluster_debug_index;

#define UBO_CVAR_DO(name, default_value) cvar_t *cvar_##name;
UBO_CVAR_LIST
#undef UBO_CVAR_DO

static bsp_t *bsp_world_model;

static qboolean temporal_frame_valid = qfalse;

static int world_anim_frame = 0;

static vec3_t avg_envmap_color = { 0.f };

static image_t *water_normal_texture = NULL;

int num_accumulated_frames = 0;

static qboolean frame_ready = qfalse;

static float sky_rotation = 0.f;
static vec3_t sky_axis = { 0.f };

#define NUM_TAA_SAMPLES 128
static vec2_t taa_samples[NUM_TAA_SAMPLES];

typedef enum {
	VKPT_INIT_DEFAULT            = (0),
	VKPT_INIT_SWAPCHAIN_RECREATE = (1 << 1),
	VKPT_INIT_RELOAD_SHADER      = (1 << 2),
} VkptInitFlags_t;

typedef struct VkptInit_s {
	const char *name;
	VkResult (*initialize)();
	VkResult (*destroy)();
	VkptInitFlags_t flags;
	int is_initialized;
} VkptInit_t;
VkptInit_t vkpt_initialization[] = {
	{ "profiler", vkpt_profiler_initialize,            vkpt_profiler_destroy,                VKPT_INIT_DEFAULT,            0 },
	{ "vbo",      vkpt_vertex_buffer_create,           vkpt_vertex_buffer_destroy,           VKPT_INIT_DEFAULT,            0 },
	{ "ubo",      vkpt_uniform_buffer_create,          vkpt_uniform_buffer_destroy,          VKPT_INIT_DEFAULT,            0 },
	{ "textures", vkpt_textures_initialize,            vkpt_textures_destroy,                VKPT_INIT_DEFAULT,            0 },
	{ "shadowmap", 	vkpt_shadow_map_initialize,        vkpt_shadow_map_destroy,              VKPT_INIT_DEFAULT,            0 },
	{ "shadowmap|", vkpt_shadow_map_create_pipelines,  vkpt_shadow_map_destroy_pipelines,    VKPT_INIT_RELOAD_SHADER ,     0 },
	{ "images",   vkpt_create_images,                  vkpt_destroy_images,                  VKPT_INIT_SWAPCHAIN_RECREATE, 0 },
	{ "draw",     vkpt_draw_initialize,                vkpt_draw_destroy,                    VKPT_INIT_DEFAULT,            0 },
	{ "pt",       vkpt_pt_init,                        vkpt_pt_destroy,                      VKPT_INIT_DEFAULT,            0 },
	{ "pt|",      vkpt_pt_create_pipelines,            vkpt_pt_destroy_pipelines,            VKPT_INIT_RELOAD_SHADER,      0 },
	{ "draw|",    vkpt_draw_create_pipelines,          vkpt_draw_destroy_pipelines,          VKPT_INIT_SWAPCHAIN_RECREATE
	                                                                                       | VKPT_INIT_RELOAD_SHADER,      0 },
	{ "vbo|",     vkpt_vertex_buffer_create_pipelines, vkpt_vertex_buffer_destroy_pipelines, VKPT_INIT_RELOAD_SHADER,      0 },
	{ "asvgf",    vkpt_asvgf_initialize,               vkpt_asvgf_destroy,                   VKPT_INIT_DEFAULT,            0 },
	{ "asvgf|",   vkpt_asvgf_create_pipelines,         vkpt_asvgf_destroy_pipelines,         VKPT_INIT_RELOAD_SHADER,      0 },
	{ "bloom",    vkpt_bloom_initialize,               vkpt_bloom_destroy,                   VKPT_INIT_DEFAULT,            0 },
	{ "bloom|",   vkpt_bloom_create_pipelines,         vkpt_bloom_destroy_pipelines,         VKPT_INIT_RELOAD_SHADER,      0 },
	{ "tonemap",  vkpt_tone_mapping_initialize,        vkpt_tone_mapping_destroy,            VKPT_INIT_DEFAULT,            0 },
	{ "tonemap|", vkpt_tone_mapping_create_pipelines,  vkpt_tone_mapping_destroy_pipelines,  VKPT_INIT_RELOAD_SHADER,      0 },

	{ "physicalSky", vkpt_physical_sky_initialize,         vkpt_physical_sky_destroy,            VKPT_INIT_DEFAULT,        0 },
	{ "physicalSky|", vkpt_physical_sky_create_pipelines,  vkpt_physical_sky_destroy_pipelines,  VKPT_INIT_RELOAD_SHADER,  0 },
	{ "godrays",    vkpt_initialize_god_rays,           vkpt_destroy_god_rays,              VKPT_INIT_DEFAULT,             0 },
	{ "godrays|",   vkpt_god_rays_create_pipelines,     vkpt_god_rays_destroy_pipelines,    VKPT_INIT_RELOAD_SHADER,       0 },
	{ "godraysI",   vkpt_god_rays_update_images,        vkpt_god_rays_noop,                 VKPT_INIT_SWAPCHAIN_RECREATE,  0 },
};

void debug_output(const char* format, ...);
static void recreate_swapchain();

static void viewsize_changed(cvar_t *self)
{
	Cvar_ClampInteger(scr_viewsize, 25, 200);
	Com_Printf("Resolution scale: %d%%\n", scr_viewsize->integer);
}

static void pt_nearest_changed(cvar_t* self)
{
	vkpt_invalidate_texture_descriptors();
}

static void drs_target_changed(cvar_t *self)
{
	Cvar_ClampInteger(self, 30, 240);
}

static void drs_minscale_changed(cvar_t *self)
{
	Cvar_ClampInteger(self, 25, 100);
}

static void drs_maxscale_changed(cvar_t *self)
{
	Cvar_ClampInteger(self, 50, 200);
}

static void accumulation_cvar_changed(cvar_t* self)
{
	// Reset accumulation rendering on DoF parameter change
	num_accumulated_frames = 0;
}

static inline qboolean extents_equal(VkExtent2D a, VkExtent2D b)
{
	return a.width == b.width && a.height == b.height;
}

static VkExtent2D get_render_extent()
{
	int scale = (drs_effective_scale != 0) ? drs_effective_scale : scr_viewsize->integer;

	VkExtent2D result;
	result.width = (uint32_t)(qvk.extent_unscaled.width * (float)scale / 100.f);
	result.height = (uint32_t)(qvk.extent_unscaled.height * (float)scale / 100.f);

	result.width = (result.width + 1) & ~1;

	return result;
}

static VkExtent2D get_screen_image_extent()
{
	VkExtent2D result;
	if (cvar_drs_enable->integer)
	{
		int drs_maxscale = max(cvar_drs_minscale->integer, cvar_drs_maxscale->integer);
		result.width = (uint32_t)(qvk.extent_unscaled.width * (float)drs_maxscale / 100.f);
		result.height = (uint32_t)(qvk.extent_unscaled.height * (float)drs_maxscale / 100.f);
	}
	else
	{
		result.width = max(qvk.extent_render.width, qvk.extent_unscaled.width);
		result.height = max(qvk.extent_render.height, qvk.extent_unscaled.height);
	}

	result.width = (result.width + 1) & ~1;

	return result;
}

void vkpt_reset_accumulation()
{
	num_accumulated_frames = 0;
}

VkResult
vkpt_initialize_all(VkptInitFlags_t init_flags)
{
	vkDeviceWaitIdle(qvk.device);

	qvk.extent_render = get_render_extent();
	qvk.extent_screen_images = get_screen_image_extent();

	qvk.extent_taa_images.width = max(qvk.extent_screen_images.width, qvk.extent_unscaled.width);
	qvk.extent_taa_images.height = max(qvk.extent_screen_images.height, qvk.extent_unscaled.height);

	qvk.gpu_slice_width = (qvk.extent_render.width + qvk.device_count - 1) / qvk.device_count;

	for(int i = 0; i < LENGTH(vkpt_initialization); i++) {
		VkptInit_t *init = vkpt_initialization + i;
		if((init->flags & init_flags) != init_flags)
			continue;
		
		// some entries will respond to multiple events --- do not initialize twice
		if (init->is_initialized)
			continue;

		init->is_initialized = init->initialize
			? (init->initialize() == VK_SUCCESS)
			: 1;
		assert(init->is_initialized);

		if (!init->is_initialized)
		  Com_Error(ERR_FATAL, "Couldn't initialize %s.\n", init->name);
	}

	if ((VKPT_INIT_DEFAULT & init_flags) == init_flags)
	{
		if (!initialize_transparency())
			return VK_RESULT_MAX_ENUM;
	}

	vkpt_textures_prefetch();

	water_normal_texture = IMG_Find("textures/water_n.tga", IT_SKIN, IF_PERMANENT);

	return VK_SUCCESS;
}

VkResult
vkpt_destroy_all(VkptInitFlags_t destroy_flags)
{
	vkDeviceWaitIdle(qvk.device);

	for(int i = LENGTH(vkpt_initialization) - 1; i >= 0; i--) {
		VkptInit_t *init = vkpt_initialization + i;
		if((init->flags & destroy_flags) != destroy_flags)
			continue;
		
		// some entries will respond to multiple events --- do not destroy twice
		if (!init->is_initialized)
			continue;

		init->is_initialized = init->destroy
			? !(init->destroy() == VK_SUCCESS)
			: 0;
		assert(!init->is_initialized);
	}

	if ((VKPT_INIT_DEFAULT & destroy_flags) == destroy_flags)
	{
		destroy_transparency();
		vkpt_light_stats_destroy();
	}

	return VK_SUCCESS;
}

void
vkpt_reload_shader()
{
	char buf[1024];
#ifdef _WIN32
	FILE *f = _popen("compile_shaders.bat", "r");
#else
	FILE *f = popen("make -j compile_shaders", "r");
#endif
	if(f) {
		while(fgets(buf, sizeof buf, f)) {
			Com_Printf("%s", buf);
		}
#ifdef _WIN32
		_pclose(f);
#else
		pclose(f);
#endif
	}

	vkpt_destroy_shader_modules();
	vkpt_load_shader_modules();

	vkpt_destroy_all(VKPT_INIT_RELOAD_SHADER);
	vkpt_initialize_all(VKPT_INIT_RELOAD_SHADER);
}

static void vkpt_reload_textures()
{
	IMG_ReloadAll();
}

//
//
//

vkpt_refdef_t vkpt_refdef = {
	.z_near = 1.0f,
	.z_far  = 4096.0f,
};

QVK_t qvk = {
	.win_width          = 1920,
	.win_height         = 1080,
	.frame_counter      = 0,
};

#define VK_EXTENSION_DO(a) PFN_##a q##a = 0;
LIST_EXTENSIONS_ACCEL_STRUCT
LIST_EXTENSIONS_RAY_PIPELINE
LIST_EXTENSIONS_DEBUG
LIST_EXTENSIONS_INSTANCE
#undef VK_EXTENSION_DO

const char *vk_validation_layers[] = {
	"VK_LAYER_KHRONOS_validation"
};

const char *vk_requested_instance_extensions[] = {
	VK_EXT_DEBUG_UTILS_EXTENSION_NAME,
	VK_EXT_DEBUG_REPORT_EXTENSION_NAME,
#ifdef VKPT_DEVICE_GROUPS
	VK_KHR_DEVICE_GROUP_CREATION_EXTENSION_NAME,
#endif
};

const char *vk_requested_device_extensions_common[] = {
	VK_KHR_SWAPCHAIN_EXTENSION_NAME,
	VK_EXT_DESCRIPTOR_INDEXING_EXTENSION_NAME,
	VK_EXT_SAMPLER_FILTER_MINMAX_EXTENSION_NAME,
#ifdef VKPT_DEVICE_GROUPS
	VK_KHR_DEVICE_GROUP_EXTENSION_NAME,
	VK_KHR_BIND_MEMORY_2_EXTENSION_NAME,
#endif
};

const char *vk_requested_device_extensions_ray_pipeline[] = {
	VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME,
	VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME,
	VK_KHR_PIPELINE_LIBRARY_EXTENSION_NAME,
	VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME
};

const char* vk_requested_device_extensions_ray_query[] = {
	VK_KHR_RAY_QUERY_EXTENSION_NAME,
	VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME,
	VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME
};

const char *vk_requested_device_extensions_debug[] = {
	VK_EXT_DEBUG_MARKER_EXTENSION_NAME,
};

static const VkApplicationInfo vk_app_info = {
	.sType              = VK_STRUCTURE_TYPE_APPLICATION_INFO,
	.pApplicationName   = "quake 2 pathtracing",
	.applicationVersion = VK_MAKE_VERSION(1, 0, 0),
	.pEngineName        = "vkpt",
	.engineVersion      = VK_MAKE_VERSION(1, 0, 0),
	.apiVersion         = VK_API_VERSION_1_2,
};

/* use this to override file names */
static const char *shader_module_file_names[NUM_QVK_SHADER_MODULES];

void
get_vk_extension_list(
		const char *layer,
		uint32_t *num_extensions,
		VkExtensionProperties **ext)
{
	_VK(vkEnumerateInstanceExtensionProperties(layer, num_extensions, NULL));
	*ext = malloc(sizeof(**ext) * *num_extensions);
	_VK(vkEnumerateInstanceExtensionProperties(layer, num_extensions, *ext));
}

void
get_vk_layer_list(
		uint32_t *num_layers,
		VkLayerProperties **ext)
{
	_VK(vkEnumerateInstanceLayerProperties(num_layers, NULL));
	*ext = malloc(sizeof(**ext) * *num_layers);
	_VK(vkEnumerateInstanceLayerProperties(num_layers, *ext));
}

static VKAPI_ATTR VkBool32 VKAPI_CALL
vk_debug_callback(
		VkDebugUtilsMessageSeverityFlagBitsEXT severity,
		VkDebugUtilsMessageTypeFlagsEXT type,
		const VkDebugUtilsMessengerCallbackDataEXT* callback_data,
		void *user_data)
{
	Com_EPrintf("validation layer %i %i: %s\n", (int32_t)type, (int32_t)severity,  callback_data->pMessage);
	debug_output("Vulkan error: %s\n", callback_data->pMessage);

	if (callback_data->cmdBufLabelCount)
	{
		Com_EPrintf("~~~ ");
		for (uint32_t i = 0; i < callback_data->cmdBufLabelCount; ++i)
		{
			const VkDebugUtilsLabelEXT* label = &callback_data->pCmdBufLabels[i];
			Com_EPrintf("%s ~ ", label->pLabelName);
		}
		Com_EPrintf("\n");
	}

	if (callback_data->objectCount)
	{
		for (uint32_t i = 0; i < callback_data->objectCount; ++i)
		{
			const VkDebugUtilsObjectNameInfoEXT* obj = &callback_data->pObjects[i];
			Com_EPrintf("--- %s %i\n", obj->pObjectName, (int32_t)obj->objectType);
		}
	}

	Com_EPrintf("\n");
	return VK_FALSE;
}

VkResult
qvkCreateDebugUtilsMessengerEXT(
		VkInstance instance,
		const VkDebugUtilsMessengerCreateInfoEXT* pCreateInfo,
		const VkAllocationCallbacks* pAllocator,
		VkDebugUtilsMessengerEXT* pCallback)
{
	PFN_vkCreateDebugUtilsMessengerEXT func = (PFN_vkCreateDebugUtilsMessengerEXT)
		vkGetInstanceProcAddr(instance, "vkCreateDebugUtilsMessengerEXT");
	if(func)
		return func(instance, pCreateInfo, pAllocator, pCallback);
	return VK_ERROR_EXTENSION_NOT_PRESENT;
}

VkResult
qvkDestroyDebugUtilsMessengerEXT(
		VkInstance instance,
		VkDebugUtilsMessengerEXT callback,
		const VkAllocationCallbacks* pAllocator)
{
	PFN_vkDestroyDebugUtilsMessengerEXT func = (PFN_vkDestroyDebugUtilsMessengerEXT)
		vkGetInstanceProcAddr(instance, "vkDestroyDebugUtilsMessengerEXT");
	if(func) {
		func(instance, callback, pAllocator);
		return VK_SUCCESS;
	}
	return VK_ERROR_EXTENSION_NOT_PRESENT;
}

VkResult
create_swapchain()
{
    num_accumulated_frames = 0;

	/* create swapchain (query details and ignore them afterwards :-) )*/
	VkSurfaceCapabilitiesKHR surf_capabilities;
	vkGetPhysicalDeviceSurfaceCapabilitiesKHR(qvk.physical_device, qvk.surface, &surf_capabilities);

	if (surf_capabilities.currentExtent.width == 0 || surf_capabilities.currentExtent.height == 0)
		return VK_SUCCESS;

	uint32_t num_formats = 0;
	vkGetPhysicalDeviceSurfaceFormatsKHR(qvk.physical_device, qvk.surface, &num_formats, NULL);
	VkSurfaceFormatKHR *avail_surface_formats = alloca(sizeof(VkSurfaceFormatKHR) * num_formats);
	vkGetPhysicalDeviceSurfaceFormatsKHR(qvk.physical_device, qvk.surface, &num_formats, avail_surface_formats);
	/* Com_Printf("num surface formats: %d\n", num_formats);

	Com_Printf("available surface formats:\n");
	for(int i = 0; i < num_formats; i++)
		Com_Printf("  %s\n", vk_format_to_string(avail_surface_formats[i].format)); */ 


	VkFormat acceptable_formats[] = {
		VK_FORMAT_R8G8B8A8_SRGB, VK_FORMAT_B8G8R8A8_SRGB,
	};

	//qvk.surf_format.format     = VK_FORMAT_R8G8B8A8_SRGB;
	//qvk.surf_format.format     = VK_FORMAT_B8G8R8A8_SRGB;
	for(int i = 0; i < LENGTH(acceptable_formats); i++) {
		for(int j = 0; j < num_formats; j++)
			if(acceptable_formats[i] == avail_surface_formats[j].format) {
				qvk.surf_format = avail_surface_formats[j];
				goto out;
			}
	}
out:;

	uint32_t num_present_modes = 0;
	vkGetPhysicalDeviceSurfacePresentModesKHR(qvk.physical_device, qvk.surface, &num_present_modes, NULL);
	VkPresentModeKHR *avail_present_modes = alloca(sizeof(VkPresentModeKHR) * num_present_modes);
	vkGetPhysicalDeviceSurfacePresentModesKHR(qvk.physical_device, qvk.surface, &num_present_modes, avail_present_modes);
	qboolean immediate_mode_available = qfalse;

	for (int i = 0; i < num_present_modes; i++) {
		if (avail_present_modes[i] == VK_PRESENT_MODE_IMMEDIATE_KHR) {
			immediate_mode_available = qtrue;
			break;
		}
	}

	if (cvar_vsync->integer) {
		qvk.present_mode = VK_PRESENT_MODE_FIFO_KHR;
	} else if (immediate_mode_available) {
		qvk.present_mode = VK_PRESENT_MODE_IMMEDIATE_KHR;
	} else {
		qvk.present_mode = VK_PRESENT_MODE_MAILBOX_KHR;
	}

	if(surf_capabilities.currentExtent.width != ~0u) {
		qvk.extent_unscaled = surf_capabilities.currentExtent;
	}
	else {
		qvk.extent_unscaled.width = MIN(surf_capabilities.maxImageExtent.width, qvk.win_width);
		qvk.extent_unscaled.height = MIN(surf_capabilities.maxImageExtent.height, qvk.win_height);

		qvk.extent_unscaled.width = MAX(surf_capabilities.minImageExtent.width, qvk.extent_unscaled.width);
		qvk.extent_unscaled.height = MAX(surf_capabilities.minImageExtent.height, qvk.extent_unscaled.height);
	}

	uint32_t num_images = 2;
	//uint32_t num_images = surf_capabilities.minImageCount + 1;
	if(surf_capabilities.maxImageCount > 0)
		num_images = MIN(num_images, surf_capabilities.maxImageCount);

	VkSwapchainCreateInfoKHR swpch_create_info = {
		.sType                 = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
		.surface               = qvk.surface,
		.minImageCount         = num_images,
		.imageFormat           = qvk.surf_format.format,
		.imageColorSpace       = qvk.surf_format.colorSpace,
		.imageExtent           = qvk.extent_unscaled,
		.imageArrayLayers      = 1, /* only needs to be changed for stereoscopic rendering */ 
		.imageUsage            = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT
							   | VK_IMAGE_USAGE_TRANSFER_SRC_BIT
							   | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
		.imageSharingMode      = VK_SHARING_MODE_EXCLUSIVE, /* VK_SHARING_MODE_CONCURRENT if not using same queue */
		.queueFamilyIndexCount = 0,
		.pQueueFamilyIndices   = NULL,
		.preTransform          = surf_capabilities.currentTransform,
		.compositeAlpha        = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR, /* no alpha for window transparency */
		.presentMode           = qvk.present_mode,
		.clipped               = VK_FALSE, /* do not render pixels that are occluded by other windows */
		//.clipped               = VK_TRUE, /* do not render pixels that are occluded by other windows */
		.oldSwapchain          = VK_NULL_HANDLE, /* need to provide previous swapchain in case of window resize */
	};

	if(vkCreateSwapchainKHR(qvk.device, &swpch_create_info, NULL, &qvk.swap_chain) != VK_SUCCESS) {
		Com_EPrintf("error creating swapchain\n");
		return 1;
	}

	vkGetSwapchainImagesKHR(qvk.device, qvk.swap_chain, &qvk.num_swap_chain_images, NULL);
	assert(qvk.num_swap_chain_images);
	qvk.swap_chain_images = malloc(qvk.num_swap_chain_images * sizeof(*qvk.swap_chain_images));
	vkGetSwapchainImagesKHR(qvk.device, qvk.swap_chain, &qvk.num_swap_chain_images, qvk.swap_chain_images);

	qvk.swap_chain_image_views = malloc(qvk.num_swap_chain_images * sizeof(*qvk.swap_chain_image_views));
	for(int i = 0; i < qvk.num_swap_chain_images; i++) {
		VkImageViewCreateInfo img_create_info = {
			.sType      = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
			.image      = qvk.swap_chain_images[i],
			.viewType   = VK_IMAGE_VIEW_TYPE_2D,
			.format     = qvk.surf_format.format,
#if 1
			.components = {
				VK_COMPONENT_SWIZZLE_R,
				VK_COMPONENT_SWIZZLE_G,
				VK_COMPONENT_SWIZZLE_B,
				VK_COMPONENT_SWIZZLE_A
			},
#endif
			.subresourceRange = {
				.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
				.baseMipLevel   = 0,
				.levelCount     = 1,
				.baseArrayLayer = 0,
				.layerCount     = 1
			}
		};

		if(vkCreateImageView(qvk.device, &img_create_info, NULL, qvk.swap_chain_image_views + i) != VK_SUCCESS) {
			Com_EPrintf("error creating image view!");

			free(qvk.swap_chain_image_views);
			qvk.swap_chain_image_views = NULL;

			free(qvk.swap_chain_images);
			qvk.swap_chain_images = NULL;

			qvk.num_swap_chain_images = 0;
			return 1;
		}
	}

	VkCommandBuffer cmd_buf = vkpt_begin_command_buffer(&qvk.cmd_buffers_graphics);

	for (int image_index = 0; image_index < qvk.num_swap_chain_images; image_index++)
	{
		IMAGE_BARRIER(cmd_buf,
			.image = qvk.swap_chain_images[image_index],
			.subresourceRange = {
				.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
				.baseMipLevel = 0,
				.levelCount = 1,
				.baseArrayLayer = 0,
				.layerCount = 1
			},
			.srcAccessMask = 0,
			.dstAccessMask = 0,
			.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
			.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
		);
	}

	vkpt_submit_command_buffer_simple(cmd_buf, qvk.queue_graphics, qtrue);
	vkpt_wait_idle(qvk.queue_graphics, &qvk.cmd_buffers_graphics);

	return VK_SUCCESS;
}

VkResult
create_command_pool_and_fences()
{
	VkCommandPoolCreateInfo cmd_pool_create_info = {
		.sType            = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
		.queueFamilyIndex = qvk.queue_idx_graphics,
		.flags            = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
	};

	/* command pool and buffers */
	_VK(vkCreateCommandPool(qvk.device, &cmd_pool_create_info, NULL, &qvk.cmd_buffers_graphics.command_pool));
	
	cmd_pool_create_info.queueFamilyIndex = qvk.queue_idx_transfer;
	_VK(vkCreateCommandPool(qvk.device, &cmd_pool_create_info, NULL, &qvk.cmd_buffers_transfer.command_pool));

	/* fences and semaphores */
	for (int frame = 0; frame < MAX_FRAMES_IN_FLIGHT; frame++)
	{
		for (int gpu = 0; gpu < qvk.device_count; gpu++)
		{
			semaphore_group_t* group = &qvk.semaphores[frame][gpu];

			VkSemaphoreCreateInfo semaphore_info = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
		
			_VK(vkCreateSemaphore(qvk.device, &semaphore_info, NULL, &group->image_available));
			_VK(vkCreateSemaphore(qvk.device, &semaphore_info, NULL, &group->render_finished));
			_VK(vkCreateSemaphore(qvk.device, &semaphore_info, NULL, &group->transfer_finished));
			_VK(vkCreateSemaphore(qvk.device, &semaphore_info, NULL, &group->trace_finished));

			ATTACH_LABEL_VARIABLE(group->image_available, SEMAPHORE);
			ATTACH_LABEL_VARIABLE(group->render_finished, SEMAPHORE);
			ATTACH_LABEL_VARIABLE(group->transfer_finished, SEMAPHORE);
			ATTACH_LABEL_VARIABLE(group->trace_finished, SEMAPHORE);

			group->trace_signaled = qfalse;
		}
	}

	VkFenceCreateInfo fence_info = {
		.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
		.flags = VK_FENCE_CREATE_SIGNALED_BIT, /* fence's initial state set to be signaled
												  to make program not hang */
	};
	for(int i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
		_VK(vkCreateFence(qvk.device, &fence_info, NULL, qvk.fences_frame_sync + i));
		ATTACH_LABEL_VARIABLE(qvk.fences_frame_sync[i], FENCE);
	}
	_VK(vkCreateFence(qvk.device, &fence_info, NULL, &qvk.fence_vertex_sync));
	ATTACH_LABEL_VARIABLE(qvk.fence_vertex_sync, FENCE);

	return VK_SUCCESS;
}

static void
append_string_list(const char** dst, uint32_t* dst_count, uint32_t dst_capacity, const char** src, uint32_t src_count)
{
	assert(*dst_count + src_count <= dst_capacity);
	dst += *dst_count;
	memcpy((void*)dst, src, sizeof(char*) * src_count);
	*dst_count += src_count;
}

qboolean
init_vulkan()
{
	Com_Printf("----- init_vulkan -----\n");

	/* layers */
	get_vk_layer_list(&qvk.num_layers, &qvk.layers);
	Com_Printf("Available Vulkan layers: \n");
	for(int i = 0; i < qvk.num_layers; i++) {
		Com_Printf("  %s\n", qvk.layers[i].layerName);
	}
	
	/* instance extensions */

	if (!SDL_Vulkan_GetInstanceExtensions(qvk.window, &qvk.num_sdl2_extensions, NULL)) {
		Com_EPrintf("Couldn't get SDL2 Vulkan extension count\n");
		return qfalse;
	}

	qvk.sdl2_extensions = malloc(sizeof(char*) * qvk.num_sdl2_extensions);
	if (!SDL_Vulkan_GetInstanceExtensions(qvk.window, &qvk.num_sdl2_extensions, qvk.sdl2_extensions)) {
		Com_EPrintf("Couldn't get SDL2 Vulkan extensions\n");
		return qfalse;
	}

	Com_Printf("Vulkan instance extensions required by SDL2: \n");
	for (int i = 0; i < qvk.num_sdl2_extensions; i++) {
		Com_Printf("  %s\n", qvk.sdl2_extensions[i]);
	}

	int num_inst_ext_combined = qvk.num_sdl2_extensions + LENGTH(vk_requested_instance_extensions);
	char **ext = alloca(sizeof(char *) * num_inst_ext_combined);
	memcpy(ext, qvk.sdl2_extensions, qvk.num_sdl2_extensions * sizeof(*qvk.sdl2_extensions));
	memcpy(ext + qvk.num_sdl2_extensions, vk_requested_instance_extensions, sizeof(vk_requested_instance_extensions));

	get_vk_extension_list(NULL, &qvk.num_extensions, &qvk.extensions); /* valid here? */
	Com_Printf("Supported Vulkan instance extensions: \n");
	for(int i = 0; i < qvk.num_extensions; i++) {
		int requested = 0;
		for(int j = 0; j < num_inst_ext_combined; j++) {
			if(!strcmp(qvk.extensions[i].extensionName, ext[j])) {
				requested = 1;
				break;
			}
		}
		Com_Printf("  %s%s\n", qvk.extensions[i].extensionName, requested ? " (requested)" : "");
	}

	/* create instance */
	VkInstanceCreateInfo inst_create_info = {
		.sType                   = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
		.pApplicationInfo        = &vk_app_info,
		.enabledExtensionCount   = num_inst_ext_combined,
		.ppEnabledExtensionNames = (const char * const*)ext,
	};

	qvk.enable_validation = qfalse;

	if (cvar_vk_validation->integer)
	{
		inst_create_info.ppEnabledLayerNames = vk_validation_layers;
		inst_create_info.enabledLayerCount = LENGTH(vk_validation_layers);
		qvk.enable_validation = qtrue;
	}

	VkResult result = vkCreateInstance(&inst_create_info, NULL, &qvk.instance);

	if (result == VK_ERROR_LAYER_NOT_PRESENT)
	{
		Com_WPrintf("Vulkan validation layer is requested through cvar %s but is not available.\n", cvar_vk_validation->name);

		// Try again, this time without the validation layer

		inst_create_info.enabledLayerCount = 0;
		result = vkCreateInstance(&inst_create_info, NULL, &qvk.instance);
		qvk.enable_validation = qfalse;
	}
	else if (cvar_vk_validation->integer)
	{
		Com_WPrintf("Vulkan validation layer is enabled, expect degraded game performance.\n");
	}

	if (result != VK_SUCCESS)
	{
		Com_Error(ERR_FATAL, "Failed to initialize a Vulkan instance.\nError code: %s", qvk_result_to_string(result));
		return qfalse;
	}

#define VK_EXTENSION_DO(a) \
		q##a = (PFN_##a) vkGetInstanceProcAddr(qvk.instance, #a); \
		if (!q##a) { Com_EPrintf("warning: could not load instance function %s\n", #a); }
	LIST_EXTENSIONS_INSTANCE
#undef VK_EXTENSION_DO

	/* setup debug callback */
	VkDebugUtilsMessengerCreateInfoEXT dbg_create_info = {
		.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT,
		.messageSeverity =
			VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT
			| VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT,
		.messageType =
			  VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT
			| VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT,
		.pfnUserCallback = vk_debug_callback,
		.pUserData = NULL
	};

	_VK(qvkCreateDebugUtilsMessengerEXT(qvk.instance, &dbg_create_info, NULL, &qvk.dbg_messenger));

	/* create surface */
	if(!SDL_Vulkan_CreateSurface(qvk.window, qvk.instance, &qvk.surface)) {
		Com_EPrintf("SDL2 could not create a surface!\n");
		return qfalse;
	}

	/* pick physical device (iterate over all but pick device 0 anyways) */
	uint32_t num_devices = 0;
	_VK(vkEnumeratePhysicalDevices(qvk.instance, &num_devices, NULL));
	if(num_devices == 0)
		return qfalse;
	VkPhysicalDevice *devices = alloca(sizeof(VkPhysicalDevice) *num_devices);
	_VK(vkEnumeratePhysicalDevices(qvk.instance, &num_devices, devices));

#ifdef VKPT_DEVICE_GROUPS
	uint32_t num_device_groups = 0;

	if (cvar_sli->integer)
		_VK(vkEnumeratePhysicalDeviceGroups(qvk.instance, &num_device_groups, NULL));

	VkDeviceGroupDeviceCreateInfo device_group_create_info;
	VkPhysicalDeviceGroupProperties device_group_info;

	if(num_device_groups > 0) {
		// we always use the first group
		num_device_groups = 1;
		_VK(vkEnumeratePhysicalDeviceGroups(qvk.instance, &num_device_groups, &device_group_info));

		if (device_group_info.physicalDeviceCount > VKPT_MAX_GPUS)
		{
			Com_EPrintf("SLI: device group 0 has %d devices, which is more than maximum supported count (%d).\n",
				device_group_info.physicalDeviceCount, VKPT_MAX_GPUS);
			return qfalse;
		}

		device_group_create_info.sType = VK_STRUCTURE_TYPE_DEVICE_GROUP_DEVICE_CREATE_INFO;
		device_group_create_info.pNext = NULL;
		device_group_create_info.physicalDeviceCount = device_group_info.physicalDeviceCount;
		device_group_create_info.pPhysicalDevices = device_group_info.physicalDevices;

		qvk.device_count = device_group_create_info.physicalDeviceCount;
		for(int i = 0; i < qvk.device_count; i++) {
			qvk.device_group_physical_devices[i] = device_group_create_info.pPhysicalDevices[i];
		}
		Com_Printf("SLI: using device group 0 with %d device(s).\n", qvk.device_count);
	}
	else
	{
		qvk.device_count = 1;
		if (!cvar_sli->integer)
			Com_Printf("SLI: multi-GPU support disabled through the 'sli' console variable.\n");
		else
			Com_Printf("SLI: no device groups found, using a single device.\n");
	}
#else
	qvk.device_count = 1;
#endif

	int picked_device_with_ray_pipeline = -1;
	int picked_device_with_ray_query = -1;
	VkDriverId picked_driver_ray_query = VK_DRIVER_ID_MAX_ENUM;
	qvk.use_ray_query = qfalse;

	for(int i = 0; i < num_devices; i++) 
	{
		VkPhysicalDeviceDriverProperties driver_properties = {
			.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES,
			.pNext = NULL
		};

		VkPhysicalDeviceProperties2 dev_properties2 = {
			.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
			.pNext = &driver_properties
		};
		vkGetPhysicalDeviceProperties2(devices[i], &dev_properties2);

		VkPhysicalDeviceFeatures dev_features;
		vkGetPhysicalDeviceFeatures(devices[i], &dev_features);

		Com_Printf("Physical device %d: %s\n", i, dev_properties2.properties.deviceName);

		uint32_t num_ext;
		vkEnumerateDeviceExtensionProperties(devices[i], NULL, &num_ext, NULL);

		VkExtensionProperties *ext_properties = alloca(sizeof(VkExtensionProperties) * num_ext);
		vkEnumerateDeviceExtensionProperties(devices[i], NULL, &num_ext, ext_properties);

		Com_Printf("Supported Vulkan device extensions:\n");
		for(int j = 0; j < num_ext; j++) 
		{
			Com_Printf("  %s\n", ext_properties[j].extensionName);

			if(!strcmp(ext_properties[j].extensionName, VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME)) 
			{
				if (picked_device_with_ray_pipeline < 0)
				{
					picked_device_with_ray_pipeline = i;
				}
			}

			if (!strcmp(ext_properties[j].extensionName, VK_KHR_RAY_QUERY_EXTENSION_NAME))
			{
				if (picked_device_with_ray_query < 0)
				{
					picked_device_with_ray_query = i;
					picked_driver_ray_query = driver_properties.driverID;
				}
			}
		}
	}

	int picked_device = -1;

	if (!Q_strcasecmp(cvar_ray_tracing_api->string, "query") && picked_device_with_ray_query >= 0)
	{
		qvk.use_ray_query = qtrue;
		picked_device = picked_device_with_ray_query;
	}
	else if (!Q_strcasecmp(cvar_ray_tracing_api->string, "pipeline") && picked_device_with_ray_pipeline >= 0)
	{
		qvk.use_ray_query = qfalse;
		picked_device = picked_device_with_ray_pipeline;
	}
	
	if (picked_device < 0)
	{
		if (Q_strcasecmp(cvar_ray_tracing_api->string, "auto"))
		{
			Com_WPrintf("Requested Ray Tracing API (%s) is not available, switching to automatic selection.\n", cvar_ray_tracing_api->string);
		}

		if (picked_driver_ray_query == VK_DRIVER_ID_NVIDIA_PROPRIETARY)
		{
			// Pick KHR_ray_query on NVIDIA drivers, if available.
			qvk.use_ray_query = qtrue;
			picked_device = picked_device_with_ray_query;
		}
		else if (picked_device_with_ray_pipeline >= 0)
		{
			// Pick KHR_ray_tracing_pipeline otherwise
			qvk.use_ray_query = qfalse;
			picked_device = picked_device_with_ray_pipeline;
		}
	}

	if (picked_device < 0)
	{
		Com_Error(ERR_FATAL, "No ray tracing capable GPU found.");
	}

	qvk.physical_device = devices[picked_device];

	{
		VkPhysicalDeviceDriverProperties driver_properties = {
			.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES,
			.pNext = NULL
		};

		VkPhysicalDeviceProperties2 dev_properties2 = {
			.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
			.pNext = &driver_properties
		};

		vkGetPhysicalDeviceProperties2(devices[picked_device], &dev_properties2);

		// Store the timestamp period to get correct profiler results
		qvk.timestampPeriod = dev_properties2.properties.limits.timestampPeriod;

		Com_Printf("Picked physical device %d: %s\n", picked_device, dev_properties2.properties.deviceName);
		Com_Printf("Using %s\n", (qvk.use_ray_query ? VK_KHR_RAY_QUERY_EXTENSION_NAME : VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME));

#ifdef _WIN32
		if (dev_properties2.properties.vendorID == 0x10de) // NVIDIA vendor ID
		{
			uint32_t driver_major = (dev_properties2.properties.driverVersion >> 22) & 0x3ff;
			uint32_t driver_minor = (dev_properties2.properties.driverVersion >> 14) & 0xff;

			Com_Printf("NVIDIA GPU detected. Driver version: %u.%02u\n", driver_major, driver_minor);
			
			uint32_t required_major = 0;
			uint32_t required_minor = 0;
			int nfields = sscanf(cvar_min_driver_version_nvidia->string, "%u.%u", &required_major, &required_minor);
			if (nfields == 2)
			{
				if (driver_major < required_major || driver_major == required_major && driver_minor < required_minor)
				{
					Com_Error(ERR_FATAL, "This game requires NVIDIA Graphics Driver version to be at least %u.%02u, "
						"while the installed version is %u.%02u.\nPlease update the NVIDIA Graphics Driver.",
						required_major, required_minor, driver_major, driver_minor);
				}
			}
		}
		else if (driver_properties.driverID == VK_DRIVER_ID_AMD_PROPRIETARY)
		{
			Com_Printf("AMD GPU detected. Driver version: %s\n", driver_properties.driverInfo);

			uint32_t present_major = 0;
			uint32_t present_minor = 0;
			uint32_t present_patch = 0;
			int nfields_present = sscanf(driver_properties.driverInfo, "%u.%u.%u", &present_major, &present_minor, &present_patch);

			uint32_t required_major = 0;
			uint32_t required_minor = 0;
			uint32_t required_patch = 0;
			int nfields_required = sscanf(cvar_min_driver_version_amd->string, "%u.%u.%u", &required_major, &required_minor, &required_patch);

			if (nfields_present == 3 && nfields_required == 3)
			{
				if (present_major < required_major || present_major == required_major && present_minor < required_minor || present_major == required_major && present_minor == required_minor && present_patch < required_patch)
				{
					Com_Error(ERR_FATAL, "This game requires AMD Radeon Software version to be at least %s, while the installed version is %s.\nPlease update the AMD Radeon Software.",
						cvar_min_driver_version_amd->string, driver_properties.driverInfo);
				}
			}
		}
#endif
	}

	vkGetPhysicalDeviceMemoryProperties(qvk.physical_device, &qvk.mem_properties);

	/* queue family and create physical device */
	uint32_t num_queue_families = 0;
	vkGetPhysicalDeviceQueueFamilyProperties(qvk.physical_device, &num_queue_families, NULL);
	VkQueueFamilyProperties *queue_families = alloca(sizeof(VkQueueFamilyProperties) * num_queue_families);
	vkGetPhysicalDeviceQueueFamilyProperties(qvk.physical_device, &num_queue_families, queue_families);

	// Com_Printf("num queue families: %d\n", num_queue_families);

	qvk.queue_idx_graphics = -1;
	qvk.queue_idx_transfer = -1;

	for(int i = 0; i < num_queue_families; i++) {
		if(!queue_families[i].queueCount)
			continue;
		VkBool32 present_support = 0;
		vkGetPhysicalDeviceSurfaceSupportKHR(qvk.physical_device, i, qvk.surface, &present_support);

		const int supports_graphics = queue_families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT;
		const int supports_compute = queue_families[i].queueFlags & VK_QUEUE_COMPUTE_BIT;
		const int supports_transfer = queue_families[i].queueFlags & VK_QUEUE_TRANSFER_BIT;

		if(supports_graphics && supports_compute && supports_transfer && qvk.queue_idx_graphics < 0) {
			if(!present_support)
				continue;
			qvk.queue_idx_graphics = i;
		}
		else if(supports_transfer && qvk.queue_idx_transfer < 0) {
			qvk.queue_idx_transfer = i;
		}
	}

	if(qvk.queue_idx_graphics < 0 || qvk.queue_idx_transfer < 0) {
		Com_Error(ERR_FATAL, "Could not find a suitable Vulkan queue family!\n");
		return qfalse;
	}
	
	float queue_priorities = 1.0f;
	int num_create_queues = 0;
	VkDeviceQueueCreateInfo queue_create_info[3];

	{
		VkDeviceQueueCreateInfo q = {
			.sType            = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
			.queueCount       = 1,
			.pQueuePriorities = &queue_priorities,
			.queueFamilyIndex = qvk.queue_idx_graphics,
		};

		queue_create_info[num_create_queues++] = q;
	};
	if(qvk.queue_idx_transfer != qvk.queue_idx_graphics) {
		VkDeviceQueueCreateInfo q = {
			.sType            = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
			.queueCount       = 1,
			.pQueuePriorities = &queue_priorities,
			.queueFamilyIndex = qvk.queue_idx_transfer,
		};
		queue_create_info[num_create_queues++] = q;
	};

	VkPhysicalDeviceDescriptorIndexingFeatures idx_features = {
		.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES,
		.runtimeDescriptorArray = 1,
		.shaderSampledImageArrayNonUniformIndexing = 1,
		.shaderStorageBufferArrayNonUniformIndexing = 1
	};

#ifdef VKPT_DEVICE_GROUPS
	if (qvk.device_count > 1) {
		idx_features.pNext = &device_group_create_info;
	}
#endif
	VkPhysicalDeviceAccelerationStructureFeaturesKHR physical_device_as_features = {
		.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR,
		.pNext = &idx_features,
		.accelerationStructure = VK_TRUE,
	};

	VkPhysicalDeviceBufferDeviceAddressFeatures physical_device_address_features = {
		.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES,
		.pNext = &physical_device_as_features,
		.bufferDeviceAddress = VK_TRUE
	};

#ifdef VKPT_DEVICE_GROUPS
	if (qvk.device_count > 1) {
		physical_device_address_features.bufferDeviceAddressMultiDevice = VK_TRUE;
	}
#endif

	VkPhysicalDeviceRayTracingPipelineFeaturesKHR physical_device_rt_pipeline_features = {
		.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_FEATURES_KHR,
		.pNext = &physical_device_address_features,
		.rayTracingPipeline = VK_TRUE
	};

	VkPhysicalDeviceRayQueryFeaturesKHR physical_device_ray_query_features = {
		.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR,
		.pNext = &physical_device_address_features,
		.rayQuery = VK_TRUE
	};

	VkPhysicalDeviceFeatures2 device_features = {
		.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2_KHR,
		.features = {
			.robustBufferAccess = 1,
			.fullDrawIndexUint32 = 1,
			.imageCubeArray = 1,
			.independentBlend = 1,
			.geometryShader = 0,
			.tessellationShader = 0,
			.sampleRateShading = 0,
			.dualSrcBlend = 0,
			.logicOp = 0,
			.multiDrawIndirect = 0,
			.drawIndirectFirstInstance = 0,
			.depthClamp = 0,
			.depthBiasClamp = 0,
			.fillModeNonSolid = 0,
			.depthBounds = 0,
			.wideLines = 0,
			.largePoints = 0,
			.alphaToOne = 0,
			.multiViewport = 0,
			.samplerAnisotropy = 1,
			.textureCompressionETC2 = 0,
			.textureCompressionASTC_LDR = 0,
			.textureCompressionBC = 0,
			.occlusionQueryPrecise = 0,
			.pipelineStatisticsQuery = 1,
			.vertexPipelineStoresAndAtomics = 0,
			.fragmentStoresAndAtomics = 0,
			.shaderTessellationAndGeometryPointSize = 0,
			.shaderImageGatherExtended = 0,
			.shaderStorageImageExtendedFormats = 1,
			.shaderStorageImageMultisample = 0,
			.shaderStorageImageReadWithoutFormat = 0,
			.shaderStorageImageWriteWithoutFormat = 0,
			.shaderUniformBufferArrayDynamicIndexing = 1,
			.shaderSampledImageArrayDynamicIndexing = 1,
			.shaderStorageBufferArrayDynamicIndexing = 1,
			.shaderStorageImageArrayDynamicIndexing = 1,
			.shaderClipDistance = 0,
			.shaderCullDistance = 0,
			.shaderFloat64 = 0,
			.shaderInt64 = 0,
			.shaderInt16 = 0,
			.shaderResourceResidency = 0,
			.shaderResourceMinLod = 0,
			.sparseBinding = 1,
			.sparseResidencyBuffer = 0,
			.sparseResidencyImage2D = 0,
			.sparseResidencyImage3D = 0,
			.sparseResidency2Samples = 0,
			.sparseResidency4Samples = 0,
			.sparseResidency8Samples = 0,
			.sparseResidency16Samples = 0,
			.sparseResidencyAliased = 0,
			.variableMultisampleRate = 0,
			.inheritedQueries = 0,
		}
	};
	VkDeviceCreateInfo dev_create_info = {
		.sType                   = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
		.pNext                   = &device_features,
		.pQueueCreateInfos       = queue_create_info,
		.queueCreateInfoCount    = num_create_queues
	};

	uint32_t max_extension_count = LENGTH(vk_requested_device_extensions_common);
	max_extension_count += max(LENGTH(vk_requested_device_extensions_ray_pipeline), LENGTH(vk_requested_device_extensions_ray_query));
	max_extension_count += LENGTH(vk_requested_device_extensions_debug);

	const char** device_extensions = alloca(sizeof(char*) * max_extension_count);
	uint32_t device_extension_count = 0;

	append_string_list(device_extensions, &device_extension_count, max_extension_count, 
		vk_requested_device_extensions_common, LENGTH(vk_requested_device_extensions_common));

	if (qvk.use_ray_query)
	{
		append_string_list(device_extensions, &device_extension_count, max_extension_count,
			vk_requested_device_extensions_ray_query, LENGTH(vk_requested_device_extensions_ray_query));

		device_features.pNext = &physical_device_ray_query_features;
	}
	else
	{
		append_string_list(device_extensions, &device_extension_count, max_extension_count,
			vk_requested_device_extensions_ray_pipeline, LENGTH(vk_requested_device_extensions_ray_pipeline));

		device_features.pNext = &physical_device_rt_pipeline_features;
	}
	
	if (qvk.enable_validation)
	{
		append_string_list(device_extensions, &device_extension_count, max_extension_count,
			vk_requested_device_extensions_debug, LENGTH(vk_requested_device_extensions_debug));
	}

	dev_create_info.enabledExtensionCount = device_extension_count;
	dev_create_info.ppEnabledExtensionNames = device_extensions;

	/* create device and queue */
	result = vkCreateDevice(qvk.physical_device, &dev_create_info, NULL, &qvk.device);
	if (result != VK_SUCCESS)
	{
		Com_Error(ERR_FATAL, "Failed to create a Vulkan device.\nError code: %s", qvk_result_to_string(result));
		return qfalse;
	}

	vkGetDeviceQueue(qvk.device, qvk.queue_idx_graphics, 0, &qvk.queue_graphics);
	vkGetDeviceQueue(qvk.device, qvk.queue_idx_transfer, 0, &qvk.queue_transfer);

#define VK_EXTENSION_DO(a) \
	q##a = (PFN_##a) vkGetDeviceProcAddr(qvk.device, #a); \
	if(!q##a) { Com_EPrintf("warning: could not load function %s\n", #a); }

	LIST_EXTENSIONS_ACCEL_STRUCT

	if (!qvk.use_ray_query)
	{
		LIST_EXTENSIONS_RAY_PIPELINE
	}

	if(qvk.enable_validation)
	{
		LIST_EXTENSIONS_DEBUG
	}

#undef VK_EXTENSION_DO

	Com_Printf("-----------------------\n");

	return qtrue;
}

static VkShaderModule
create_shader_module_from_file(const char *name, const char *enum_name, qboolean is_rt_shader)
{
	const char* suffix = "";
	if (is_rt_shader)
	{
		if (qvk.use_ray_query)
			suffix = ".query";
		else
			suffix = ".pipeline";
	}

	char path[1024];
	snprintf(path, sizeof path, "shader_vkpt/%s%s.spv", name ? name : (enum_name + 8), suffix);
	if(!name) {
		int len = 0;
		for(len = 0; path[len]; len++)
			path[len] = tolower(path[len]);
		while(--len >= 0) {
			if(path[len] == '_') {
				path[len] = '.';
				break;
			}
		}
	}

	char *data;
	size_t size;

	size = FS_LoadFile(path, (void**)&data);
	if(!data) {
		Com_EPrintf("Couldn't find shader module %s!\n", path);
		return VK_NULL_HANDLE;
	}

	VkShaderModule module;

	VkShaderModuleCreateInfo create_info = {
		.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
		.codeSize = size,
		.pCode = (uint32_t *) data,
	};

	_VK(vkCreateShaderModule(qvk.device, &create_info, NULL, &module));

	Z_Free(data);

	return module;
}

VkResult
vkpt_load_shader_modules()
{
	VkResult ret = VK_SUCCESS;
#define SHADER_MODULE_DO(a) do { \
	qvk.shader_modules[a] = create_shader_module_from_file(shader_module_file_names[a], #a, IS_RT_SHADER); \
	ret = (ret == VK_SUCCESS && qvk.shader_modules[a]) ? VK_SUCCESS : VK_ERROR_INITIALIZATION_FAILED; \
	if(qvk.shader_modules[a]) { \
		ATTACH_LABEL_VARIABLE_NAME((uint64_t)qvk.shader_modules[a], SHADER_MODULE, #a); \
	}\
	} while(0);

#define IS_RT_SHADER qfalse
	LIST_SHADER_MODULES;
#undef IS_RT_SHADER
#define IS_RT_SHADER qtrue
	LIST_RT_RGEN_SHADER_MODULES
	if(!qvk.use_ray_query)
	{
		LIST_RT_PIPELINE_SHADER_MODULES
	}
#undef IS_RT_SHADER

#undef SHADER_MODULE_DO
	return ret;
}

VkResult
vkpt_destroy_shader_modules()
{
	for (int i = 0; i < NUM_QVK_SHADER_MODULES; i++)
	{
		vkDestroyShaderModule(qvk.device, qvk.shader_modules[i], NULL);
		qvk.shader_modules[i] = VK_NULL_HANDLE;
	}

	return VK_SUCCESS;
}

VkResult
destroy_swapchain()
{
	for(int i = 0; i < qvk.num_swap_chain_images; i++) {
		vkDestroyImageView  (qvk.device, qvk.swap_chain_image_views[i], NULL);
		qvk.swap_chain_image_views[i] = VK_NULL_HANDLE;
	}
	free(qvk.swap_chain_image_views);
	qvk.swap_chain_image_views = NULL;

	free(qvk.swap_chain_images);
	qvk.swap_chain_images = NULL;

	qvk.num_swap_chain_images = 0;

	vkDestroySwapchainKHR(qvk.device, qvk.swap_chain, NULL);
	qvk.swap_chain = VK_NULL_HANDLE;

	return VK_SUCCESS;
}

int
destroy_vulkan()
{
	vkDeviceWaitIdle(qvk.device);

	destroy_swapchain();
	vkDestroySurfaceKHR(qvk.instance, qvk.surface,    NULL);

	for (int frame = 0; frame < MAX_FRAMES_IN_FLIGHT; frame++)
	{
		for (int gpu = 0; gpu < qvk.device_count; gpu++)
		{
			semaphore_group_t* group = &qvk.semaphores[frame][gpu];

			vkDestroySemaphore(qvk.device, group->image_available, NULL);
			vkDestroySemaphore(qvk.device, group->render_finished, NULL);
			vkDestroySemaphore(qvk.device, group->transfer_finished, NULL);
			vkDestroySemaphore(qvk.device, group->trace_finished, NULL);
		}
	}

	for(int i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
		vkDestroyFence(qvk.device, qvk.fences_frame_sync[i], NULL);
	}
	vkDestroyFence(qvk.device, qvk.fence_vertex_sync, NULL);

	vkpt_free_command_buffers(&qvk.cmd_buffers_graphics);
	vkpt_free_command_buffers(&qvk.cmd_buffers_transfer);

	vkDestroyCommandPool(qvk.device, qvk.cmd_buffers_graphics.command_pool, NULL);
	vkDestroyCommandPool(qvk.device, qvk.cmd_buffers_transfer.command_pool, NULL);

	vkDestroyDevice(qvk.device,   NULL);
	_VK(qvkDestroyDebugUtilsMessengerEXT(qvk.instance, qvk.dbg_messenger, NULL));
	vkDestroyInstance(qvk.instance, NULL);

	free(qvk.extensions);
	qvk.extensions = NULL;
	qvk.num_extensions = 0;

	free(qvk.layers);
	qvk.layers = NULL;
	qvk.num_layers = 0;

	// Clear the extension function pointers to make sure they don't refer non-requested extensions after vid_restart
#define VK_EXTENSION_DO(a) q##a = NULL;
	LIST_EXTENSIONS_ACCEL_STRUCT
	LIST_EXTENSIONS_RAY_PIPELINE
	LIST_EXTENSIONS_DEBUG
	LIST_EXTENSIONS_INSTANCE
#undef VK_EXTENSION_DO

	return 0;
}

typedef struct entity_hash_s {
	unsigned int mesh : 8;
	unsigned int model : 9;
	unsigned int entity : 15;
} entity_hash_t;

static int entity_frame_num = 0;
static int model_entity_ids[2][MAX_ENTITIES];
static int world_entity_ids[2][MAX_ENTITIES];
static int model_entity_id_count[2];
static int world_entity_id_count[2];
static int iqm_matrix_count[2];

#define MAX_MODEL_LIGHTS 16384
static int num_model_lights = 0;
static light_poly_t model_lights[MAX_MODEL_LIGHTS];

static pbr_material_t const * get_mesh_material(const entity_t* entity, const maliasmesh_t* mesh)
{
	if (entity->skin)
	{
		return MAT_ForSkin(IMG_ForHandle(entity->skin));
	}

	int skinnum = 0;
	if (mesh->materials[entity->skinnum])
		skinnum = entity->skinnum;

	return mesh->materials[skinnum];
}

static inline uint32_t fill_model_instance(const entity_t* entity, const model_t* model, const maliasmesh_t* mesh,
	const float* transform, int model_instance_index, qboolean is_viewer_weapon, qboolean is_double_sided, int iqm_matrix_index)
{
	pbr_material_t const * material = get_mesh_material(entity, mesh);

	if (!material)
	{
		Com_EPrintf("Cannot find material for model '%s'\n", model->name);
		return 0;
	}

	int material_id = material->flags;

	if(MAT_IsKind(material_id, MATERIAL_KIND_INVISIBLE))
		return 0; // skip the mesh

	if(MAT_IsKind(material_id, MATERIAL_KIND_CHROME))
		material_id = MAT_SetKind(material_id, MATERIAL_KIND_CHROME_MODEL);

	if (model->model_class == MCLASS_EXPLOSION)
	{
		material_id = MAT_SetKind(material_id, MATERIAL_KIND_EXPLOSION);
		material_id |= MATERIAL_FLAG_LIGHT;
	}

	if (is_viewer_weapon)
		material_id |= MATERIAL_FLAG_WEAPON;

	if (is_double_sided)
		material_id |= MATERIAL_FLAG_DOUBLE_SIDED;

	if (!MAT_IsKind(material_id, MATERIAL_KIND_GLASS))  
	{
		if (entity->flags & RF_SHELL_RED)
			material_id |= MATERIAL_FLAG_SHELL_RED;
		if (entity->flags & RF_SHELL_GREEN)
			material_id |= MATERIAL_FLAG_SHELL_GREEN;
		if (entity->flags & RF_SHELL_BLUE)
			material_id |= MATERIAL_FLAG_SHELL_BLUE;
	}

	ModelInstance* instance = &vkpt_refdef.uniform_instance_buffer.model_instances[model_instance_index];

	int frame = entity->frame;
	int oldframe = entity->oldframe;
	if (frame >= model->numframes) frame = 0;
	if (oldframe >= model->numframes) oldframe = 0;

	memcpy(instance->M, transform, sizeof(float) * 16);
	instance->idx_offset = mesh->idx_offset;
	instance->model_index = model - r_models;
	instance->offset_curr = mesh->vertex_offset + frame    * mesh->numverts * (sizeof(model_vertex_t) / sizeof(uint32_t));
	instance->offset_prev = mesh->vertex_offset + oldframe * mesh->numverts * (sizeof(model_vertex_t) / sizeof(uint32_t));
	instance->backlerp = entity->backlerp;
	instance->material = material_id;
	instance->alpha = (entity->flags & RF_TRANSLUCENT) ? entity->alpha : 1.0f;
	instance->is_iqm = (model->iqmData) ? 1 : 0;
	if (instance->is_iqm)
		instance->offset_prev = iqm_matrix_index;

	return material_id;
}

static void
add_dlights(const dlight_t* lights, int num_lights, QVKUniformBuffer_t* ubo)
{
	ubo->num_sphere_lights = 0;

	for (int i = 0; i < num_lights; i++)
	{
		const dlight_t* light = lights + i;

		float* dynlight_data = (float*)(ubo->sphere_light_data + ubo->num_sphere_lights * 2);
		float* center = dynlight_data;
		float* radius = dynlight_data + 3;
		float* color = dynlight_data + 4;
		dynlight_data[7] = 0.f;

		VectorCopy(light->origin, center);
		VectorScale(light->color, light->intensity / 25.f, color);
		*radius = light->radius;

		ubo->num_sphere_lights++;
	}
}

static inline void transform_point(const float* p, const float* matrix, float* result)
{
	vec4_t point = { p[0], p[1], p[2], 1.f };
	vec4_t transformed;
	mult_matrix_vector(transformed, matrix, point);
	VectorCopy(transformed, result); // vec4 -> vec3
}

static void instance_model_lights(int num_light_polys, const light_poly_t* light_polys, const float* transform)
{
	for (int nlight = 0; nlight < num_light_polys; nlight++)
	{
		if (num_model_lights >= MAX_MODEL_LIGHTS)
		{
			assert(!"Model light count overflow");
			break;
		}

		const light_poly_t* src_light = light_polys + nlight;
		light_poly_t* dst_light = model_lights + num_model_lights;

		// Transform the light's positions and center
		transform_point(src_light->positions + 0, transform, dst_light->positions + 0);
		transform_point(src_light->positions + 3, transform, dst_light->positions + 3);
		transform_point(src_light->positions + 6, transform, dst_light->positions + 6);
		transform_point(src_light->off_center, transform, dst_light->off_center);

		// Find the cluster based on the center. Maybe it's OK to use the model's cluster, need to test.
		dst_light->cluster = BSP_PointLeaf(bsp_world_model->nodes, dst_light->off_center)->cluster;

		// We really need to map these lights to a cluster
		if (dst_light->cluster < 0)
			continue;

		// Copy the other light properties
		VectorCopy(src_light->color, dst_light->color);
		dst_light->material = src_light->material;
		dst_light->style = src_light->style;

		num_model_lights++;
	}
}

static void process_bsp_entity(const entity_t* entity, int* bsp_mesh_idx, int* instance_idx, int* num_instanced_vert)
{
	QVKInstanceBuffer_t* uniform_instance_buffer = &vkpt_refdef.uniform_instance_buffer;
	uint32_t* ubo_bsp_cluster_id = (uint32_t*)uniform_instance_buffer->bsp_cluster_id;
	uint32_t* ubo_bsp_prim_offset = (uint32_t*)uniform_instance_buffer->bsp_prim_offset;
	uint32_t* ubo_instance_buf_offset = (uint32_t*)uniform_instance_buffer->bsp_instance_buf_offset;
	uint32_t* ubo_instance_buf_size = (uint32_t*)uniform_instance_buffer->bsp_instance_buf_size;

	const int current_bsp_mesh_index = *bsp_mesh_idx;
	if (current_bsp_mesh_index >= SHADER_MAX_BSP_ENTITIES)
	{
		assert(!"BSP entity count overflow");
		return;
	}

	if (*instance_idx >= (SHADER_MAX_ENTITIES + SHADER_MAX_BSP_ENTITIES))
	{
		assert(!"Total entity count overflow");
		return;
	}

	world_entity_ids[entity_frame_num][current_bsp_mesh_index] = entity->id;

	float transform[16];
	create_entity_matrix(transform, (entity_t*)entity, qfalse);
	BspMeshInstance* ubo_instance_info = uniform_instance_buffer->bsp_mesh_instances + current_bsp_mesh_index;
	memcpy(&ubo_instance_info->M, transform, sizeof(transform));
	ubo_instance_info->frame = entity->frame;
	memset(ubo_instance_info->padding, 0, sizeof(ubo_instance_info->padding));

	bsp_model_t* model = vkpt_refdef.bsp_mesh_world.models + (~entity->model);

	vec3_t origin;
	transform_point(model->center, transform, origin);
	int cluster = BSP_PointLeaf(bsp_world_model->nodes, origin)->cluster;

	if (cluster < 0)
	{
		// In some cases, a model slides into a wall, like a push button, so that its center 
		// is no longer in any BSP node. We still need to assign a cluster to the model,
		// so try the corners of the model instead, see if any of them has a valid cluster.

		for (int corner = 0; corner < 8; corner++)
		{
			vec3_t corner_pt = {
				(corner & 1) ? model->aabb_max[0] : model->aabb_min[0],
				(corner & 2) ? model->aabb_max[1] : model->aabb_min[1],
				(corner & 4) ? model->aabb_max[2] : model->aabb_min[2]
			};

			vec3_t corner_pt_world;
			transform_point(corner_pt, transform, corner_pt_world);

			cluster = BSP_PointLeaf(bsp_world_model->nodes, corner_pt_world)->cluster;

			if(cluster >= 0)
				break;
		}
	}
	ubo_bsp_cluster_id[current_bsp_mesh_index] = cluster;

	ubo_bsp_prim_offset[current_bsp_mesh_index] = model->idx_offset / 3;
	
	const int mesh_vertex_num = model->idx_count;

	ubo_instance_buf_offset[current_bsp_mesh_index] = *num_instanced_vert / 3;
	ubo_instance_buf_size[current_bsp_mesh_index] = mesh_vertex_num / 3;
	
	((int*)uniform_instance_buffer->model_indices)[*instance_idx] = ~current_bsp_mesh_index;

	*num_instanced_vert += mesh_vertex_num;
	
	instance_model_lights(model->num_light_polys, model->light_polys, transform);

	(*bsp_mesh_idx)++;
	(*instance_idx)++;
}

static inline qboolean is_transparent_material(uint32_t material)
{
	return MAT_IsKind(material, MATERIAL_KIND_SLIME)
		|| MAT_IsKind(material, MATERIAL_KIND_WATER)
		|| MAT_IsKind(material, MATERIAL_KIND_GLASS)
		|| MAT_IsKind(material, MATERIAL_KIND_TRANSPARENT);
}

static inline qboolean is_masked_material(uint32_t material)
{
	const pbr_material_t* mat = MAT_ForIndex(material & MATERIAL_INDEX_MASK);
	
	return mat && mat->image_mask;
}

#define MESH_FILTER_TRANSPARENT 1
#define MESH_FILTER_OPAQUE 2
#define MESH_FILTER_MASKED 4
#define MESH_FILTER_ALL 7

static void process_regular_entity(
	const entity_t* entity, 
	const model_t* model, 
	qboolean is_viewer_weapon, 
	qboolean is_double_sided, 
	int* model_instance_idx, 
	int* instance_idx, 
	int* num_instanced_vert, 
	int mesh_filter, 
	qboolean* contains_transparent,
	qboolean* contains_masked,
	int* iqm_matrix_offset,
	float* iqm_matrix_data)
{
	QVKInstanceBuffer_t* uniform_instance_buffer = &vkpt_refdef.uniform_instance_buffer;
	uint32_t* ubo_instance_buf_offset = (uint32_t*)uniform_instance_buffer->model_instance_buf_offset;
	uint32_t* ubo_instance_buf_size = (uint32_t*)uniform_instance_buffer->model_instance_buf_size;
	uint32_t* ubo_model_idx_offset = (uint32_t*)uniform_instance_buffer->model_idx_offset;
	uint32_t* ubo_model_cluster_id = (uint32_t*)uniform_instance_buffer->model_cluster_id;

	float transform[16];
	create_entity_matrix(transform, (entity_t*)entity, is_viewer_weapon);
	
	int current_model_instance_index = *model_instance_idx;
	int current_instance_index = *instance_idx;
	int current_num_instanced_vert = *num_instanced_vert;

	if (contains_transparent)
		*contains_transparent = qfalse;

	int iqm_matrix_index = -1;
	if (model->iqmData && model->iqmData->num_poses)
	{
		iqm_matrix_index = *iqm_matrix_offset;
		
		if (iqm_matrix_index + model->iqmData->num_poses > MAX_IQM_MATRICES)
		{
			assert(!"IQM matrix buffer overflow");
			return;
		}
		
		R_ComputeIQMTransforms(model->iqmData, entity, iqm_matrix_data + (iqm_matrix_index * 12));
		
		*iqm_matrix_offset += (int)model->iqmData->num_poses;
	}

	for (int i = 0; i < model->nummeshes; i++)
	{
		const maliasmesh_t* mesh = model->meshes + i;

		if (current_model_instance_index >= SHADER_MAX_ENTITIES)
		{
			assert(!"Model entity count overflow");
			break;
		}

		if (current_instance_index >= (SHADER_MAX_ENTITIES + SHADER_MAX_BSP_ENTITIES))
		{
			assert(!"Total entity count overflow");
			break;
		}

		if (mesh->idx_offset < 0 || mesh->vertex_offset < 0)
		{
			// failed to upload the vertex data - don't instance this mesh
			continue;
		}

		uint32_t material_id = fill_model_instance(entity, model, mesh, transform,
			current_model_instance_index,is_viewer_weapon, is_double_sided, iqm_matrix_index);
		
		if (!material_id)
			continue;

		if (is_masked_material(material_id))
		{
			if (contains_masked)
				*contains_masked = qtrue;

			if (!(mesh_filter & MESH_FILTER_MASKED))
				continue;
		}
		else if (is_transparent_material(material_id))
		{
			if(contains_transparent)
				*contains_transparent = qtrue;

			if(!(mesh_filter & MESH_FILTER_TRANSPARENT))
				continue;
		}
		else
		{
			if (!(mesh_filter & MESH_FILTER_OPAQUE))
				continue;
		}

		entity_hash_t hash;
		hash.entity = entity->id;
		hash.model = entity->model;
		hash.mesh = i;

		model_entity_ids[entity_frame_num][current_model_instance_index] = *(uint32_t*)&hash;

		uint32_t cluster_id = ~0u;
		if(bsp_world_model) 
			cluster_id = BSP_PointLeaf(bsp_world_model->nodes, ((entity_t*)entity)->origin)->cluster;
		ubo_model_cluster_id[current_model_instance_index] = cluster_id;

		ubo_model_idx_offset[current_model_instance_index] = mesh->idx_offset;

		ubo_instance_buf_offset[current_model_instance_index] = current_num_instanced_vert / 3;
		ubo_instance_buf_size[current_model_instance_index] = mesh->numtris;

		((int*)uniform_instance_buffer->model_indices)[current_instance_index] = current_model_instance_index;

		current_model_instance_index++;
		current_instance_index++;
		current_num_instanced_vert += mesh->numtris * 3;
	}

	// add cylinder lights for wall lamps
	if (model->model_class == MCLASS_STATIC_LIGHT)
	{
		vec4_t begin, end, color;
		vec4_t offset1 = { 0.f, 0.5f, -10.f, 1.f };
		vec4_t offset2 = { 0.f, 0.5f,  10.f, 1.f };

		mult_matrix_vector(begin, transform, offset1);
		mult_matrix_vector(end, transform, offset2);
		VectorSet(color, 0.25f, 0.5f, 0.07f);

		vkpt_build_cylinder_light(model_lights, &num_model_lights, MAX_MODEL_LIGHTS, bsp_world_model, begin, end, color, 1.5f);
	}

	*model_instance_idx = current_model_instance_index;
	*instance_idx = current_instance_index;
	*num_instanced_vert = current_num_instanced_vert;
}

#if CL_RTX_SHADERBALLS
extern vec3_t cl_dev_shaderballs_pos;

void
vkpt_drop_shaderballs()
{
	VectorCopy(vkpt_refdef.fd->vieworg, cl_dev_shaderballs_pos);
	cl_dev_shaderballs_pos[2] -= 46.12f; // player eye-level
}
#endif

static void
prepare_entities(EntityUploadInfo* upload_info)
{
	entity_frame_num = !entity_frame_num;

	QVKInstanceBuffer_t* instance_buffer = &vkpt_refdef.uniform_instance_buffer;

	memcpy(instance_buffer->bsp_mesh_instances_prev, instance_buffer->bsp_mesh_instances,
		sizeof(instance_buffer->bsp_mesh_instances_prev));
	memcpy(instance_buffer->model_instances_prev, instance_buffer->model_instances,
		sizeof(instance_buffer->model_instances_prev));

	memcpy(instance_buffer->bsp_cluster_id_prev, instance_buffer->bsp_cluster_id, sizeof(instance_buffer->bsp_cluster_id));
	memcpy(instance_buffer->model_cluster_id_prev, instance_buffer->model_cluster_id, sizeof(instance_buffer->model_cluster_id));

	static int transparent_model_indices[MAX_ENTITIES];
	static int masked_model_indices[MAX_ENTITIES];
	static int viewer_model_indices[MAX_ENTITIES];
	static int viewer_weapon_indices[MAX_ENTITIES];
	static int explosion_indices[MAX_ENTITIES];
	int transparent_model_num = 0;
	int masked_model_num = 0;
	int viewer_model_num = 0;
	int viewer_weapon_num = 0;
	int explosion_num = 0;

	int model_instance_idx = 0;
	int bsp_mesh_idx = 0;
	int num_instanced_vert = 0; /* need to track this here to find lights */
	int instance_idx = 0;
	int iqm_matrix_offset = 0;

	const qboolean first_person_model = (cl_player_model->integer == CL_PLAYER_MODEL_FIRST_PERSON) && cl.baseclientinfo.model;

	for (int i = 0; i < vkpt_refdef.fd->num_entities; i++)
	{
		const entity_t* entity = vkpt_refdef.fd->entities + i;

		if (entity->model & 0x80000000)
		{
			const bsp_model_t* model = vkpt_refdef.bsp_mesh_world.models + (~entity->model);
			if (model->masked)
				masked_model_indices[masked_model_num++] = i;
			else if (model->transparent)
				transparent_model_indices[transparent_model_num++] = i;
			else
				process_bsp_entity(entity, &bsp_mesh_idx, &instance_idx, &num_instanced_vert); /* embedded in bsp */
		}
		else
		{
			const model_t* model = MOD_ForHandle(entity->model);
			if (model == NULL || model->meshes == NULL)
				continue;

			if (entity->flags & RF_VIEWERMODEL)
				viewer_model_indices[viewer_model_num++] = i;
			else if (entity->flags & RF_WEAPONMODEL)
				viewer_weapon_indices[viewer_weapon_num++] = i;
			else if (model->model_class == MCLASS_EXPLOSION || model->model_class == MCLASS_SMOKE)
				explosion_indices[explosion_num++] = i;
			else
			{
				qboolean contains_transparent = qfalse;
				qboolean contains_masked = qfalse;
				process_regular_entity(entity, model, qfalse, qfalse, &model_instance_idx, &instance_idx, &num_instanced_vert,
					MESH_FILTER_OPAQUE, &contains_transparent, &contains_masked, &iqm_matrix_offset, qvk.iqm_matrices_shadow);

				if (contains_transparent)
					transparent_model_indices[transparent_model_num++] = i;
				if (contains_masked)
					masked_model_indices[masked_model_num++] = i;
			}

			if (model->num_light_polys > 0)
			{
				float transform[16];
				const qboolean is_viewer_weapon = (entity->flags & RF_WEAPONMODEL) != 0;
				create_entity_matrix(transform, (entity_t*)entity, is_viewer_weapon);

				instance_model_lights(model->num_light_polys, model->light_polys, transform);
			}
		}
	}

	upload_info->dynamic_vertex_num = num_instanced_vert;

	const uint32_t transparent_model_base_vertex_num = num_instanced_vert;
	for (int i = 0; i < transparent_model_num; i++)
	{
		const entity_t* entity = vkpt_refdef.fd->entities + transparent_model_indices[i];

		if (entity->model & 0x80000000)
		{
			process_bsp_entity(entity, &bsp_mesh_idx, &instance_idx, &num_instanced_vert);
		}
		else
		{
			const model_t* model = MOD_ForHandle(entity->model);
			process_regular_entity(entity, model, qfalse, qfalse, &model_instance_idx, &instance_idx, &num_instanced_vert,
				MESH_FILTER_TRANSPARENT, NULL, NULL, &iqm_matrix_offset, qvk.iqm_matrices_shadow);
		}
	}

	upload_info->transparent_model_vertex_offset = transparent_model_base_vertex_num;
	upload_info->transparent_model_vertex_num = num_instanced_vert - transparent_model_base_vertex_num;

	const uint32_t masked_model_base_vertex_num = num_instanced_vert;
	for (int i = 0; i < masked_model_num; i++)
	{
		const entity_t* entity = vkpt_refdef.fd->entities + masked_model_indices[i];

		if (entity->model & 0x80000000)
		{
			process_bsp_entity(entity, &bsp_mesh_idx, &instance_idx, &num_instanced_vert);
		}
		else
		{
			const model_t* model = MOD_ForHandle(entity->model);
			process_regular_entity(entity, model, qfalse, qtrue, &model_instance_idx, &instance_idx, &num_instanced_vert,
				MESH_FILTER_MASKED, NULL, NULL, &iqm_matrix_offset, qvk.iqm_matrices_shadow);
		}
	}

	upload_info->masked_model_vertex_offset = masked_model_base_vertex_num;
	upload_info->masked_model_vertex_num = num_instanced_vert - masked_model_base_vertex_num;

	const uint32_t viewer_model_base_vertex_num = num_instanced_vert;
	if (first_person_model)
	{
		for (int i = 0; i < viewer_model_num; i++)
		{
			const entity_t* entity = vkpt_refdef.fd->entities + viewer_model_indices[i];
			const model_t* model = MOD_ForHandle(entity->model);
			process_regular_entity(entity, model, qfalse, qtrue, &model_instance_idx, &instance_idx, &num_instanced_vert,
				MESH_FILTER_ALL, NULL, NULL, &iqm_matrix_offset, qvk.iqm_matrices_shadow);
		}
	}

	upload_info->viewer_model_vertex_offset = viewer_model_base_vertex_num;
	upload_info->viewer_model_vertex_num = num_instanced_vert - viewer_model_base_vertex_num;

	upload_info->weapon_left_handed = qfalse;

	const uint32_t viewer_weapon_base_vertex_num = num_instanced_vert;
	for (int i = 0; i < viewer_weapon_num; i++)
	{
		const entity_t* entity = vkpt_refdef.fd->entities + viewer_weapon_indices[i];
		const model_t* model = MOD_ForHandle(entity->model);
		process_regular_entity(entity, model, qtrue, qfalse, &model_instance_idx, &instance_idx, &num_instanced_vert,
			MESH_FILTER_ALL, NULL, NULL, &iqm_matrix_offset, qvk.iqm_matrices_shadow);

		if (entity->flags & RF_LEFTHAND)
			upload_info->weapon_left_handed = qtrue;
	}

	upload_info->viewer_weapon_vertex_offset = viewer_weapon_base_vertex_num;
	upload_info->viewer_weapon_vertex_num = num_instanced_vert - viewer_weapon_base_vertex_num;

	const uint32_t explosion_base_vertex_num = num_instanced_vert;
	for (int i = 0; i < explosion_num; i++)
	{
		const entity_t* entity = vkpt_refdef.fd->entities + explosion_indices[i];
		const model_t* model = MOD_ForHandle(entity->model);
		process_regular_entity(entity, model, qfalse, qfalse, &model_instance_idx, &instance_idx, &num_instanced_vert,
			MESH_FILTER_ALL, NULL, NULL, &iqm_matrix_offset, qvk.iqm_matrices_shadow);
	}

	upload_info->explosions_vertex_offset = explosion_base_vertex_num;
	upload_info->explosions_vertex_num = num_instanced_vert - explosion_base_vertex_num;

	upload_info->num_instances = instance_idx;
	upload_info->num_vertices  = num_instanced_vert;

	memset(instance_buffer->world_current_to_prev, ~0u, sizeof(instance_buffer->world_current_to_prev));
	memset(instance_buffer->world_prev_to_current, ~0u, sizeof(instance_buffer->world_prev_to_current));
	memset(instance_buffer->model_current_to_prev, ~0u, sizeof(instance_buffer->model_current_to_prev));
	memset(instance_buffer->model_prev_to_current, ~0u, sizeof(instance_buffer->model_prev_to_current));

	world_entity_id_count[entity_frame_num] = bsp_mesh_idx;
	for(int i = 0; i < world_entity_id_count[entity_frame_num]; i++) {
		for(int j = 0; j < world_entity_id_count[!entity_frame_num]; j++) {
			if(world_entity_ids[entity_frame_num][i] == world_entity_ids[!entity_frame_num][j]) {
				instance_buffer->world_current_to_prev[i] = j;
				instance_buffer->world_prev_to_current[j] = i;
			}
		}
	}

	model_entity_id_count[entity_frame_num] = model_instance_idx;
	for(int i = 0; i < model_entity_id_count[entity_frame_num]; i++) {
		for(int j = 0; j < model_entity_id_count[!entity_frame_num]; j++) {
			entity_hash_t hash = *(entity_hash_t*)&model_entity_ids[entity_frame_num][i];

			if(model_entity_ids[entity_frame_num][i] == model_entity_ids[!entity_frame_num][j] && hash.entity != 0) {
				instance_buffer->model_current_to_prev[i] = j;
				instance_buffer->model_prev_to_current[j] = i;
			}
		}
	}

	// Store the number of IQM matrices for the next frame
	iqm_matrix_count[entity_frame_num] = iqm_matrix_offset;

	if (iqm_matrix_count[entity_frame_num] > 0)
	{
		// If we had some matrices previously...
		if (iqm_matrix_count[!entity_frame_num] > 0)
		{
			// Copy over the previous frame IQM matrices into an offset location in the current frame buffer
			memcpy(qvk.iqm_matrices_shadow + (iqm_matrix_count[entity_frame_num] * 12),
				qvk.iqm_matrices_prev, iqm_matrix_count[!entity_frame_num] * 12 * sizeof(float));

			// Patch the previous model instances to point at the offset matrices
			for (int i = 0; i < model_entity_id_count[!entity_frame_num]; i++)
			{
				ModelInstance* instance = &instance_buffer->model_instances_prev[i];
				if (instance->is_iqm) {
					// Offset = current matrix count
					instance->offset_prev += iqm_matrix_count[entity_frame_num];
				}
			}
		}

		// Store the current matrices for the next frame
		memcpy(qvk.iqm_matrices_prev, qvk.iqm_matrices_shadow, iqm_matrix_count[entity_frame_num] * 12 * sizeof(float));

		// Upload the current matrices to the staging buffer
		IqmMatrixBuffer* iqm_matrix_staging = buffer_map(&qvk.buf_iqm_matrices_staging[qvk.current_frame_index]);

		int total_matrix_count = (iqm_matrix_count[entity_frame_num] + iqm_matrix_count[!entity_frame_num]);
		memcpy(iqm_matrix_staging, qvk.iqm_matrices_shadow, total_matrix_count * 12 * sizeof(float));

		buffer_unmap(&qvk.buf_iqm_matrices_staging[qvk.current_frame_index]);
	}
}

#ifdef VKPT_IMAGE_DUMPS
static void 
copy_to_dump_texture(VkCommandBuffer cmd_buf, int src_image_index)
{
	VkImage src_image = qvk.images[src_image_index];
	VkImage dst_image = qvk.dump_image;

	VkImageCopy image_copy_region = {
		.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
		.srcSubresource.mipLevel = 0,
		.srcSubresource.baseArrayLayer = 0,
		.srcSubresource.layerCount = 1,

		.srcOffset.x = 0,
		.srcOffset.y = 0,
		.srcOffset.z = 0,

		.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
		.dstSubresource.mipLevel = 0,
		.dstSubresource.baseArrayLayer = 0,
		.dstSubresource.layerCount = 1,

		.dstOffset.x = 0,
		.dstOffset.y = 0,
		.dstOffset.z = 0,

		.extent.width = IMG_WIDTH,
		.extent.height = IMG_HEIGHT,
		.extent.depth = 1
	};

	VkImageSubresourceRange subresource_range = {
		.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
		.baseMipLevel = 0,
		.levelCount = 1,
		.baseArrayLayer = 0,
		.layerCount = 1
	};

	IMAGE_BARRIER(cmd_buf,
		.image = src_image,
		.subresourceRange = subresource_range,
		.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
		.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
		.oldLayout = VK_IMAGE_LAYOUT_GENERAL,
		.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL
	);

	IMAGE_BARRIER(cmd_buf,
		.image = dst_image,
		.subresourceRange = subresource_range,
		.srcAccessMask = VK_ACCESS_HOST_READ_BIT,
		.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
		.oldLayout = VK_IMAGE_LAYOUT_GENERAL,
		.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL
	);

	vkCmdCopyImage(cmd_buf,
		src_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
		dst_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		1, &image_copy_region);

	IMAGE_BARRIER(cmd_buf,
		.image = src_image,
		.subresourceRange = subresource_range,
		.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
		.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
		.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
		.newLayout = VK_IMAGE_LAYOUT_GENERAL
	);

	IMAGE_BARRIER(cmd_buf,
		.image = dst_image,
		.subresourceRange = subresource_range,
		.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
		.dstAccessMask = VK_ACCESS_HOST_READ_BIT,
		.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		.newLayout = VK_IMAGE_LAYOUT_GENERAL
	);
}
#endif

VkDescriptorSet qvk_get_current_desc_set_textures()
{
	return (qvk.frame_counter & 1) ? qvk.desc_set_textures_odd : qvk.desc_set_textures_even;
}

static void
process_render_feedback(ref_feedback_t *feedback, mleaf_t* viewleaf, qboolean* sun_visible, float* adapted_luminance)
{
	if (viewleaf)
		feedback->viewcluster = viewleaf->cluster;
	else
		feedback->viewcluster = -1;

	{
		static char const * unknown = "<unknown>";
		char const * view_material = unknown;
		char const * view_material_override = unknown;
		ReadbackBuffer readback;
		vkpt_readback(&readback);
		if (readback.material != ~0u)
		{
			int material_id = readback.material & MATERIAL_INDEX_MASK;
			feedback->view_material_index = material_id;
			pbr_material_t const* material = MAT_ForIndex(material_id);
			if (material)
			{
				image_t const* image = material->image_base;
				if (image)
				{
					view_material = image->name;
					view_material_override = image->filepath;
				}
			}
		}
		else
			feedback->view_material_index = -1;
		strcpy(feedback->view_material, view_material);
		strcpy(feedback->view_material_override, view_material_override);

		feedback->lookatcluster = readback.cluster;
		feedback->num_light_polys = 0;

		if (vkpt_refdef.bsp_mesh_world_loaded && feedback->lookatcluster >= 0 && feedback->lookatcluster < vkpt_refdef.bsp_mesh_world.num_clusters)
		{
			int* light_offsets = vkpt_refdef.bsp_mesh_world.cluster_light_offsets + feedback->lookatcluster;
			feedback->num_light_polys = light_offsets[1] - light_offsets[0];
		}

		VectorCopy(readback.hdr_color, feedback->hdr_color);

		*sun_visible = readback.sun_luminance > 0.f;
		*adapted_luminance = readback.adapted_luminance;
	}
}

typedef struct reference_mode_s 
{
	qboolean enable_accumulation;
	qboolean enable_denoiser;
	float num_bounce_rays;
	float temporal_blend_factor;
	int reflect_refract;
} reference_mode_t;

static int
get_accumulation_rendering_framenum()
{
	return max(128, cvar_pt_accumulation_rendering_framenum->integer);
}

static qboolean is_accumulation_rendering_active()
{
	return cl_paused->integer == 2 && sv_paused->integer && cvar_pt_accumulation_rendering->integer > 0;
}

static void draw_shadowed_string(int x, int y, int flags, size_t maxlen, const char* s)
{
	R_SetColor(0xff000000u);
	SCR_DrawStringEx(x + 1, y + 1, flags, maxlen, s, SCR_GetFont());
	R_SetColor(~0u);
	SCR_DrawStringEx(x, y, flags, maxlen, s, SCR_GetFont());
}

static void
evaluate_reference_mode(reference_mode_t* ref_mode)
{
	if (is_accumulation_rendering_active())
	{
		num_accumulated_frames++;

		const int num_warmup_frames = 5;
		const int num_frames_to_accumulate = get_accumulation_rendering_framenum();

		ref_mode->enable_accumulation = qtrue;
		ref_mode->enable_denoiser = qfalse;
		ref_mode->num_bounce_rays = 2;
		ref_mode->temporal_blend_factor = 1.f / min(max(1, num_accumulated_frames - num_warmup_frames), num_frames_to_accumulate);
		ref_mode->reflect_refract = max(4, cvar_pt_reflect_refract->integer);

		switch (cvar_pt_accumulation_rendering->integer)
		{
		case 1: {
			char text[MAX_QPATH];
			float percentage = powf(max(0.f, (num_accumulated_frames - num_warmup_frames) / (float)num_frames_to_accumulate), 0.5f);
			Q_snprintf(text, sizeof(text), "Photo mode: accumulating samples... %d%%", (int)(min(1.f, percentage) * 100.f));

			int frames_after_accumulation_finished = num_accumulated_frames - num_warmup_frames - num_frames_to_accumulate;
			float hud_alpha = max(0.f, min(1.f, (50 - frames_after_accumulation_finished) * 0.02f)); // fade out for 50 frames after accumulation finishes

			int x = r_config.width / 4;
			int y = 30;
			R_SetScale(0.5f);
			R_SetAlphaScale(hud_alpha);
			draw_shadowed_string(x, y, UI_CENTER, MAX_QPATH, text);

			if (cvar_pt_dof->integer)
			{
				x = 5;
				y = r_config.height / 2 - 55;
				Q_snprintf(text, sizeof(text), "Focal Distance: %.1f", cvar_pt_focus->value);
				draw_shadowed_string(x, y, UI_LEFT, MAX_QPATH, text);

				y += 10;
				Q_snprintf(text, sizeof(text), "Aperture: %.2f", cvar_pt_aperture->value);
				draw_shadowed_string(x, y, UI_LEFT, MAX_QPATH, text);

				y += 10;
				draw_shadowed_string(x, y, UI_LEFT, MAX_QPATH, "Use Mouse Wheel, Shift, Ctrl to adjust");
			}

			R_SetAlphaScale(1.f);

			SCR_SetHudAlpha(hud_alpha);
			break;
		}
		case 2:
			SCR_SetHudAlpha(0.f);
			break;
		}
	}
	else
	{
		num_accumulated_frames = 0;

		ref_mode->enable_accumulation = qfalse;
		ref_mode->enable_denoiser = !!cvar_flt_enable->integer;
		if (cvar_pt_num_bounce_rays->value == 0.5f)
			ref_mode->num_bounce_rays = 0.5f;
		else
			ref_mode->num_bounce_rays = max(0, min(2, round(cvar_pt_num_bounce_rays->value)));
		ref_mode->temporal_blend_factor = 0.f;
		ref_mode->reflect_refract = max(0, cvar_pt_reflect_refract->integer);
	}

	ref_mode->reflect_refract = min(10, ref_mode->reflect_refract);
}

static void
evaluate_taa_settings(const reference_mode_t* ref_mode)
{
	qvk.effective_aa_mode = AA_MODE_OFF;
	qvk.extent_taa_output = qvk.extent_render;

	if (!ref_mode->enable_denoiser)
		return;

	if (cvar_flt_taa->integer == AA_MODE_TAA)
	{
		qvk.effective_aa_mode = AA_MODE_TAA;
	}
	else if (cvar_flt_taa->integer == AA_MODE_UPSCALE)
	{
		if (qvk.extent_render.width > qvk.extent_unscaled.width || qvk.extent_render.height > qvk.extent_unscaled.height)
		{
			qvk.effective_aa_mode = AA_MODE_TAA;
		}
		else
		{
			qvk.effective_aa_mode = AA_MODE_UPSCALE;
			qvk.extent_taa_output = qvk.extent_unscaled;
		}
	}
}

static void
prepare_sky_matrix(float time, vec3_t sky_matrix[3])
{
	if (sky_rotation != 0.f)
	{
		SetupRotationMatrix(sky_matrix, sky_axis, time * sky_rotation);
	}
	else
	{
		VectorSet(sky_matrix[0], 1.f, 0.f, 0.f);
		VectorSet(sky_matrix[1], 0.f, 1.f, 0.f);
		VectorSet(sky_matrix[2], 0.f, 0.f, 1.f);
	}
}

static void
prepare_camera(const vec3_t position, const vec3_t direction, mat4_t data)
{
	vec3_t forward, right, up;
	VectorCopy(direction, forward);
	VectorNormalize(forward);

	if (fabs(forward[2]) < 0.99f)
		VectorSet(up, 0.f, 0.f, 1.f);
	else
		VectorSet(up, 0.f, 1.f, 0.f);

	CrossProduct(forward, up, right);
	CrossProduct(right, forward, up);
	VectorNormalize(up);
	VectorNormalize(right);

	float aspect = 1.75f;
	float tan_half_fov_x = 1.f;
	float tan_half_fov_y = tan_half_fov_x / aspect;

	VectorCopy(position, data + 0);
	VectorCopy(forward, data + 4);
	VectorMA(data + 4, -tan_half_fov_x, right, data + 4);
	VectorMA(data + 4, tan_half_fov_y, up, data + 4);
	VectorScale(right, 2.f * tan_half_fov_x, data + 8);
	VectorScale(up, -2.f * tan_half_fov_y, data + 12);
}

static void
prepare_ubo(refdef_t *fd, mleaf_t* viewleaf, const reference_mode_t* ref_mode, const vec3_t sky_matrix[3], qboolean render_world)
{
	float P[16];
	float V[16];

	QVKUniformBuffer_t *ubo = &vkpt_refdef.uniform_buffer;
	memcpy(ubo->V_prev, ubo->V, sizeof(float) * 16);
	memcpy(ubo->P_prev, ubo->P, sizeof(float) * 16);
	memcpy(ubo->invP_prev, ubo->invP, sizeof(float) * 16);
	ubo->cylindrical_hfov_prev = ubo->cylindrical_hfov;
	ubo->prev_taa_output_width = ubo->taa_output_width;
	ubo->prev_taa_output_height = ubo->taa_output_height;

	{
		float raw_proj[16];
		create_projection_matrix(raw_proj, vkpt_refdef.z_near, vkpt_refdef.z_far, fd->fov_x, fd->fov_y);

		// In some cases (ex.: player setup), 'fd' will describe a viewport that is not full screen.
		// Simulate that with a projection matrix adjustment to avoid modifying the rendering code.

		float viewport_proj[16] = {
			[0] = (float)fd->width / (float)qvk.extent_unscaled.width,
			[12] = (float)(fd->x * 2 + fd->width - (int)qvk.extent_unscaled.width) / (float)qvk.extent_unscaled.width,
			[5] = (float)fd->height / (float)qvk.extent_unscaled.height,
			[13] = -(float)(fd->y * 2 + fd->height - (int)qvk.extent_unscaled.height) / (float)qvk.extent_unscaled.height,
			[10] = 1.f,
			[15] = 1.f
		};

		mult_matrix_matrix(P, viewport_proj, raw_proj);
	}
	create_view_matrix(V, fd);
	memcpy(ubo->V, V, sizeof(float) * 16);
	memcpy(ubo->P, P, sizeof(float) * 16);
	inverse(V, ubo->invV);
	inverse(P, ubo->invP);

	if (cvar_pt_projection->integer == 1 && render_world)
	{
		float rad_per_pixel = atanf(tanf(fd->fov_y * M_PI / 360.0f) / ((float)qvk.extent_unscaled.height * 0.5f));
		ubo->cylindrical_hfov = rad_per_pixel * (float)qvk.extent_unscaled.width;
	}
	else
	{
		ubo->cylindrical_hfov = 0.f;
	}
	
	ubo->current_frame_idx = qvk.frame_counter;
	ubo->width = qvk.extent_render.width;
	ubo->height = qvk.extent_render.height;
	ubo->prev_width = qvk.extent_render_prev.width;
	ubo->prev_height = qvk.extent_render_prev.height;
	ubo->inv_width = 1.0f / (float)qvk.extent_render.width;
	ubo->inv_height = 1.0f / (float)qvk.extent_render.height;
	ubo->unscaled_width = qvk.extent_unscaled.width;
	ubo->unscaled_height = qvk.extent_unscaled.height;
	ubo->taa_image_width = qvk.extent_taa_images.width;
	ubo->taa_image_height = qvk.extent_taa_images.height;
	ubo->taa_output_width = qvk.extent_taa_output.width;
	ubo->taa_output_height = qvk.extent_taa_output.height;
	ubo->current_gpu_slice_width = qvk.gpu_slice_width;
	ubo->prev_gpu_slice_width = qvk.gpu_slice_width_prev;
	ubo->screen_image_width = qvk.extent_screen_images.width;
	ubo->screen_image_height = qvk.extent_screen_images.height;
	ubo->water_normal_texture = water_normal_texture - r_images;
	ubo->pt_swap_checkerboard = 0;
	qvk.extent_render_prev = qvk.extent_render;
	qvk.gpu_slice_width_prev = qvk.gpu_slice_width;

	int camera_cluster_contents = viewleaf ? viewleaf->contents : 0;

	if (camera_cluster_contents & CONTENTS_WATER)
		ubo->medium = MEDIUM_WATER;
	else if (camera_cluster_contents & CONTENTS_SLIME)
		ubo->medium = MEDIUM_SLIME;
	else if (camera_cluster_contents & CONTENTS_LAVA)
		ubo->medium = MEDIUM_LAVA;
	else
		ubo->medium = MEDIUM_NONE;

	ubo->time = fd->time;
	ubo->num_static_primitives = (vkpt_refdef.bsp_mesh_world.world_idx_count + vkpt_refdef.bsp_mesh_world.world_transparent_count + vkpt_refdef.bsp_mesh_world.world_masked_count) / 3;
	ubo->num_static_lights = vkpt_refdef.bsp_mesh_world.num_light_polys;

#define UBO_CVAR_DO(name, default_value) ubo->name = cvar_##name->value;
	UBO_CVAR_LIST
#undef UBO_CVAR_DO

	if (!ref_mode->enable_denoiser)
	{
		// disable fake specular because it is not supported without denoiser, and the result
		// looks too dark with it missing
		ubo->pt_fake_roughness_threshold = 1.f;

		// swap the checkerboard fields every frame in reference or noisy mode to accumulate 
		// both reflection and refraction in every pixel
		ubo->pt_swap_checkerboard = (qvk.frame_counter & 1);

		if (ref_mode->enable_accumulation)
		{
			ubo->pt_texture_lod_bias = -log2(sqrt(get_accumulation_rendering_framenum()));

			// disable the other stabilization hacks
			ubo->pt_specular_anti_flicker = 0.f;
			ubo->pt_sun_bounce_range = 10000.f;
			ubo->pt_ndf_trim = 1.f;
		}
	}
	else if(qvk.effective_aa_mode == AA_MODE_UPSCALE)
	{
		// adjust texture LOD bias to the resolution scale, i.e. use negative bias if scale is < 100
		float resolution_scale = (drs_effective_scale != 0) ? (float)drs_effective_scale : (float)scr_viewsize->integer;
		resolution_scale *= 0.01f;
		clamp(resolution_scale, 0.1f, 1.f);
		ubo->pt_texture_lod_bias = cvar_pt_texture_lod_bias->value + log2f(resolution_scale);
	}

	{
		// figure out if DoF should be enabled in the current rendering mode

		qboolean enable_dof = qtrue;

		switch (cvar_pt_dof->integer)
		{
		case 0: enable_dof = qfalse; break;
		case 1: enable_dof = ref_mode->enable_accumulation; break;
		case 2: enable_dof = !ref_mode->enable_denoiser; break;
		default: enable_dof = qtrue; break;
		}

		if (cvar_pt_projection->integer != 0)
		{
			// DoF does not make physical sense with the cylindrical projection
			enable_dof = qfalse;
		}

		if (!enable_dof)
		{
			// if DoF should not be enabled, make the aperture size zero
			ubo->pt_aperture = 0.f;
		}
	}

	// number of polygon vertices must be an integer
	ubo->pt_aperture_type = roundf(ubo->pt_aperture_type);

	ubo->temporal_blend_factor = ref_mode->temporal_blend_factor;
	ubo->flt_enable = ref_mode->enable_denoiser;
	ubo->flt_taa = qvk.effective_aa_mode;
	ubo->pt_num_bounce_rays = ref_mode->num_bounce_rays;
	ubo->pt_reflect_refract = ref_mode->reflect_refract;

	if (ref_mode->num_bounce_rays < 1.f)
		ubo->pt_specular_mis = 0; // disable MIS if there are no specular rays

	ubo->pt_min_log_sky_luminance = exp2f(ubo->pt_min_log_sky_luminance);
	ubo->pt_max_log_sky_luminance = exp2f(ubo->pt_max_log_sky_luminance);

	memcpy(ubo->cam_pos, fd->vieworg, sizeof(float) * 3);
	ubo->cluster_debug_index = cluster_debug_index;

	if (!temporal_frame_valid)
	{
		ubo->flt_temporal_lf = 0;
		ubo->flt_temporal_hf = 0;
		ubo->flt_temporal_spec = 0;
		ubo->flt_taa = 0;
	}

	if (qvk.effective_aa_mode == AA_MODE_UPSCALE)
	{
		int taa_index = (int)(qvk.frame_counter % NUM_TAA_SAMPLES);
		ubo->sub_pixel_jitter[0] = taa_samples[taa_index][0];
		ubo->sub_pixel_jitter[1] = taa_samples[taa_index][1];
	}
	else
	{
		ubo->sub_pixel_jitter[0] = 0.f;
		ubo->sub_pixel_jitter[1] = 0.f;
	}

	ubo->first_person_model = cl_player_model->integer == CL_PLAYER_MODEL_FIRST_PERSON;

	memset(ubo->environment_rotation_matrix, 0, sizeof(ubo->environment_rotation_matrix));
	VectorCopy(sky_matrix[0], ubo->environment_rotation_matrix + 0);
	VectorCopy(sky_matrix[1], ubo->environment_rotation_matrix + 4);
	VectorCopy(sky_matrix[2], ubo->environment_rotation_matrix + 8);
	
	add_dlights(vkpt_refdef.fd->dlights, vkpt_refdef.fd->num_dlights, ubo);

	const bsp_mesh_t* wm = &vkpt_refdef.bsp_mesh_world;
	if (wm->num_cameras > 0)
	{
		for (int n = 0; n < wm->num_cameras; n++)
		{
			prepare_camera(wm->cameras[n].pos, wm->cameras[n].dir, ubo->security_camera_data[n]);
		}
	}
	else
	{
		ubo->pt_cameras = 0;
	}

	ubo->num_cameras = wm->num_cameras;
}


/* renders the map ingame */
void
R_RenderFrame_RTX(refdef_t *fd)
{
	if (!qvk.swap_chain)
		return;

	vkpt_refdef.fd = fd;
	qboolean render_world = (fd->rdflags & RDF_NOWORLDMODEL) == 0;

	static float previous_time = -1.f;
	float frame_time = min(1.f, max(0.f, fd->time - previous_time));
	previous_time = fd->time;

	vkpt_freecam_update(cls.frametime);

	static unsigned previous_wallclock_time = 0;
	unsigned current_wallclock_time = Sys_Milliseconds();
	float frame_wallclock_time = (previous_wallclock_time != 0) ? (float)(current_wallclock_time - previous_wallclock_time) * 1e-3f : 0.f;
	previous_wallclock_time = current_wallclock_time;

	if (!temporal_frame_valid)
	{
		if (vkpt_refdef.fd && vkpt_refdef.fd->lightstyles)
			memcpy(vkpt_refdef.prev_lightstyles, vkpt_refdef.fd->lightstyles, sizeof(vkpt_refdef.prev_lightstyles));
		else
			memset(vkpt_refdef.prev_lightstyles, 0, sizeof(vkpt_refdef.prev_lightstyles));
	}

	mleaf_t* viewleaf = bsp_world_model ? BSP_PointLeaf(bsp_world_model->nodes, fd->vieworg) : NULL;
	
	qboolean sun_visible_prev = qfalse;
	static float prev_adapted_luminance = 0.f;
	float adapted_luminance = 0.f;
	process_render_feedback(&fd->feedback, viewleaf, &sun_visible_prev, &adapted_luminance);

	// Sometimes, the readback returns 1.0 luminance instead of the real value.
	// Ignore these mysterious spikes.
	if (adapted_luminance != 1.0f) 
		prev_adapted_luminance = adapted_luminance;
	
	if (prev_adapted_luminance <= 0.f)
		prev_adapted_luminance = 0.005f;

	LOG_FUNC();
	if (!vkpt_refdef.bsp_mesh_world_loaded && render_world)
		return;

	vec3_t sky_matrix[3];
	prepare_sky_matrix(fd->time, sky_matrix);

	sun_light_t sun_light = { 0 };
	if (render_world)
	{
		vkpt_evaluate_sun_light(&sun_light, sky_matrix, fd->time);

		if (!vkpt_physical_sky_needs_update())
			sun_light.visible = sun_light.visible && sun_visible_prev;
	}

	reference_mode_t ref_mode;
	evaluate_reference_mode(&ref_mode);
	evaluate_taa_settings(&ref_mode);
	
	qboolean menu_mode = cl_paused->integer == 1 && uis.menuDepth > 0 && render_world;

	int new_world_anim_frame = (int)(fd->time * 2);
	qboolean update_world_animations = (new_world_anim_frame != world_anim_frame);
	world_anim_frame = new_world_anim_frame;

	num_model_lights = 0;
	EntityUploadInfo upload_info = { 0 };
	prepare_entities(&upload_info);
	if (bsp_world_model)
	{
		vkpt_build_beam_lights(model_lights, &num_model_lights, MAX_MODEL_LIGHTS, bsp_world_model, fd->entities, fd->num_entities, prev_adapted_luminance);
	}

	QVKUniformBuffer_t *ubo = &vkpt_refdef.uniform_buffer;
	prepare_ubo(fd, viewleaf, &ref_mode, sky_matrix, render_world);
	ubo->prev_adapted_luminance = prev_adapted_luminance;

	if (cvar_tm_blend_enable->integer)
		Vector4Copy(fd->blend, ubo->fs_blend_color);
	else
		Vector4Set(ubo->fs_blend_color, 0.f, 0.f, 0.f, 0.f);

	vkpt_physical_sky_update_ubo(ubo, &sun_light, render_world);
	vkpt_bloom_update(ubo, frame_time, ubo->medium != MEDIUM_NONE, menu_mode);

	if(update_world_animations)
		bsp_mesh_animate_light_polys(&vkpt_refdef.bsp_mesh_world);
	vec3_t sky_radiance;
	VectorScale(avg_envmap_color, ubo->pt_env_scale, sky_radiance);
	vkpt_light_buffer_upload_to_staging(render_world, &vkpt_refdef.bsp_mesh_world, bsp_world_model, num_model_lights, model_lights, sky_radiance);
	
	float shadowmap_view_proj[16];
	float shadowmap_depth_scale;
	vkpt_shadow_map_setup(
		&sun_light,
		vkpt_refdef.bsp_mesh_world.world_aabb.mins,
		vkpt_refdef.bsp_mesh_world.world_aabb.maxs,
		shadowmap_view_proj,
		&shadowmap_depth_scale,
		ref_mode.enable_accumulation && num_accumulated_frames > 1);

	vkpt_god_rays_prepare_ubo(
		ubo,
		&vkpt_refdef.bsp_mesh_world.world_aabb,
		ubo->P,
		ubo->V,
		shadowmap_view_proj,
		shadowmap_depth_scale);

	qboolean god_rays_enabled = vkpt_god_rays_enabled(&sun_light) && render_world;

	VkSemaphore transfer_semaphores[VKPT_MAX_GPUS];
	VkSemaphore trace_semaphores[VKPT_MAX_GPUS];
	VkSemaphore prev_trace_semaphores[VKPT_MAX_GPUS];
	VkPipelineStageFlags wait_stages[VKPT_MAX_GPUS];
	uint32_t device_indices[VKPT_MAX_GPUS];
	uint32_t all_device_mask = (1 << qvk.device_count) - 1;
	qboolean* prev_trace_signaled = &qvk.semaphores[(qvk.current_frame_index - 1) % MAX_FRAMES_IN_FLIGHT][0].trace_signaled;
	qboolean* curr_trace_signaled = &qvk.semaphores[qvk.current_frame_index][0].trace_signaled;

	{
		// Transfer the light buffer from staging into device memory.
		// Previous frame's tracing still uses device memory, so only do the copy after that is finished.

		VkCommandBuffer transfer_cmd_buf = vkpt_begin_command_buffer(&qvk.cmd_buffers_transfer);

		vkpt_light_buffer_upload_staging(transfer_cmd_buf);
		vkpt_iqm_matrix_buffer_upload_staging(transfer_cmd_buf);

		for (int gpu = 0; gpu < qvk.device_count; gpu++)
		{
			device_indices[gpu] = gpu;
			transfer_semaphores[gpu] = qvk.semaphores[qvk.current_frame_index][gpu].transfer_finished;
			trace_semaphores[gpu] = qvk.semaphores[qvk.current_frame_index][gpu].trace_finished;
			prev_trace_semaphores[gpu] = qvk.semaphores[(qvk.current_frame_index - 1) % MAX_FRAMES_IN_FLIGHT][gpu].trace_finished;
			wait_stages[gpu] = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
		}

		vkpt_submit_command_buffer(
			transfer_cmd_buf, 
			qvk.queue_transfer, 
			all_device_mask, 
			(*prev_trace_signaled) ? qvk.device_count : 0, prev_trace_semaphores, wait_stages, device_indices, 
			qvk.device_count, transfer_semaphores, device_indices, 
			VK_NULL_HANDLE);

		*prev_trace_signaled = qfalse;
	}

	{
		VkCommandBuffer trace_cmd_buf = vkpt_begin_command_buffer(&qvk.cmd_buffers_graphics);

		update_transparency(trace_cmd_buf, ubo->V, fd->particles, fd->num_particles, fd->entities, fd->num_entities);

		_VK(vkpt_uniform_buffer_update(trace_cmd_buf));

		// put a profiler query without a marker for the frame begin/end - because markers do not 
		// work well across different command lists
		_VK(vkpt_profiler_query(trace_cmd_buf, PROFILER_FRAME_TIME, PROFILER_START));

		BEGIN_PERF_MARKER(trace_cmd_buf, PROFILER_UPDATE_ENVIRONMENT);
		if (render_world)
		{
			vkpt_physical_sky_record_cmd_buffer(trace_cmd_buf);
		}
		END_PERF_MARKER(trace_cmd_buf, PROFILER_UPDATE_ENVIRONMENT);

		BEGIN_PERF_MARKER(trace_cmd_buf, PROFILER_INSTANCE_GEOMETRY);
		vkpt_instance_geometry(trace_cmd_buf, upload_info.num_instances, update_world_animations);
		END_PERF_MARKER(trace_cmd_buf, PROFILER_INSTANCE_GEOMETRY);

		BEGIN_PERF_MARKER(trace_cmd_buf, PROFILER_BVH_UPDATE);
		assert(upload_info.num_vertices % 3 == 0);
		vkpt_pt_create_all_dynamic(trace_cmd_buf, qvk.current_frame_index, &upload_info);
		vkpt_pt_create_toplevel(trace_cmd_buf, qvk.current_frame_index, render_world, upload_info.weapon_left_handed);
		vkpt_pt_update_descripter_set_bindings(qvk.current_frame_index);
		END_PERF_MARKER(trace_cmd_buf, PROFILER_BVH_UPDATE);

		BEGIN_PERF_MARKER(trace_cmd_buf, PROFILER_SHADOW_MAP);
		if (god_rays_enabled)
		{
			vkpt_shadow_map_render(trace_cmd_buf, shadowmap_view_proj,
				vkpt_refdef.bsp_mesh_world.world_idx_count,
				upload_info.dynamic_vertex_num,
				vkpt_refdef.bsp_mesh_world.world_transparent_offset,
				vkpt_refdef.bsp_mesh_world.world_transparent_count);
		}
		END_PERF_MARKER(trace_cmd_buf, PROFILER_SHADOW_MAP);

		vkpt_pt_trace_primary_rays(trace_cmd_buf);

		vkpt_submit_command_buffer(
			trace_cmd_buf,
			qvk.queue_graphics,
			all_device_mask,
			qvk.device_count, transfer_semaphores, wait_stages, device_indices,
			0, 0, 0,
			VK_NULL_HANDLE);
	}

	{
		VkCommandBuffer trace_cmd_buf = vkpt_begin_command_buffer(&qvk.cmd_buffers_graphics);

		if (god_rays_enabled)
		{
			BEGIN_PERF_MARKER(trace_cmd_buf, PROFILER_GOD_RAYS);
			vkpt_record_god_rays_trace_command_buffer(trace_cmd_buf, 0);
			END_PERF_MARKER(trace_cmd_buf, PROFILER_GOD_RAYS);
		}

		if (ref_mode.reflect_refract > 0)
		{
			BEGIN_PERF_MARKER(trace_cmd_buf, PROFILER_REFLECT_REFRACT_1);
			vkpt_pt_trace_reflections(trace_cmd_buf, 0);
			END_PERF_MARKER(trace_cmd_buf, PROFILER_REFLECT_REFRACT_1);
		}

		if (god_rays_enabled)
		{
			if (ref_mode.reflect_refract > 0)
			{
				BEGIN_PERF_MARKER(trace_cmd_buf, PROFILER_GOD_RAYS_REFLECT_REFRACT);
				vkpt_record_god_rays_trace_command_buffer(trace_cmd_buf, 1);
				END_PERF_MARKER(trace_cmd_buf, PROFILER_GOD_RAYS_REFLECT_REFRACT);
			}

			BEGIN_PERF_MARKER(trace_cmd_buf, PROFILER_GOD_RAYS_FILTER);
			vkpt_record_god_rays_filter_command_buffer(trace_cmd_buf);
			END_PERF_MARKER(trace_cmd_buf, PROFILER_GOD_RAYS_FILTER);
		}

		if (ref_mode.reflect_refract > 1)
		{
			BEGIN_PERF_MARKER(trace_cmd_buf, PROFILER_REFLECT_REFRACT_2);
			for (int pass = 0; pass < ref_mode.reflect_refract - 1; pass++)
			{
				vkpt_pt_trace_reflections(trace_cmd_buf, pass + 1);
			}
			END_PERF_MARKER(trace_cmd_buf, PROFILER_REFLECT_REFRACT_2);
		}

		if (ref_mode.enable_denoiser)
		{
			BEGIN_PERF_MARKER(trace_cmd_buf, PROFILER_ASVGF_GRADIENT_REPROJECT);
			vkpt_asvgf_gradient_reproject(trace_cmd_buf);
			END_PERF_MARKER(trace_cmd_buf, PROFILER_ASVGF_GRADIENT_REPROJECT);
		}

		vkpt_pt_trace_lighting(trace_cmd_buf, ref_mode.num_bounce_rays);
		
		vkpt_submit_command_buffer(
			trace_cmd_buf,
			qvk.queue_graphics,
			all_device_mask,
			0, 0, 0, 0,
			qvk.device_count, trace_semaphores, device_indices,
			VK_NULL_HANDLE);

		*curr_trace_signaled = qtrue;
	}

	{
		VkCommandBuffer post_cmd_buf = vkpt_begin_command_buffer(&qvk.cmd_buffers_graphics);

		BEGIN_PERF_MARKER(post_cmd_buf, PROFILER_ASVGF_FULL);
		if (ref_mode.enable_denoiser)
		{
			vkpt_asvgf_filter(post_cmd_buf, cvar_pt_num_bounce_rays->value >= 0.5f);
		}
		else
		{
			vkpt_compositing(post_cmd_buf);
		}
		END_PERF_MARKER(post_cmd_buf, PROFILER_ASVGF_FULL);

		vkpt_interleave(post_cmd_buf);

		vkpt_taa(post_cmd_buf);

		BEGIN_PERF_MARKER(post_cmd_buf, PROFILER_BLOOM);
		if (cvar_bloom_enable->integer != 0 || menu_mode)
		{
			vkpt_bloom_record_cmd_buffer(post_cmd_buf);
		}
		END_PERF_MARKER(post_cmd_buf, PROFILER_BLOOM);

#ifdef VKPT_IMAGE_DUMPS
		if (cvar_dump_image->integer)
		{
			copy_to_dump_texture(post_cmd_buf, VKPT_IMG_TAA_OUTPUT);
		}
#endif

		BEGIN_PERF_MARKER(post_cmd_buf, PROFILER_TONE_MAPPING);
		if (cvar_tm_enable->integer != 0)
		{
			vkpt_tone_mapping_record_cmd_buffer(post_cmd_buf, frame_time <= 0.f ? frame_wallclock_time : frame_time);
		}
		END_PERF_MARKER(post_cmd_buf, PROFILER_TONE_MAPPING);

		{
			VkBufferCopy copyRegion = { 0, 0, sizeof(ReadbackBuffer) };
			vkCmdCopyBuffer(post_cmd_buf, qvk.buf_readback.buffer, qvk.buf_readback_staging[qvk.current_frame_index].buffer, 1, &copyRegion);
		}

		_VK(vkpt_profiler_query(post_cmd_buf, PROFILER_FRAME_TIME, PROFILER_STOP));

		vkpt_submit_command_buffer_simple(post_cmd_buf, qvk.queue_graphics, qtrue);
	}

	temporal_frame_valid = ref_mode.enable_denoiser;
	
	frame_ready = qtrue;

	if (vkpt_refdef.fd && vkpt_refdef.fd->lightstyles) {
		memcpy(vkpt_refdef.prev_lightstyles, vkpt_refdef.fd->lightstyles, sizeof(vkpt_refdef.prev_lightstyles));
	}
}

static void temporal_cvar_changed(cvar_t *self)
{
	temporal_frame_valid = qfalse;
}

static void
recreate_swapchain()
{
	vkDeviceWaitIdle(qvk.device);
	vkpt_destroy_all(VKPT_INIT_SWAPCHAIN_RECREATE);
	destroy_swapchain();
	SDL_GetWindowSize(qvk.window, &qvk.win_width, &qvk.win_height);
	create_swapchain();
	vkpt_initialize_all(VKPT_INIT_SWAPCHAIN_RECREATE);

	qvk.wait_for_idle_frames = MAX_FRAMES_IN_FLIGHT * 2;
}

static int compare_doubles(const void* pa, const void* pb)
{
	double a = *(double*)pa;
	double b = *(double*)pb;

	if (a < b) return -1; 
	if (a > b) return 1;
	return 0;
}

// DRS (Dynamic Resolution Scaling) functions

static void drs_init()
{
	cvar_drs_enable = Cvar_Get("drs_enable", "0", CVAR_ARCHIVE);
	// Target FPS value
	cvar_drs_target = Cvar_Get("drs_target", "60", CVAR_ARCHIVE);
	cvar_drs_target->changed = drs_target_changed;
	// Minimum resolution scale in percents
	cvar_drs_minscale = Cvar_Get("drs_minscale", "50", 0);
	cvar_drs_minscale->changed = drs_minscale_changed;
	// Maximum resolution scale in percents
	cvar_drs_maxscale = Cvar_Get("drs_maxscale", "100", 0);
	cvar_drs_maxscale->changed = drs_maxscale_changed;
	// Resolution regulator parameters, see the `dynamic_resolution_scaling()` function
	cvar_drs_gain = Cvar_Get("drs_gain", "20", 0);
	cvar_drs_adjust_up = Cvar_Get("drs_adjust_up", "0.92", 0);
	cvar_drs_adjust_down = Cvar_Get("drs_adjust_down", "0.98", 0);
}

static void drs_process()
{
#define SCALING_FRAMES 5
	static int num_valid_frames = 0;
	static double valid_frame_times[SCALING_FRAMES];

	if (cvar_drs_enable->integer == 0)
	{
		num_valid_frames = 0;

		if (is_accumulation_rendering_active())
			drs_effective_scale = max(100, scr_viewsize->integer);
		else
			drs_effective_scale = 0;

		return;
	}

	if (is_accumulation_rendering_active())
	{
		num_valid_frames = 0;
		drs_effective_scale = max(cvar_drs_minscale->integer, cvar_drs_maxscale->integer);
		return;
	}

	drs_effective_scale = drs_current_scale;

	double ms = vkpt_get_profiler_result(PROFILER_FRAME_TIME);

	if (ms < 0 || ms > 1000)
		return;

	valid_frame_times[num_valid_frames] = ms;
	num_valid_frames++;

	if (num_valid_frames < SCALING_FRAMES)
		return;

	num_valid_frames = 0;

	qsort(valid_frame_times, SCALING_FRAMES, sizeof(double), compare_doubles);

	double representative_time = 0;
	for(int i = 1; i < SCALING_FRAMES - 1; i++)
		representative_time += valid_frame_times[i];
	representative_time /= (SCALING_FRAMES - 2);

	double target_time = 1000.0 / cvar_drs_target->value;
	double f = cvar_drs_gain->value * (1.0 - representative_time / target_time) - 1.0;

	int scale = drs_current_scale;
	if (representative_time < target_time * cvar_drs_adjust_up->value)
	{
		f += 0.5;
		clamp(f, 1, 10);
		scale += (int)f;
	}
	else if (representative_time > target_time * cvar_drs_adjust_down->value)
	{
		f -= 0.5;
		clamp(f, -1, -10);
		scale += f;
	}

	drs_current_scale = max(cvar_drs_minscale->integer, min(cvar_drs_maxscale->integer, scale));
	drs_effective_scale = drs_current_scale;
}

void
R_BeginFrame_RTX(void)
{
	LOG_FUNC();

	qvk.current_frame_index = qvk.frame_counter % MAX_FRAMES_IN_FLIGHT;

	VkResult res_fence = vkWaitForFences(qvk.device, 1, qvk.fences_frame_sync + qvk.current_frame_index, VK_TRUE, ~((uint64_t) 0));
	
	if (res_fence == VK_ERROR_DEVICE_LOST)
	{
		// TODO implement a good error box or vid_restart or something
		Com_EPrintf("Device lost!\n");
		exit(1);
	}

	if (!qvk.swap_chain)
	{
		VkSurfaceCapabilitiesKHR surf_capabilities;
		vkGetPhysicalDeviceSurfaceCapabilitiesKHR(qvk.physical_device, qvk.surface, &surf_capabilities);

		// see if we're un-minimized again
		if (surf_capabilities.currentExtent.width != 0 && surf_capabilities.currentExtent.height != 0)
		{
			recreate_swapchain();
		}
	}

	drs_process();
	if (vkpt_refdef.fd)
	{
		vkpt_refdef.fd->feedback.resolution_scale = (drs_effective_scale != 0) ? drs_effective_scale : scr_viewsize->integer;
	}

	qvk.extent_render = get_render_extent();
	qvk.gpu_slice_width = (qvk.extent_render.width + qvk.device_count - 1) / qvk.device_count;
	
	VkExtent2D extent_screen_images = get_screen_image_extent();

	if(!extents_equal(extent_screen_images, qvk.extent_screen_images))
	{
		qvk.extent_screen_images = extent_screen_images;
		recreate_swapchain();
	}

retry:;

	if (!qvk.swap_chain) // we're minimized, don't render
		return;

#ifdef VKPT_DEVICE_GROUPS
	VkAcquireNextImageInfoKHR acquire_info = {
		.sType = VK_STRUCTURE_TYPE_ACQUIRE_NEXT_IMAGE_INFO_KHR,
		.swapchain = qvk.swap_chain,
		.timeout = (~((uint64_t) 0)),
		.semaphore = qvk.semaphores[qvk.current_frame_index][0].image_available,
		.fence = VK_NULL_HANDLE,
		.deviceMask = (1 << qvk.device_count) - 1,
	};

	VkResult res_swapchain = vkAcquireNextImage2KHR(qvk.device, &acquire_info, &qvk.current_swap_chain_image_index);
#else
	VkResult res_swapchain = vkAcquireNextImageKHR(qvk.device, qvk.swap_chain, ~((uint64_t) 0),
		qvk.semaphores[qvk.current_frame_index][0].image_available, VK_NULL_HANDLE, &qvk.current_swap_chain_image_index);
#endif
	if(res_swapchain == VK_ERROR_OUT_OF_DATE_KHR || res_swapchain == VK_SUBOPTIMAL_KHR) {
		recreate_swapchain();
		goto retry;
	}
	else if(res_swapchain != VK_SUCCESS) {
		Com_EPrintf("Error %d in vkAcquireNextImageKHR\n", res_swapchain);
	}

	if (qvk.wait_for_idle_frames) {
		vkDeviceWaitIdle(qvk.device);
		qvk.wait_for_idle_frames--;
	}

	vkResetFences(qvk.device, 1, qvk.fences_frame_sync + qvk.current_frame_index);

	vkpt_reset_command_buffers(&qvk.cmd_buffers_graphics);
	vkpt_reset_command_buffers(&qvk.cmd_buffers_transfer);

	// Process the profiler queries - always enabled to support DRS
	{
		VkCommandBuffer reset_cmd_buf = vkpt_begin_command_buffer(&qvk.cmd_buffers_graphics);

		_VK(vkpt_profiler_next_frame(reset_cmd_buf));

		vkpt_submit_command_buffer_simple(reset_cmd_buf, qvk.queue_graphics, qtrue);
	}

	vkpt_textures_destroy_unused();
	vkpt_textures_end_registration();
	vkpt_textures_update_descriptor_set();

	vkpt_vertex_buffer_upload_models();
	vkpt_draw_clear_stretch_pics();

	SCR_SetHudAlpha(1.f);
}

void
R_EndFrame_RTX(void)
{
	LOG_FUNC();

	if (!qvk.swap_chain)
	{
		vkpt_draw_clear_stretch_pics();
		return;
	}

	if(cvar_profiler->integer)
		draw_profiler(cvar_flt_enable->integer != 0);

	VkCommandBuffer cmd_buf = vkpt_begin_command_buffer(&qvk.cmd_buffers_graphics);

	if (frame_ready)
	{
		if (qvk.effective_aa_mode == AA_MODE_UPSCALE)
		{
			vkpt_final_blit_simple(cmd_buf);
		}
		else
		{
			VkExtent2D extent_unscaled_half;
			extent_unscaled_half.width = qvk.extent_unscaled.width / 2;
			extent_unscaled_half.height = qvk.extent_unscaled.height / 2;

			if (extents_equal(qvk.extent_render, qvk.extent_unscaled) ||
				extents_equal(qvk.extent_render, extent_unscaled_half) && drs_effective_scale == 0) // don't do nearest filter 2x upscale with DRS enabled
				vkpt_final_blit_simple(cmd_buf);
			else
				vkpt_final_blit_filtered(cmd_buf);
		}

		frame_ready = qfalse;
	}

	vkpt_draw_submit_stretch_pics(cmd_buf);

	VkSemaphore wait_semaphores[] = { qvk.semaphores[qvk.current_frame_index][0].image_available };
	VkPipelineStageFlags wait_stages[] = { VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT };
	uint32_t wait_device_indices[] = { 0 };

	VkSemaphore signal_semaphores[VKPT_MAX_GPUS];
	uint32_t signal_device_indices[VKPT_MAX_GPUS];
	for (int gpu = 0; gpu < qvk.device_count; gpu++)
	{
		signal_semaphores[gpu] = qvk.semaphores[qvk.current_frame_index][gpu].render_finished;
		signal_device_indices[gpu] = gpu;
	}

	vkpt_submit_command_buffer(
		cmd_buf,
		qvk.queue_graphics,
		(1 << qvk.device_count) - 1,
		LENGTH(wait_semaphores), wait_semaphores, wait_stages, wait_device_indices,
		qvk.device_count, signal_semaphores, signal_device_indices,
		qvk.fences_frame_sync[qvk.current_frame_index]);


#ifdef VKPT_IMAGE_DUMPS
	if (cvar_dump_image->integer) {
		_VK(vkQueueWaitIdle(qvk.queue_graphics));

		VkImageSubresource subresource = {
			.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
			.arrayLayer = 0,
			.mipLevel = 0
		};

		VkSubresourceLayout subresource_layout;
		vkGetImageSubresourceLayout(qvk.device, qvk.dump_image, &subresource, &subresource_layout);

		void *data;
		_VK(vkMapMemory(qvk.device, qvk.dump_image_memory, 0, qvk.dump_image_memory_size, 0, &data));
		save_to_pfm_file("color_buffer", qvk.frame_counter, IMG_WIDTH, IMG_HEIGHT, (char *)data, subresource_layout.rowPitch, 0);
		vkUnmapMemory(qvk.device, qvk.dump_image_memory);

		Cvar_SetInteger(cvar_dump_image, 0, FROM_CODE);
	}
#endif

	VkPresentInfoKHR present_info = {
		.sType              = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
		.waitSemaphoreCount = qvk.device_count,
		.pWaitSemaphores    = signal_semaphores,
		.swapchainCount     = 1,
		.pSwapchains        = &qvk.swap_chain,
		.pImageIndices      = &qvk.current_swap_chain_image_index,
		.pResults           = NULL,
	};

#ifdef VKPT_DEVICE_GROUPS
	uint32_t present_device_mask = 1;
	VkDeviceGroupPresentInfoKHR group_present_info = {
		.sType				= VK_STRUCTURE_TYPE_DEVICE_GROUP_PRESENT_INFO_KHR,
		.swapchainCount		= 1,
		.pDeviceMasks		= &present_device_mask,
		.mode				= VK_DEVICE_GROUP_PRESENT_MODE_LOCAL_BIT_KHR,
	};

	if (qvk.device_count > 1) {
		present_info.pNext = &group_present_info;
	}
#endif

	VkResult res_present = vkQueuePresentKHR(qvk.queue_graphics, &present_info);
	if(res_present == VK_ERROR_OUT_OF_DATE_KHR || res_present == VK_SUBOPTIMAL_KHR) {
		recreate_swapchain();
	}
	qvk.frame_counter++;
}

void
R_ModeChanged_RTX(int width, int height, int flags, int rowbytes, void *pixels)
{
	Com_DPrintf("mode changed %d %d\n", width, height);

	r_config.width  = width;
	r_config.height = height;
	r_config.flags  = flags;

	qvk.wait_for_idle_frames = MAX_FRAMES_IN_FLIGHT * 2;
}

static void
vkpt_show_pvs(void)
{
	if (!vkpt_refdef.fd)
		return;

	if (vkpt_refdef.fd->feedback.lookatcluster < 0)
	{
		memset(cluster_debug_mask, 0, sizeof(cluster_debug_mask));
		cluster_debug_index = -1;
		return;
	}

	BSP_ClusterVis(bsp_world_model, cluster_debug_mask, vkpt_refdef.fd->feedback.lookatcluster, DVIS_PVS);
	cluster_debug_index = vkpt_refdef.fd->feedback.lookatcluster;
}

static float halton(int base, int index) {
	float f = 1.f;
	float r = 0.f;
	int i = index;

	while (i > 0)
	{
		f = f / base;
		r = r + f * (i % base);
		i = i / base;
	}
	return r;
};

// Autocompletion support for ray_tracing_api cvar
static void ray_tracing_api_g(genctx_t *ctx)
{
	Prompt_AddMatch(ctx, "auto");
	Prompt_AddMatch(ctx, "query");
	Prompt_AddMatch(ctx, "pipeline");
}

/* called when the library is loaded */
qboolean
R_Init_RTX(qboolean total)
{
	registration_sequence = 1;

	if (!VID_Init(GAPI_VULKAN)) {
		Com_Error(ERR_FATAL, "VID_Init failed\n");
		return qfalse;
	}

	extern SDL_Window *sdl_window;
	qvk.window = sdl_window;

	cvar_profiler = Cvar_Get("profiler", "0", 0);
	cvar_vsync = Cvar_Get("vid_vsync", "0", CVAR_REFRESH | CVAR_ARCHIVE);
	cvar_vsync->changed = NULL; // in case the GL renderer has set it
	cvar_pt_caustics = Cvar_Get("pt_caustics", "1", CVAR_ARCHIVE);
	cvar_pt_enable_nodraw = Cvar_Get("pt_enable_nodraw", "0", 0);
	/* Synthesize materials for surfaces with LIGHT flag.
	 * 0: disabled
	 * 1: enabled for "custom" materials (not in materials.csv)
	 * 2: enabled for all materials w/o an emissive texture */
	cvar_pt_enable_surface_lights = Cvar_Get("pt_enable_surface_lights", "1", CVAR_FILES);
	/* LIGHT flag synthesis for "warp" surfaces (water, slime),
	 * separately controlled for aesthetic reasons
	 * 0: disabled
	 * 1: hack up a material that emits light but doesn't render with an emissive texture
	 * 2: "full" synthesis (incl emissive texture) */
	cvar_pt_enable_surface_lights_warp = Cvar_Get("pt_enable_surface_lights_warp", "0", CVAR_FILES);
	/* How to choose emissive texture for LIGHT flag synthesis:
	 * 0: Just use diffuse texture
	 * 1: Use (diffuse) pixels above a certain relative brightness for emissive texture */
	cvar_pt_surface_lights_fake_emissive_algo = Cvar_Get("pt_surface_lights_fake_emissive_algo", "1", CVAR_FILES);

	// Threshold for pixel values used when constructing a fake emissive image.
	cvar_pt_surface_lights_threshold = Cvar_Get("pt_surface_lights_threshold", "215", CVAR_FILES);

	// Multiplier for texinfo radiance field to convert radiance to emissive factors
	cvar_pt_bsp_radiance_scale = Cvar_Get("pt_bsp_radiance_scale", "0.001", CVAR_FILES);

	// 0 -> disabled, regular pause; 1 -> enabled; 2 -> enabled, hide GUI
	cvar_pt_accumulation_rendering = Cvar_Get("pt_accumulation_rendering", "1", CVAR_ARCHIVE);

	// number of frames to accumulate with linear weights in accumulation rendering modes
	cvar_pt_accumulation_rendering_framenum = Cvar_Get("pt_accumulation_rendering_framenum", "500", 0);

	// 0 -> perspective, 1 -> cylindrical
	cvar_pt_projection = Cvar_Get("pt_projection", "0", CVAR_ARCHIVE);

	// depth of field control:
	// 0 -> disabled
	// 1 -> enabled only in the reference mode
	// 2 -> enabled in the reference and no-denoiser modes
	// 3 -> always enabled (where are my glasses?)
	cvar_pt_dof = Cvar_Get("pt_dof", "1", CVAR_ARCHIVE);

	// freecam mode toggle
	cvar_pt_freecam = Cvar_Get("pt_freecam", "1", CVAR_ARCHIVE);

	// texture filtering mode:
	// 0 -> linear magnification, anisotropic minification
	// 1 -> nearest magnification, anisotropic minification
	// 2 -> nearest magnification and minification, no mipmaps (noisy)
	cvar_pt_nearest = Cvar_Get("pt_nearest", "0", CVAR_ARCHIVE);
	cvar_pt_nearest->changed = pt_nearest_changed;

#ifdef VKPT_DEVICE_GROUPS
	cvar_sli = Cvar_Get("sli", "1", CVAR_REFRESH | CVAR_ARCHIVE);
#endif

#ifdef VKPT_IMAGE_DUMPS
	cvar_dump_image = Cvar_Get("dump_image", "0", 0);
#endif

	scr_viewsize = Cvar_Get("viewsize", "100", CVAR_ARCHIVE);
	scr_viewsize->changed = viewsize_changed;

	// enables or disables full screen blending effects
	cvar_tm_blend_enable = Cvar_Get("tm_blend_enable", "1", CVAR_ARCHIVE);

	drs_init();
	
	// Minimum NVIDIA driver version - this is a cvar in case something changes in the future,
	// and the current test no longer works.
	cvar_min_driver_version_nvidia = Cvar_Get("min_driver_version_nvidia", "460.82", 0);

	// Minimum AMD driver version
	cvar_min_driver_version_amd = Cvar_Get("min_driver_version_amd", "21.1.1", 0);

	// Selects which RT API to use:
	//  auto     - automatic selection based on the GPU
	//  query    - prefer KHR_ray_query
	//  pipeline - prefer KHR_ray_tracing_pipeline
	cvar_ray_tracing_api = Cvar_Get("ray_tracing_api", "auto", CVAR_REFRESH | CVAR_ARCHIVE);
	cvar_ray_tracing_api->generator = &ray_tracing_api_g;

	// When nonzero, the Vulkan validation layer is requested
	cvar_vk_validation = Cvar_Get("vk_validation", "0", CVAR_REFRESH | CVAR_ARCHIVE);

	InitialiseSkyCVars();

	MAT_Init();

#define UBO_CVAR_DO(name, default_value) cvar_##name = Cvar_Get(#name, #default_value, 0);
	UBO_CVAR_LIST
#undef UBO_CVAR_LIST

	cvar_flt_temporal_hf->changed = temporal_cvar_changed;
	cvar_flt_temporal_lf->changed = temporal_cvar_changed;
	cvar_flt_temporal_spec->changed = temporal_cvar_changed;
	cvar_flt_enable->changed = temporal_cvar_changed;

	cvar_pt_dof->changed = accumulation_cvar_changed;
	cvar_pt_aperture->changed = accumulation_cvar_changed;
	cvar_pt_aperture_type->changed = accumulation_cvar_changed;
	cvar_pt_aperture_angle->changed = accumulation_cvar_changed;
	cvar_pt_focus->changed = accumulation_cvar_changed;
	cvar_pt_freecam->changed = accumulation_cvar_changed;
	cvar_pt_projection->changed = accumulation_cvar_changed;

	cvar_pt_num_bounce_rays->flags |= CVAR_ARCHIVE;

	qvk.win_width  = r_config.width;
	qvk.win_height = r_config.height;

	IMG_Init();
	IMG_GetPalette();
	MOD_Init();
	
	if(!init_vulkan()) {
		Com_Error(ERR_FATAL, "Couldn't initialize Vulkan.\n");
		return qfalse;
	}

	_VK(create_command_pool_and_fences());
	_VK(create_swapchain());

	vkpt_load_shader_modules();

	_VK(vkpt_initialize_all(VKPT_INIT_DEFAULT));
	_VK(vkpt_initialize_all(VKPT_INIT_RELOAD_SHADER));
	_VK(vkpt_initialize_all(VKPT_INIT_SWAPCHAIN_RECREATE));

	Cmd_AddCommand("reload_shader", (xcommand_t)&vkpt_reload_shader);
	Cmd_AddCommand("reload_textures", (xcommand_t)&vkpt_reload_textures);
	Cmd_AddCommand("show_pvs", (xcommand_t)&vkpt_show_pvs);
	Cmd_AddCommand("next_sun", (xcommand_t)&vkpt_next_sun_preset);
#if CL_RTX_SHADERBALLS
	Cmd_AddCommand("drop_balls", (xcommand_t)&vkpt_drop_shaderballs);
#endif

	for (int i = 0; i < 256; i++) {
		qvk.sintab[i] = sinf(i * (2 * M_PI / 255));
	}

	for (int i = 0; i < NUM_TAA_SAMPLES; i++)
	{
		taa_samples[i][0] = halton(2, i + 1) - 0.5f;
		taa_samples[i][1] = halton(3, i + 1) - 0.5f;
	}

	return qtrue;
}

/* called before the library is unloaded */
void
R_Shutdown_RTX(qboolean total)
{
	vkpt_freecam_reset();

	vkDeviceWaitIdle(qvk.device);
	
	Cmd_RemoveCommand("reload_shader");
	Cmd_RemoveCommand("reload_textures");
	Cmd_RemoveCommand("show_pvs");
	Cmd_RemoveCommand("next_sun");
#if CL_RTX_SHADERBALLS
	Cmd_RemoveCommand("drop_balls");
#endif

	MAT_Shutdown();
	IMG_FreeAll();
	vkpt_textures_destroy_unused();

	_VK(vkpt_destroy_all(VKPT_INIT_DEFAULT));
	vkpt_destroy_shader_modules();

	if(destroy_vulkan()) {
		Com_EPrintf("destroy_vulkan failed\n");
	}

	IMG_Shutdown();
	MOD_Shutdown(); // todo: currently leaks memory, need to clear submeshes
	VID_Shutdown();
}

// for screenshots
byte *
IMG_ReadPixels_RTX(int *width, int *height, int *rowbytes)
{
	if (qvk.surf_format.format != VK_FORMAT_B8G8R8A8_SRGB &&
		qvk.surf_format.format != VK_FORMAT_R8G8B8A8_SRGB)
	{
		Com_EPrintf("IMG_ReadPixels: unsupported swap chain format (%d)!\n", qvk.surf_format.format);
		return NULL;
	}

	VkCommandBuffer cmd_buf = vkpt_begin_command_buffer(&qvk.cmd_buffers_graphics);

	VkImage swap_chain_image = qvk.swap_chain_images[qvk.current_swap_chain_image_index];

	VkImageSubresourceRange subresource_range = {
		.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
		.baseMipLevel = 0,
		.levelCount = 1,
		.baseArrayLayer = 0,
		.layerCount = 1
	};
		
	IMAGE_BARRIER(cmd_buf,
		.image = swap_chain_image,
		.subresourceRange = subresource_range,
		.srcAccessMask = 0,
		.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
		.oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
		.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL
	);

	IMAGE_BARRIER(cmd_buf,
		.image = qvk.screenshot_image,
		.subresourceRange = subresource_range,
		.srcAccessMask = VK_ACCESS_HOST_READ_BIT,
		.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
		.oldLayout = VK_IMAGE_LAYOUT_GENERAL,
		.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL
	);

	VkImageCopy img_copy_region = {
		.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
		.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
		.extent = { qvk.extent_unscaled.width, qvk.extent_unscaled.height, 1 }
	};

	vkCmdCopyImage(cmd_buf,
		swap_chain_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
		qvk.screenshot_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		1, &img_copy_region);

	IMAGE_BARRIER(cmd_buf,
		.image = swap_chain_image,
		.subresourceRange = subresource_range,
		.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
		.dstAccessMask = 0,
		.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
		.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR
	);

	IMAGE_BARRIER(cmd_buf,
		.image = qvk.screenshot_image,
		.subresourceRange = subresource_range,
		.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
		.dstAccessMask = VK_ACCESS_HOST_READ_BIT,
		.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		.newLayout = VK_IMAGE_LAYOUT_GENERAL
	);

	vkpt_submit_command_buffer_simple(cmd_buf, qvk.queue_graphics, qfalse);
	vkpt_wait_idle(qvk.queue_graphics, &qvk.cmd_buffers_graphics);

	VkImageSubresource subresource = {
		.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
		.arrayLayer = 0,
		.mipLevel = 0
	};

	VkSubresourceLayout subresource_layout;
	vkGetImageSubresourceLayout(qvk.device, qvk.screenshot_image, &subresource, &subresource_layout);

	void *device_data;
	_VK(vkMapMemory(qvk.device, qvk.screenshot_image_memory, 0, qvk.screenshot_image_memory_size, 0, &device_data));
	
	int pitch = qvk.extent_unscaled.width * 3;
	byte *pixels = FS_AllocTempMem(pitch * qvk.extent_unscaled.height);

	for (int row = 0; row < qvk.extent_unscaled.height; row++)
	{
		byte* src_row = (byte*)device_data + subresource_layout.rowPitch * row;
		byte* dst_row = pixels + pitch * (qvk.extent_unscaled.height - row - 1);

		if (qvk.surf_format.format == VK_FORMAT_B8G8R8A8_SRGB)
		{
			for (int col = 0; col < qvk.extent_unscaled.width; col++)
			{
				dst_row[0] = src_row[2];
				dst_row[1] = src_row[1];
				dst_row[2] = src_row[0];

				src_row += 4;
				dst_row += 3;
			}
		}
		else // must be VK_FORMAT_R8G8B8A8_SRGB then
		{
			for (int col = 0; col < qvk.extent_unscaled.width; col++)
			{
				dst_row[0] = src_row[0];
				dst_row[1] = src_row[1];
				dst_row[2] = src_row[2];

				src_row += 4;
				dst_row += 3;
			}
		}
	}

	vkUnmapMemory(qvk.device, qvk.screenshot_image_memory);

	*width = qvk.extent_unscaled.width;
	*height = qvk.extent_unscaled.height;
	*rowbytes = pitch;
	return pixels;
}

void
R_SetSky_RTX(const char *name, float rotate, vec3_t axis)
{
	int     i;
	char    pathname[MAX_QPATH];
	// 3dstudio environment map names
	const char *suf[6] = { "ft", "bk", "up", "dn", "rt", "lf" };

	byte *data = NULL;

	sky_rotation = rotate;
	VectorNormalize2(axis, sky_axis);

	int avg_color[3] = { 0 };
	int w_prev, h_prev;
	for (i = 0; i < 6; i++) {
		Q_concat(pathname, sizeof(pathname), "env/", name, suf[i], ".tga", NULL);
		FS_NormalizePath(pathname, pathname);
		image_t *img = IMG_Find(pathname, IT_SKY, IF_NONE);

		if(img == R_NOTEXTURE) {
			if(data) {
				Z_Free(data);
			}
			data = Z_Malloc(6 * sizeof(uint32_t));
			for(int j = 0; j < 6; j++)
				((uint32_t *)data)[j] = 0xff00ffffu;
			w_prev = h_prev = 1;
			break;
		}

		size_t s = img->upload_width * img->upload_height * 4;
		if(!data) {
			data = Z_Malloc(s * 6);
			w_prev = img->upload_width;
			h_prev = img->upload_height;
		}

		memcpy(data + s * i, img->pix_data, s);

		for (int p = 0; p < img->upload_width * img->upload_height; p++)
		{
			uint32_t pix = *((uint32_t*)img->pix_data + p);
			avg_color[0] += pix & 0xff;
			avg_color[1] += (pix >> 8) & 0xff;
			avg_color[2] += (pix >> 16) & 0xff;
		}

		assert(w_prev == img->upload_width);
		assert(h_prev == img->upload_height);

		List_Remove(&img->entry);

		IMG_Unload(img);

		memset(img, 0, sizeof(*img));
	}

	float inv_num_pixels = 1.0f / (w_prev * h_prev * 6);

	VectorSet(avg_envmap_color,
		(float)avg_color[0] * inv_num_pixels / 255.f,
		(float)avg_color[1] * inv_num_pixels / 255.f,
		(float)avg_color[2] * inv_num_pixels / 255.f
	);

	vkpt_textures_upload_envmap(w_prev, h_prev, data);
	Z_Free(data);
}

void R_AddDecal_RTX(decal_t *d)
{ }

void
R_BeginRegistration_RTX(const char *name)
{
	registration_sequence++;
	LOG_FUNC();
	Com_Printf("loading %s\n", name);
	vkDeviceWaitIdle(qvk.device);

	Com_AddConfigFile("maps/default.cfg", 0);
	Com_AddConfigFile(va("maps/%s.cfg", name), 0);

	if(vkpt_refdef.bsp_mesh_world_loaded) {
		bsp_mesh_destroy(&vkpt_refdef.bsp_mesh_world);
		vkpt_refdef.bsp_mesh_world_loaded = 0;
	}

	if(bsp_world_model) {
		BSP_Free(bsp_world_model);
		bsp_world_model = NULL;
	}

	char bsp_path[MAX_QPATH];
	Q_concat(bsp_path, sizeof(bsp_path), "maps/", name, ".bsp", NULL);
	bsp_t *bsp;
	qerror_t ret = BSP_Load(bsp_path, &bsp);
	if(!bsp) {
		Com_Error(ERR_DROP, "%s: couldn't load %s: %s", __func__, bsp_path, Q_ErrorString(ret));
	}
	bsp_world_model = bsp;
	bsp_mesh_register_textures(bsp);
	bsp_mesh_create_from_bsp(&vkpt_refdef.bsp_mesh_world, bsp, name);
	vkpt_light_stats_create(&vkpt_refdef.bsp_mesh_world);
	_VK(vkpt_vertex_buffer_upload_bsp_mesh_to_staging(&vkpt_refdef.bsp_mesh_world));
	_VK(vkpt_vertex_buffer_bsp_upload_staging());
	vkpt_refdef.bsp_mesh_world_loaded = 1;
	bsp = NULL;
	world_anim_frame = 0;

	Cvar_Set("sv_novis", vkpt_refdef.bsp_mesh_world.num_cameras > 0 ? "1" : "0");

	// register physical sky attributes based on map name lookup
	vkpt_physical_sky_beginRegistration();
	UpdatePhysicalSkyCVars();

	vkpt_physical_sky_latch_local_time();
	vkpt_bloom_reset();
	vkpt_tone_mapping_request_reset();
	vkpt_light_buffer_reset_counts();

	vkpt_pt_destroy_static();
	const bsp_mesh_t *m = &vkpt_refdef.bsp_mesh_world;
	_VK(vkpt_pt_create_static(
		m->world_idx_count, 
		m->world_transparent_count,
		m->world_masked_count,
		m->world_sky_count,
		m->world_custom_sky_count));

	memset(cluster_debug_mask, 0, sizeof(cluster_debug_mask));
	cluster_debug_index = -1;
}

void
R_EndRegistration_RTX(void)
{
	LOG_FUNC();
	
	vkpt_physical_sky_endRegistration();

	IMG_FreeUnused();
	MOD_FreeUnused();
	MAT_FreeUnused();
}

VkCommandBuffer vkpt_begin_command_buffer(cmd_buf_group_t* group)
{
	if (group->used_this_frame == group->count_per_frame)
	{
		uint32_t new_count = max(4, group->count_per_frame * 2);
		VkCommandBuffer* new_buffers = Z_Mallocz(new_count * MAX_FRAMES_IN_FLIGHT * sizeof(VkCommandBuffer));

		for (int frame = 0; frame < MAX_FRAMES_IN_FLIGHT; frame++)
		{
			if (group->count_per_frame > 0)
			{
				memcpy(new_buffers + new_count * frame, group->buffers + group->count_per_frame * frame, group->count_per_frame * sizeof(VkCommandBuffer));
			}

			VkCommandBufferAllocateInfo cmd_buf_alloc_info = {
				.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
				.commandPool = group->command_pool,
				.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
				.commandBufferCount = new_count - group->count_per_frame
			};

			_VK(vkAllocateCommandBuffers(qvk.device, &cmd_buf_alloc_info, new_buffers + new_count * frame + group->count_per_frame));
		}

#ifdef _DEBUG
		void** new_addrs = Z_Mallocz(new_count * MAX_FRAMES_IN_FLIGHT * sizeof(void*));

		if (group->count_per_frame > 0)
		{
			for (int frame = 0; frame < MAX_FRAMES_IN_FLIGHT; frame++)
			{
				memcpy(new_addrs + new_count * frame, group->buffer_begin_addrs + group->count_per_frame * frame, group->count_per_frame * sizeof(void*));
			}
		}

		Z_Free(group->buffer_begin_addrs);
		group->buffer_begin_addrs = new_addrs;
#endif

		Z_Free(group->buffers);
		group->buffers = new_buffers;
		group->count_per_frame = new_count;
	}

	VkCommandBuffer cmd_buf = group->buffers[group->count_per_frame * qvk.current_frame_index + group->used_this_frame];

	VkCommandBufferBeginInfo begin_info = {
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
		.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
		.pInheritanceInfo = NULL,
	};
	_VK(vkResetCommandBuffer(cmd_buf, 0));
	_VK(vkBeginCommandBuffer(cmd_buf, &begin_info));


#ifdef _DEBUG
	void** begin_addr = group->buffer_begin_addrs + group->count_per_frame * qvk.current_frame_index + group->used_this_frame;

#if (defined __GNUC__)
	*begin_addr = __builtin_return_address(0);
#elif (defined _MSC_VER)
	*begin_addr = _ReturnAddress();
#else
	*begin_addr = NULL;
#endif
#endif

	group->used_this_frame += 1;

	return cmd_buf;
}

void vkpt_free_command_buffers(cmd_buf_group_t* group)
{
	if (group->count_per_frame == 0)
		return;

	vkFreeCommandBuffers(qvk.device, group->command_pool, group->count_per_frame * MAX_FRAMES_IN_FLIGHT, group->buffers);

	Z_Free(group->buffers);
	group->buffers = NULL;

#ifdef _DEBUG
	Z_Free(group->buffer_begin_addrs);
	group->buffer_begin_addrs = NULL;
#endif

	group->count_per_frame = 0;
	group->used_this_frame = 0;
}

void vkpt_reset_command_buffers(cmd_buf_group_t* group)
{
	group->used_this_frame = 0;

#ifdef _DEBUG
	for (int i = 0; i < group->count_per_frame; i++)
	{
		void* addr = group->buffer_begin_addrs[group->count_per_frame * qvk.current_frame_index + i];
		//seth: this seems unrelated to the raytracing changes, but skip it until raytracing is working
		//assert(addr == 0);
	}
#endif
}

void vkpt_wait_idle(VkQueue queue, cmd_buf_group_t* group)
{
	vkQueueWaitIdle(queue);
	vkpt_reset_command_buffers(group);
}

void vkpt_submit_command_buffer(
	VkCommandBuffer cmd_buf,
	VkQueue queue,
	uint32_t execute_device_mask,
	int wait_semaphore_count,
	VkSemaphore* wait_semaphores,
	VkPipelineStageFlags* wait_stages,
	uint32_t* wait_device_indices,
	int signal_semaphore_count,
	VkSemaphore* signal_semaphores,
	uint32_t* signal_device_indices,
	VkFence fence)
{
	_VK(vkEndCommandBuffer(cmd_buf));

	VkSubmitInfo submit_info = {
		.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
		.waitSemaphoreCount = wait_semaphore_count,
		.pWaitSemaphores = wait_semaphores,
		.pWaitDstStageMask = wait_stages,
		.signalSemaphoreCount = signal_semaphore_count,
		.pSignalSemaphores = signal_semaphores,
		.commandBufferCount = 1,
		.pCommandBuffers = &cmd_buf,
	};

#ifdef VKPT_DEVICE_GROUPS
	VkDeviceGroupSubmitInfo device_group_submit_info = {
		.sType = VK_STRUCTURE_TYPE_DEVICE_GROUP_SUBMIT_INFO,
		.pNext = NULL,
		.waitSemaphoreCount = wait_semaphore_count,
		.pWaitSemaphoreDeviceIndices = wait_device_indices,
		.commandBufferCount = 1,
		.pCommandBufferDeviceMasks = &execute_device_mask,
		.signalSemaphoreCount = signal_semaphore_count,
		.pSignalSemaphoreDeviceIndices = signal_device_indices,
	};

	if (qvk.device_count > 1) {
		submit_info.pNext = &device_group_submit_info;
	}
#endif

	_VK(vkQueueSubmit(queue, 1, &submit_info, fence));

#ifdef _DEBUG
	cmd_buf_group_t* groups[] = { &qvk.cmd_buffers_graphics, &qvk.cmd_buffers_transfer };
	for (int ngroup = 0; ngroup < LENGTH(groups); ngroup++)
	{
		cmd_buf_group_t* group = groups[ngroup];
		for (int i = 0; i < MAX_FRAMES_IN_FLIGHT * group->count_per_frame; i++)
		{
			if (group->buffers[i] == cmd_buf)
			{
				group->buffer_begin_addrs[i] = NULL;
				return;
			}
		}
	}
#endif
}

void vkpt_submit_command_buffer_simple(
	VkCommandBuffer cmd_buf,
	VkQueue queue,
	qboolean all_gpus)
{
	vkpt_submit_command_buffer(cmd_buf, queue, all_gpus ? (1 << qvk.device_count) - 1 : 1, 0, NULL, NULL, NULL, 0, NULL, NULL, 0);
}

#if _WIN32
	#include <windows.h>
#else
	#include <stdio.h>
#endif

void debug_output(const char* format, ...)
{
	char buffer[2048];

	va_list args;
	va_start(args, format);
	vsnprintf(buffer, sizeof(buffer), format, args);
	va_end(args);

#if _WIN32
	OutputDebugStringA(buffer);
#else
	fprintf(stderr, "%s", buffer);
#endif
}

void R_RegisterFunctionsRTX()
{
	R_Init = R_Init_RTX;
	R_Shutdown = R_Shutdown_RTX;
	R_BeginRegistration = R_BeginRegistration_RTX;
	R_EndRegistration = R_EndRegistration_RTX;
	R_SetSky = R_SetSky_RTX;
	R_RenderFrame = R_RenderFrame_RTX;
	R_LightPoint = R_LightPoint_RTX;
	R_ClearColor = R_ClearColor_RTX;
	R_SetAlpha = R_SetAlpha_RTX;
	R_SetAlphaScale = R_SetAlphaScale_RTX;
	R_SetColor = R_SetColor_RTX;
	R_SetClipRect = R_SetClipRect_RTX;
	R_SetScale = R_SetScale_RTX;
	R_DrawChar = R_DrawChar_RTX;
	R_DrawString = R_DrawString_RTX;
	R_DrawPic = R_DrawPic_RTX;
	R_DrawStretchPic = R_DrawStretchPic_RTX;
	R_TileClear = R_TileClear_RTX;
	R_DrawFill8 = R_DrawFill8_RTX;
	R_DrawFill32 = R_DrawFill32_RTX;
	R_BeginFrame = R_BeginFrame_RTX;
	R_EndFrame = R_EndFrame_RTX;
	R_ModeChanged = R_ModeChanged_RTX;
	R_AddDecal = R_AddDecal_RTX;
	R_InterceptKey = R_InterceptKey_RTX;
	IMG_Load = IMG_Load_RTX;
	IMG_Unload = IMG_Unload_RTX;
	IMG_ReadPixels = IMG_ReadPixels_RTX;
	MOD_LoadMD2 = MOD_LoadMD2_RTX;
	MOD_LoadMD3 = MOD_LoadMD3_RTX;
	MOD_LoadIQM = MOD_LoadIQM_RTX;
	MOD_Reference = MOD_Reference_RTX;
}

// vim: shiftwidth=4 noexpandtab tabstop=4 cindent
