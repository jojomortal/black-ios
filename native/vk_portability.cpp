// see vk_portability.h
#ifdef __APPLE__
#define VK_ENABLE_BETA_EXTENSIONS 1
#endif
#include "vk_portability.h"

#include <cstring>
#include <string>

#include "core/log.h"

namespace vkport {

namespace {

Features g_features;

#ifdef __APPLE__
VkPhysicalDevicePortabilitySubsetFeaturesKHR g_subset{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PORTABILITY_SUBSET_FEATURES_KHR};
bool g_linked = false;

bool Has(const std::vector<VkExtensionProperties>& list, const char* name) {
    for (const VkExtensionProperties& e : list)
        if (!std::strcmp(e.extensionName, name)) return true;
    return false;
}
#endif

}  // namespace

const Features& Get() { return g_features; }

#ifdef __APPLE__

void InstanceSetup(std::vector<const char*>& extensions, VkInstanceCreateFlags& flags) {
    uint32_t n = 0;
    vkEnumerateInstanceExtensionProperties(nullptr, &n, nullptr);
    std::vector<VkExtensionProperties> list(n);
    vkEnumerateInstanceExtensionProperties(nullptr, &n, list.data());
    // without these the layer lists no GPU
    auto add = [&](const char* name) {
        if (!Has(list, name)) return false;
        for (const char* e : extensions)
            if (!std::strcmp(e, name)) return true;
        extensions.push_back(name);
        return true;
    };
    if (add(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME)) flags |= VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
    else LOGR("vulkan: no %s (the layer is linked in, not enumerated by a loader)", VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
    add(VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME);
}

void Query(VkPhysicalDevice pd, const std::vector<VkExtensionProperties>& available, std::vector<const char*>& extensions) {
    g_features = {};
    g_linked = false;
    if (!Has(available, VK_KHR_PORTABILITY_SUBSET_EXTENSION_NAME)) return;
    // the spec has a device that lists it enable it
    extensions.push_back(VK_KHR_PORTABILITY_SUBSET_EXTENSION_NAME);
    g_subset = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PORTABILITY_SUBSET_FEATURES_KHR};
    VkPhysicalDeviceFeatures2 f2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &g_subset};
    vkGetPhysicalDeviceFeatures2(pd, &f2);
    g_linked = true;
    Features& f = g_features;
    f.subset = true;
    f.constant_alpha_color_blend = g_subset.constantAlphaColorBlendFactors;
    f.sampler_lod_bias = g_subset.samplerMipLodBias;
    f.mutable_comparison_samplers = g_subset.mutableComparisonSamplers;
    f.triangle_fans = g_subset.triangleFans;
    f.point_polygons = g_subset.pointPolygons;
    f.separate_stencil_mask_ref = g_subset.separateStencilMaskRef;
    f.image_view_2d_on_3d = g_subset.imageView2DOn3DImage;
    f.multisample_array_image = g_subset.multisampleArrayImage;
    f.vertex_attribute_beyond_stride = g_subset.vertexAttributeAccessBeyondStride;
    f.events = g_subset.events;
    std::string missing;
    auto note = [&](bool has, const char* name) {
        if (!has) missing += std::string(missing.empty() ? "" : ", ") + name;
    };
    note(f.constant_alpha_color_blend, "constantAlphaColorBlendFactors");
    note(f.sampler_lod_bias, "samplerMipLodBias");
    note(f.mutable_comparison_samplers, "mutableComparisonSamplers");
    note(f.triangle_fans, "triangleFans");
    note(f.point_polygons, "pointPolygons");
    note(f.separate_stencil_mask_ref, "separateStencilMaskRef");
    note(f.image_view_2d_on_3d, "imageView2DOn3DImage");
    note(f.multisample_array_image, "multisampleArrayImage");
    note(f.vertex_attribute_beyond_stride, "vertexAttributeAccessBeyondStride");
    note(f.events, "events");
    LOGR("vulkan: portability subset, missing: %s", missing.empty() ? "nothing" : missing.c_str());
    // what the backend uses: the ones it works around or cannot do without are said plainly
    if (!f.sampler_lod_bias) LOGR("vulkan: texture LOD bias is left out");
    if (!f.constant_alpha_color_blend) LOGR("vulkan: constant alpha blend factors become constant colour for colour");
    if (!f.mutable_comparison_samplers) LOGR("vulkan: comparison samplers are not mutable, shadows may be wrong");
}

void* Link(void* chain) {
    if (!g_linked) return chain;
    g_subset.pNext = chain;
    return &g_subset;
}

#else

void InstanceSetup(std::vector<const char*>&, VkInstanceCreateFlags&) {}
void Query(VkPhysicalDevice, const std::vector<VkExtensionProperties>&, std::vector<const char*>&) {}
void* Link(void* chain) { return chain; }

#endif

}  // namespace vkport
