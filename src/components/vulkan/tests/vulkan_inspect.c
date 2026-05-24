#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vulkan/vulkan.h>

int main(int argc, char **argv)
{
	VkResult result;
	VkInstance instance;
	VkPhysicalDevice *devices = NULL;
	uint32_t deviceCount = 0;
	VkInstanceCreateInfo instInfo;
	VkApplicationInfo appInfo;

	memset(&appInfo, 0, sizeof(appInfo));
	appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
	appInfo.pApplicationName = "PAPI Vulkan Inspector";
	appInfo.applicationVersion = 1;
	appInfo.apiVersion = VK_API_VERSION_1_0;

	memset(&instInfo, 0, sizeof(instInfo));
	instInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
	instInfo.pApplicationInfo = &appInfo;

	result = vkCreateInstance(&instInfo, NULL, &instance);
	if (result != VK_SUCCESS)
	{
		printf("FAIL: vkCreateInstance returned %d\n", result);
		return 1;
	}

	result = vkEnumeratePhysicalDevices(instance, &deviceCount, NULL);
	if (result != VK_SUCCESS || deviceCount == 0)
	{
		printf("No Vulkan physical devices found.\n");
		vkDestroyInstance(instance, NULL);
		return 1;
	}

	devices = (VkPhysicalDevice *)malloc(sizeof(VkPhysicalDevice) * deviceCount);
	vkEnumeratePhysicalDevices(instance, &deviceCount, devices);

	printf("Found %d Vulkan physical device(s):\n", deviceCount);

	for (uint32_t i = 0; i < deviceCount; i++)
	{
		VkPhysicalDeviceProperties props;
		VkPhysicalDeviceFeatures features;
		uint32_t extCount = 0;
		uint32_t qfCount = 0;
		int hasPerfQuery = 0;

		vkGetPhysicalDeviceProperties(devices[i], &props);
		vkGetPhysicalDeviceFeatures(devices[i], &features);

		printf("\n  Device %d: %s\n", i, props.deviceName);
		printf("    API version: %d.%d.%d\n",
			VK_VERSION_MAJOR(props.apiVersion),
			VK_VERSION_MINOR(props.apiVersion),
			VK_VERSION_PATCH(props.apiVersion));
		printf("    Driver version: %d\n", props.driverVersion);
		printf("    Device type: %d\n", props.deviceType);

		vkEnumerateDeviceExtensionProperties(devices[i], NULL, &extCount, NULL);
		VkExtensionProperties *exts = NULL;
		if (extCount > 0)
		{
			exts = (VkExtensionProperties *)malloc(
				sizeof(VkExtensionProperties) * extCount);
			vkEnumerateDeviceExtensionProperties(devices[i], NULL, &extCount, exts);
			for (uint32_t e = 0; e < extCount; e++)
			{
				printf("    Extension: %s\n", exts[e].extensionName);
				if (strcmp(exts[e].extensionName,
						VK_KHR_PERFORMANCE_QUERY_EXTENSION_NAME) == 0)
					hasPerfQuery = 1;
			}
			free(exts);
		}

		vkGetPhysicalDeviceQueueFamilyProperties(devices[i], &qfCount, NULL);
		printf("    Queue families: %d\n", qfCount);

		if (qfCount > 0)
		{
			VkQueueFamilyProperties *qfProps = (VkQueueFamilyProperties *)
				malloc(sizeof(VkQueueFamilyProperties) * qfCount);
			vkGetPhysicalDeviceQueueFamilyProperties(devices[i], &qfCount, qfProps);

			for (uint32_t q = 0; q < qfCount; q++)
			{
				printf("      QF %d: flags=0x%x count=%d\n",
					q, qfProps[q].queueFlags, qfProps[q].queueCount);

				if (hasPerfQuery)
				{
					PFN_vkEnumeratePhysicalDeviceQueueFamilyPerformanceQueryCountersKHR
						enumCounters =
						(PFN_vkEnumeratePhysicalDeviceQueueFamilyPerformanceQueryCountersKHR)
						vkGetInstanceProcAddr(instance,
							"vkEnumeratePhysicalDeviceQueueFamilyPerformanceQueryCountersKHR");

					if (enumCounters)
					{
						uint32_t counterCount = 0;
						result = enumCounters(devices[i], q, &counterCount, NULL, NULL);
						if (result == VK_SUCCESS && counterCount > 0)
						{
							printf("        Performance counters: %d\n", counterCount);

							VkPerformanceCounterKHR *counters =
								(VkPerformanceCounterKHR *)malloc(
									sizeof(VkPerformanceCounterKHR) * counterCount);
							VkPerformanceCounterDescriptionKHR *descriptions =
								(VkPerformanceCounterDescriptionKHR *)malloc(
									sizeof(VkPerformanceCounterDescriptionKHR) * counterCount);

							for (uint32_t c = 0; c < counterCount; c++)
							{
								counters[c].sType =
									VK_STRUCTURE_TYPE_PERFORMANCE_COUNTER_KHR;
								counters[c].pNext = NULL;
								descriptions[c].sType =
									VK_STRUCTURE_TYPE_PERFORMANCE_COUNTER_DESCRIPTION_KHR;
								descriptions[c].pNext = NULL;
							}

							enumCounters(devices[i], q, &counterCount, counters, descriptions);

							for (uint32_t c = 0; c < counterCount; c++)
							{
								printf("          [%d] %s (%s)\n", c,
									descriptions[c].name, descriptions[c].category);
								printf("                %s\n",
									descriptions[c].description);
							}

							free(counters);
							free(descriptions);
						}
					}
				}
			}
			free(qfProps);
		}
	}

	free(devices);
	vkDestroyInstance(instance, NULL);

	printf("\nPAPI Vulkan component test PASSED.\n");
	return 0;
}
