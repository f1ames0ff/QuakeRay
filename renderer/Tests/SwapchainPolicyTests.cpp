#include "SwapchainPolicy.h"

#include <iostream>
#include <stdexcept>

using namespace qray;

static void Check(bool condition, const char *message)
{
    if (!condition)
    {
        throw std::runtime_error(message);
    }
}

static bool Frame(SwapchainRecreateState &state, VkResult acquire, VkResult present, bool changed = false)
{
    const bool force = state.BeginAcquire(changed);
    state.Acquired(acquire);
    state.Presented(present);
    return force;
}

static void TestEpisodes(VkResult acquire, VkResult present)
{
    SwapchainRecreateState state;
    Check(!Frame(state, acquire, present), "First suboptimal frame must remain usable");
    Check(Frame(state, acquire, present), "Next frame must rebuild once");
    for (int i = 0; i < 16; i++)
    {
        Check(!Frame(state, acquire, present), "Persistent episode must not rebuild repeatedly");
    }

    Check(!Frame(state, VK_SUCCESS, VK_SUCCESS), "Recovery frame must not rebuild");
    Check(!Frame(state, acquire, present), "New episode must start with a usable frame");
    Check(Frame(state, acquire, present), "Recovered guard must allow a new rebuild");
    Check(!Frame(state, acquire, present, true), "Parameter changes must not request an extra forced rebuild");
    Check(Frame(state, acquire, present), "Changed parameters must rearm the episode guard");
}

static std::vector<VkExtensionProperties> Properties(unsigned mask, const char *khr, const char *ext)
{
    std::vector<VkExtensionProperties> result;
    const char *names[] = { khr, ext };
    for (unsigned i = 0; i < 2; i++)
    {
        if (mask & (1u << i))
        {
            VkExtensionProperties property{};
            std::strcpy(property.extensionName, names[i]);
            result.push_back(property);
        }
    }
    return result;
}

static void TestExtensions()
{
    for (unsigned instanceMask = 0; instanceMask < 4; instanceMask++)
    {
        const auto instance = Properties(instanceMask, VK_KHR_SURFACE_MAINTENANCE_1_EXTENSION_NAME,
                                        VK_EXT_SURFACE_MAINTENANCE_1_EXTENSION_NAME);
        for (bool capabilities2 : { false, true })
        {
            std::vector<const char *> names;
            AppendSurfaceMaintenanceExtensions(names, instance, capabilities2);
            AppendSurfaceMaintenanceExtensions(names, instance, capabilities2);
            const size_t expectedCount = capabilities2 ? ((instanceMask & 1u) + ((instanceMask >> 1u) & 1u)) : 0;
            Check(names.size() == expectedCount, "Enable all supported instance spellings once, only with dependencies");

            std::vector<std::string> enabled(names.begin(), names.end());
            for (unsigned deviceMask = 0; deviceMask < 4; deviceMask++)
            {
                auto device = Properties(deviceMask, VK_KHR_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME,
                                         VK_EXT_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME);
                for (bool reverse : { false, true })
                {
                    if (reverse)
                    {
                        std::reverse(device.begin(), device.end());
                    }
                    const char *selected = SelectSwapchainMaintenanceExtension(device, enabled);
                    const unsigned pairs = capabilities2 ? (instanceMask & deviceMask) : 0;
                    const char *expected = (pairs & 1u) ? VK_KHR_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME :
                                           (pairs & 2u) ? VK_EXT_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME : nullptr;
                    Check((selected == nullptr) == (expected == nullptr), "Select only a complete supported pair");
                    if (expected)
                    {
                        Check(std::strcmp(selected, expected) == 0, "Prefer KHR, but preserve available EXT fallback");
                    }
                }
            }
        }
    }
}

int main()
{
    try
    {
        TestEpisodes(VK_SUBOPTIMAL_KHR, VK_SUCCESS);
        TestEpisodes(VK_SUCCESS, VK_SUBOPTIMAL_KHR);
        TestEpisodes(VK_SUBOPTIMAL_KHR, VK_SUBOPTIMAL_KHR);
        SwapchainRecreateState state;
        for (int i = 0; i < 16; i++)
        {
            Check(!Frame(state, VK_SUCCESS, VK_SUCCESS), "Optimal frames must not force recreation");
        }
        TestExtensions();
        std::cout << "PASS: suboptimal episodes and 64 extension/order profiles\n";
        return 0;
    }
    catch (const std::exception &error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
