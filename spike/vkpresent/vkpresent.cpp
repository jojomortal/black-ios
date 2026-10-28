// the smallest Vulkan presenter on SDL3 + MoltenVK: clears the swap chain to magenta and presents it
// usage: vkpresent [--worker] [--surface-main] [--fullscreen] [--seconds N]
//   --worker: the Vulkan work (surface, swap chain, presents) on a thread of its own, the main thread pumping events
//   --swapchain-main: as --surface-main, and the swap chain too
//   --surface-main: with --worker, the surface is made on the main thread (SDL_RunOnMainThread), the rest on the worker
#include <vulkan/vulkan.h>

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>

#define CHECK(x) do { VkResult r_ = (x); if (r_ != VK_SUCCESS) { std::printf("%s failed: %d\n", #x, int(r_)); return 1; } } while (0)

static SDL_Window* g_window;
static std::atomic<bool> g_quit{false};
static bool g_worker, g_full, g_surface_main, g_swap_main;
struct SwapJob { VkDevice dev; const VkSwapchainCreateInfoKHR* ci; VkSwapchainKHR swap; VkResult r; };
static void SDLCALL MakeSwap(void* p) {
    auto* j = static_cast<SwapJob*>(p);
    j->r = vkCreateSwapchainKHR(j->dev, j->ci, nullptr, &j->swap);
}
struct SurfaceJob { VkInstance inst; VkSurfaceKHR surf; bool ok; };
static void SDLCALL MakeSurface(void* p) {
    auto* j = static_cast<SurfaceJob*>(p);
    j->ok = SDL_Vulkan_CreateSurface(g_window, j->inst, nullptr, &j->surf);
}

