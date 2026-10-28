#pragma once
// Vulkan on a portability layer (MoltenVK): the instance and device extensions it needs, and the features of the
// subset it lacks, which the backend checks before it uses them. Elsewhere every call does nothing and every feature is there.
#include <vulkan/vulkan.h>

#include <vector>

namespace vkport {

struct Features {
    bool subset = false;  // the device is a portability layer
    bool constant_alpha_color_blend = true;
    bool sampler_lod_bias = true;
    bool mutable_comparison_samplers = true;
    bool triangle_fans = true;
    bool point_polygons = true;
    bool separate_stencil_mask_ref = true;
    bool image_view_2d_on_3d = true;
    bool multisample_array_image = true;
    bool vertex_attribute_beyond_stride = true;
    bool events = true;
};

// before vkCreateInstance: the extensions and flags the layer is listed with
void InstanceSetup(std::vector<const char*>& extensions, VkInstanceCreateFlags& flags);
// the picked device: adds VK_KHR_portability_subset to its extensions when it has it, and logs the features it lacks
void Query(VkPhysicalDevice pd, const std::vector<VkExtensionProperties>& available, std::vector<const char*>& extensions);
// the pNext chain of vkCreateDevice with the subset's features, as far as the device has them
void* Link(void* chain);
const Features& Get();

}  // namespace vkport