static int Run() {
    uint32_t n = 0;
    const char* const* sdl = SDL_Vulkan_GetInstanceExtensions(&n);
    std::vector<const char*> exts(sdl, sdl + n);
    VkApplicationInfo ai{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    ai.apiVersion = VK_API_VERSION_1_1;
    VkInstanceCreateInfo ci{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ci.pApplicationInfo = &ai;
    ci.enabledExtensionCount = uint32_t(exts.size());
    ci.ppEnabledExtensionNames = exts.data();
    VkInstance inst;
    CHECK(vkCreateInstance(&ci, nullptr, &inst));
    SurfaceJob job{inst, VK_NULL_HANDLE, false};
    if (g_surface_main && !SDL_IsMainThread()) SDL_RunOnMainThread(MakeSurface, &job, true);
    else MakeSurface(&job);
    if (!job.ok) { std::printf("surface: %s\n", SDL_GetError()); return 1; }
    VkSurfaceKHR surf = job.surf;
    std::printf("surface made on the %s\n", g_surface_main && !SDL_IsMainThread() ? "main thread (marshalled)" : SDL_IsMainThread() ? "main thread" : "worker thread");
    uint32_t pn = 1;
    VkPhysicalDevice pd;
    vkEnumeratePhysicalDevices(inst, &pn, &pd);
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(pd, &props);
    std::printf("GPU %s\n", props.deviceName);
    float prio = 1;
    VkDeviceQueueCreateInfo qi{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qi.queueCount = 1;
    qi.pQueuePriorities = &prio;
    const char* dexts[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME, "VK_KHR_portability_subset"};
    VkDeviceCreateInfo di{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    di.queueCreateInfoCount = 1;
    di.pQueueCreateInfos = &qi;
    di.enabledExtensionCount = 2;
    di.ppEnabledExtensionNames = dexts;
    VkDevice dev;
    CHECK(vkCreateDevice(pd, &di, nullptr, &dev));
    VkQueue q;
    vkGetDeviceQueue(dev, 0, 0, &q);
    int w, h;
    SDL_GetWindowSizeInPixels(g_window, &w, &h);
    VkSwapchainCreateInfoKHR sc{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    sc.surface = surf;
    sc.minImageCount = 3;
    sc.imageFormat = VK_FORMAT_B8G8R8A8_UNORM;
    sc.imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    sc.imageExtent = {uint32_t(w), uint32_t(h)};
    sc.imageArrayLayers = 1;
    sc.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    sc.preTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    sc.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    sc.presentMode = VK_PRESENT_MODE_FIFO_KHR;
    sc.clipped = VK_TRUE;
    VkSwapchainKHR swap;
    SwapJob sj{dev, &sc, VK_NULL_HANDLE, VK_SUCCESS};
    if (g_swap_main && !SDL_IsMainThread()) SDL_RunOnMainThread(MakeSwap, &sj, true);
    else MakeSwap(&sj);
    CHECK(sj.r);
    swap = sj.swap;
    uint32_t count = 0;
    vkGetSwapchainImagesKHR(dev, swap, &count, nullptr);
    std::vector<VkImage> images(count);
    vkGetSwapchainImagesKHR(dev, swap, &count, images.data());
    std::printf("swap chain %dx%d, %u images\n", w, h, count);
    VkCommandPoolCreateInfo pi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    VkCommandPool pool;
    CHECK(vkCreateCommandPool(dev, &pi, nullptr, &pool));
    VkCommandBufferAllocateInfo cbi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cbi.commandPool = pool;
    cbi.commandBufferCount = 1;
    VkCommandBuffer cb;
    CHECK(vkAllocateCommandBuffers(dev, &cbi, &cb));
    VkSemaphoreCreateInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    VkSemaphore acquire, done;
    vkCreateSemaphore(dev, &si, nullptr, &acquire);
    vkCreateSemaphore(dev, &si, nullptr, &done);
    VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkFence fence;
    vkCreateFence(dev, &fi, nullptr, &fence);
    unsigned frames = 0;
    while (!g_quit) {
        uint32_t idx;
        VkResult r = vkAcquireNextImageKHR(dev, swap, UINT64_MAX, acquire, VK_NULL_HANDLE, &idx);
        if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) { std::printf("acquire %d\n", int(r)); break; }
        vkResetCommandBuffer(cb, 0);
        VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        vkBeginCommandBuffer(cb, &bi);
        VkImageSubresourceRange rng{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = images[idx];
        b.subresourceRange = rng;
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
        VkClearColorValue magenta{{1, 0, 1, 1}};
        vkCmdClearColorImage(cb, images[idx], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &magenta, 1, &rng);
        b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        b.dstAccessMask = 0;
        b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        b.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
        vkEndCommandBuffer(cb);
        VkPipelineStageFlags wait = VK_PIPELINE_STAGE_TRANSFER_BIT;
        VkSubmitInfo sub{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        sub.waitSemaphoreCount = 1;
        sub.pWaitSemaphores = &acquire;
        sub.pWaitDstStageMask = &wait;
        sub.commandBufferCount = 1;
        sub.pCommandBuffers = &cb;
        sub.signalSemaphoreCount = 1;
        sub.pSignalSemaphores = &done;
        vkQueueSubmit(q, 1, &sub, fence);
        VkPresentInfoKHR pr{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
        pr.waitSemaphoreCount = 1;
        pr.pWaitSemaphores = &done;
        pr.swapchainCount = 1;
        pr.pSwapchains = &swap;
        pr.pImageIndices = &idx;
        r = vkQueuePresentKHR(q, &pr);
        vkWaitForFences(dev, 1, &fence, VK_TRUE, UINT64_MAX);
        vkResetFences(dev, 1, &fence);
        if (++frames % 120 == 0) std::printf("%u frames, present %d\n", frames, int(r));
        if (!g_worker) {
            SDL_Event e;
            while (SDL_PollEvent(&e)) if (e.type == SDL_EVENT_QUIT) g_quit = true;
        }
    }
    vkDeviceWaitIdle(dev);
    return 0;
}

int main(int argc, char** argv) {
    int seconds = 12;
    for (int i = 1; i < argc; i++) {
        if (!std::strcmp(argv[i], "--worker")) g_worker = true;
        else if (!std::strcmp(argv[i], "--fullscreen")) g_full = true;
        else if (!std::strcmp(argv[i], "--surface-main")) g_surface_main = true;
        else if (!std::strcmp(argv[i], "--swapchain-main")) g_swap_main = g_surface_main = true;
        else if (!std::strcmp(argv[i], "--seconds") && i + 1 < argc) seconds = std::atoi(argv[++i]);
    }
    SDL_Init(SDL_INIT_VIDEO);
    SDL_WindowFlags flags = SDL_WINDOW_VULKAN | SDL_WINDOW_HIGH_PIXEL_DENSITY | (g_full ? SDL_WINDOW_FULLSCREEN : 0);
    g_window = SDL_CreateWindow("vkpresent", 1280, 720, flags);
    if (!g_window) { std::printf("window: %s\n", SDL_GetError()); return 1; }
    std::printf("mode: %s%s, %s\n", g_worker ? "worker thread" : "main thread", g_surface_main ? " + surface on main" : "", g_full ? "fullscreen" : "windowed");
    uint64_t end = SDL_GetTicks() + uint64_t(seconds) * 1000;
    if (g_worker) {
        std::thread t(Run);
        while (SDL_GetTicks() < end && !g_quit) {
            SDL_Event e;
            if (SDL_WaitEventTimeout(&e, 50) && e.type == SDL_EVENT_QUIT) g_quit = true;
        }
        g_quit = true;
        t.join();
    } else {
        std::thread timer([&] { while (SDL_GetTicks() < end) SDL_Delay(50); g_quit = true; });
        Run();
        timer.join();
    }
    return 0;
}
