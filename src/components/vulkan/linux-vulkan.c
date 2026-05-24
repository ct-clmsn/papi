#include <dlfcn.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#include "papi.h"
#include "papi_internal.h"
#include "papi_vector.h"
#include "papi_memory.h"
#include "linux-vulkan.h"

static void *vulkan_lib = NULL;

static PFN_vkGetInstanceProcAddr vkGetInstanceProcAddrPtr;
static PFN_vkCreateInstance vkCreateInstancePtr;
static PFN_vkDestroyInstance vkDestroyInstancePtr;
static PFN_vkEnumeratePhysicalDevices vkEnumeratePhysicalDevicesPtr;
static PFN_vkGetPhysicalDeviceProperties vkGetPhysicalDevicePropertiesPtr;
static PFN_vkGetPhysicalDeviceQueueFamilyProperties vkGetPhysicalDeviceQueueFamilyPropertiesPtr;
static PFN_vkGetPhysicalDeviceFeatures vkGetPhysicalDeviceFeaturesPtr;
static PFN_vkCreateDevice vkCreateDevicePtr;
static PFN_vkDestroyDevice vkDestroyDevicePtr;
static PFN_vkDeviceWaitIdle vkDeviceWaitIdlePtr;
static PFN_vkGetDeviceQueue vkGetDeviceQueuePtr;
static PFN_vkCreateQueryPool vkCreateQueryPoolPtr;
static PFN_vkDestroyQueryPool vkDestroyQueryPoolPtr;
static PFN_vkGetQueryPoolResults vkGetQueryPoolResultsPtr;
static void (*vkResetQueryPoolPtr)(VkDevice, VkQueryPool, uint32_t, uint32_t);

static PFN_vkEnumeratePhysicalDeviceQueueFamilyPerformanceQueryCountersKHR
	vkEnumeratePhysicalDeviceQueueFamilyPerformanceQueryCountersKHRPtr;
static PFN_vkGetPhysicalDeviceQueueFamilyPerformanceQueryPassesKHR
	vkGetPhysicalDeviceQueueFamilyPerformanceQueryPassesKHRPtr;
static PFN_vkAcquireProfilingLockKHR vkAcquireProfilingLockKHRPtr;
static PFN_vkReleaseProfilingLockKHR vkReleaseProfilingLockKHRPtr;

papi_vector_t _vulkan_vector;

static int linkVulkanLibraries(void);

static int
queueFamilyTypeString(VkQueueFlags flags, char *str, int len)
{
	if (flags & VK_QUEUE_GRAPHICS_BIT)
		snprintf(str, len, "graphics");
	else if (flags & VK_QUEUE_COMPUTE_BIT)
		snprintf(str, len, "compute");
	else if (flags & VK_QUEUE_TRANSFER_BIT)
		snprintf(str, len, "transfer");
	else if (flags & VK_QUEUE_SPARSE_BINDING_BIT)
		snprintf(str, len, "sparse_binding");
	else if (flags & VK_QUEUE_PROTECTED_BIT)
		snprintf(str, len, "protected");
	else
		snprintf(str, len, "unknown");
	return PAPI_OK;
}

#define VULKAN_LOAD_INSTANCE_FUNC(name) \
	do { \
		name##Ptr = (PFN_##name)(*vkGetInstanceProcAddrPtr)(vulkanInstance, #name); \
		if (!name##Ptr) { \
			snprintf(_vulkan_vector.cmp_info.disabled_reason, PAPI_MAX_STR_LEN, \
				"Failed to load Vulkan function: %s", #name); \
			return PAPI_ENOSUPP; \
		} \
	} while(0)

#define VULKAN_LOAD_OPTIONAL_INSTANCE_FUNC(name) \
	name##Ptr = (PFN_##name)(*vkGetInstanceProcAddrPtr)(vulkanInstance, #name)

static int
detectVulkan(void)
{
	VkResult result;
	uint32_t physicalDeviceCount = 0;
	uint32_t i;

	result = (*vkEnumeratePhysicalDevicesPtr)(vulkanInstance, &physicalDeviceCount, NULL);
	if (result != VK_SUCCESS || physicalDeviceCount == 0)
		return PAPI_ENOSUPP;

	if (physicalDeviceCount > VULKAN_MAX_PHYSICAL_DEVICES)
		physicalDeviceCount = VULKAN_MAX_PHYSICAL_DEVICES;

	vulkanPhysicalDevices = (VkPhysicalDevice *)malloc(
		sizeof(VkPhysicalDevice) * physicalDeviceCount);
	if (!vulkanPhysicalDevices)
		return PAPI_ENOMEM;

	result = (*vkEnumeratePhysicalDevicesPtr)(vulkanInstance,
		&physicalDeviceCount, vulkanPhysicalDevices);
	if (result != VK_SUCCESS)
	{
		free(vulkanPhysicalDevices);
		vulkanPhysicalDevices = NULL;
		return PAPI_ENOSUPP;
	}

	for (i = 0; i < physicalDeviceCount; i++)
	{
		VkPhysicalDeviceProperties props;
		uint32_t qfCount = 0;
		uint32_t j;

		(*vkGetPhysicalDevicePropertiesPtr)(vulkanPhysicalDevices[i], &props);

		vulkanDevices[vulkanDeviceCount].physicalDevice = vulkanPhysicalDevices[i];
		vulkanDevices[vulkanDeviceCount].device = VK_NULL_HANDLE;
		vulkanDevices[vulkanDeviceCount].initialized = 0;
		snprintf(vulkanDevices[vulkanDeviceCount].name, PAPI_MIN_STR_LEN, "%s",
				 props.deviceName);

		(*vkGetPhysicalDeviceQueueFamilyPropertiesPtr)(vulkanPhysicalDevices[i],
			&qfCount, NULL);
		if (qfCount > VULKAN_MAX_QUEUE_FAMILIES)
			qfCount = VULKAN_MAX_QUEUE_FAMILIES;

		vulkanDevices[vulkanDeviceCount].queueFamilyCount = qfCount;
		vulkanDevices[vulkanDeviceCount].queueFamilies =
			(VULKAN_queue_family_data_t *)calloc(qfCount,
				sizeof(VULKAN_queue_family_data_t));
		if (!vulkanDevices[vulkanDeviceCount].queueFamilies)
			continue;

		if (qfCount > 0)
		{
			VkQueueFamilyProperties *qfProps = (VkQueueFamilyProperties *)calloc(qfCount,
				sizeof(VkQueueFamilyProperties));
			if (!qfProps)
				continue;

			(*vkGetPhysicalDeviceQueueFamilyPropertiesPtr)(vulkanPhysicalDevices[i],
				&qfCount, qfProps);

			for (j = 0; j < qfCount; j++)
			{
				uint32_t counterCount = 0;

				vulkanDevices[vulkanDeviceCount].queueFamilies[j].queueFamilyIndex = j;
				queueFamilyTypeString(qfProps[j].queueFlags,
					vulkanDevices[vulkanDeviceCount].queueFamilies[j].name,
					PAPI_MIN_STR_LEN);

				if (!vkEnumeratePhysicalDeviceQueueFamilyPerformanceQueryCountersKHRPtr)
					continue;

				result = (*vkEnumeratePhysicalDeviceQueueFamilyPerformanceQueryCountersKHRPtr)(
					vulkanPhysicalDevices[i], j, &counterCount, NULL, NULL);
				if (result != VK_SUCCESS || counterCount == 0)
					continue;

				VkPerformanceCounterKHR *counters =
					(VkPerformanceCounterKHR *)calloc(counterCount,
						sizeof(VkPerformanceCounterKHR));
				VkPerformanceCounterDescriptionKHR *descriptions =
					(VkPerformanceCounterDescriptionKHR *)calloc(counterCount,
						sizeof(VkPerformanceCounterDescriptionKHR));

				if (!counters || !descriptions)
				{
					free(counters);
					free(descriptions);
					continue;
				}

				for (uint32_t k = 0; k < counterCount; k++)
				{
					counters[k].sType = VK_STRUCTURE_TYPE_PERFORMANCE_COUNTER_KHR;
					counters[k].pNext = NULL;
					descriptions[k].sType =
						VK_STRUCTURE_TYPE_PERFORMANCE_COUNTER_DESCRIPTION_KHR;
					descriptions[k].pNext = NULL;
				}

				result = (*vkEnumeratePhysicalDeviceQueueFamilyPerformanceQueryCountersKHRPtr)(
					vulkanPhysicalDevices[i], j, &counterCount, counters, descriptions);
				if (result != VK_SUCCESS)
				{
					free(counters);
					free(descriptions);
					continue;
				}

				vulkanDevices[vulkanDeviceCount].queueFamilies[j].counterCount =
					counterCount;
				vulkanDevices[vulkanDeviceCount].queueFamilies[j].counters =
					(VULKAN_counter_data_t *)calloc(counterCount,
						sizeof(VULKAN_counter_data_t));

				if (vulkanDevices[vulkanDeviceCount].queueFamilies[j].counters)
				{
					for (uint32_t k = 0; k < counterCount; k++)
					{
						VULKAN_counter_data_t *cd = &vulkanDevices[vulkanDeviceCount]
							.queueFamilies[j].counters[k];
						cd->counterIndex = k;
						cd->storageType = counters[k].storage;

						strncpy(cd->name, descriptions[k].name,
							PAPI_MAX_STR_LEN - 1);
						cd->name[PAPI_MAX_STR_LEN - 1] = '\0';

						strncpy(cd->category, descriptions[k].category,
							PAPI_MAX_STR_LEN - 1);
						cd->category[PAPI_MAX_STR_LEN - 1] = '\0';

						strncpy(cd->description, descriptions[k].description,
							PAPI_2MAX_STR_LEN - 1);
						cd->description[PAPI_2MAX_STR_LEN - 1] = '\0';
					}
				}

				free(counters);
				free(descriptions);
			}
			free(qfProps);
		}

		vulkanDeviceCount++;
	}

	if (vulkanDeviceCount == 0)
	{
		free(vulkanPhysicalDevices);
		vulkanPhysicalDevices = NULL;
		return PAPI_ENOSUPP;
	}

	return PAPI_OK;
}

static int
createNativeEvents(void)
{
	int id = 0;

	for (int dev = 0; dev < vulkanDeviceCount; dev++)
	{
		for (uint32_t qf = 0; qf < vulkanDevices[dev].queueFamilyCount; qf++)
		{
			for (uint32_t cnt = 0;
				 cnt < vulkanDevices[dev].queueFamilies[qf].counterCount; cnt++)
			{
				snprintf(vulkan_native_table[id].name, PAPI_MAX_STR_LEN,
					"%s:qf_%d:%s",
					vulkanDevices[dev].name, qf,
					vulkanDevices[dev].queueFamilies[qf].counters[cnt].name);

				snprintf(vulkan_native_table[id].description, PAPI_2MAX_STR_LEN,
					"Vulkan %s counter [%s] on %s qf%d",
					vulkanDevices[dev].queueFamilies[qf].counters[cnt].name,
					vulkanDevices[dev].queueFamilies[qf].counters[cnt].category,
					vulkanDevices[dev].name, qf);

				vulkan_native_table[id].resources.selector = id + 1;
				vulkan_native_table[id].resources.deviceIndex = dev;
				vulkan_native_table[id].resources.queueFamilyIndex = qf;
				vulkan_native_table[id].resources.counterIndex = cnt;

				id++;
			}
		}
	}

	return id;
}

static int
initializeDevice(int deviceIndex)
{
	VkResult result;

	if (deviceIndex < 0 || deviceIndex >= vulkanDeviceCount)
		return PAPI_EINVAL;

	if (vulkanDevices[deviceIndex].initialized)
		return PAPI_OK;

	VkPhysicalDevice physDev = vulkanDevices[deviceIndex].physicalDevice;

	uint32_t qfCount = 0;
	(*vkGetPhysicalDeviceQueueFamilyPropertiesPtr)(physDev, &qfCount, NULL);

	VkQueueFamilyProperties *qfProps = NULL;
	if (qfCount > 0)
	{
		qfProps = (VkQueueFamilyProperties *)calloc(qfCount,
			sizeof(VkQueueFamilyProperties));
		if (!qfProps)
			return PAPI_ENOMEM;
		(*vkGetPhysicalDeviceQueueFamilyPropertiesPtr)(physDev, &qfCount, qfProps);
	}

	uint32_t qfIndex = 0;
	for (uint32_t i = 0; i < qfCount; i++)
	{
		if (qfProps[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)
		{
			qfIndex = i;
			break;
		}
		if (qfProps[i].queueFlags & VK_QUEUE_COMPUTE_BIT)
		{
			qfIndex = i;
			break;
		}
	}

	float queuePriority = 1.0f;
	VkDeviceQueueCreateInfo queueCreateInfo;
	memset(&queueCreateInfo, 0, sizeof(queueCreateInfo));
	queueCreateInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
	queueCreateInfo.queueFamilyIndex = qfIndex;
	queueCreateInfo.queueCount = 1;
	queueCreateInfo.pQueuePriorities = &queuePriority;

	VkPhysicalDeviceFeatures features;
	(*vkGetPhysicalDeviceFeaturesPtr)(physDev, &features);

	const char *extensions[] = {
		VK_KHR_PERFORMANCE_QUERY_EXTENSION_NAME
	};

	VkDeviceCreateInfo deviceCreateInfo;
	memset(&deviceCreateInfo, 0, sizeof(deviceCreateInfo));
	deviceCreateInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
	deviceCreateInfo.queueCreateInfoCount = 1;
	deviceCreateInfo.pQueueCreateInfos = &queueCreateInfo;
	deviceCreateInfo.pEnabledFeatures = &features;
	deviceCreateInfo.enabledExtensionCount = 1;
	deviceCreateInfo.ppEnabledExtensionNames = extensions;

	result = (*vkCreateDevicePtr)(physDev, &deviceCreateInfo, NULL,
		&vulkanDevices[deviceIndex].device);
	free(qfProps);

	if (result != VK_SUCCESS)
		return PAPI_ENOSUPP;

	vulkanDevices[deviceIndex].initialized = 1;
	return PAPI_OK;
}

static int
getQueryResults(VULKAN_control_state_t *vk_ctrl)
{
	if (vk_ctrl->queryPool == VK_NULL_HANDLE || vk_ctrl->device == VK_NULL_HANDLE)
		return PAPI_ECNFLCT;

	(*vkDeviceWaitIdlePtr)(vk_ctrl->device);

	if (vk_ctrl->ncounters <= 0 || vk_ctrl->addedCounters.count <= 0)
		return PAPI_OK;

	VkPerformanceCounterResultKHR *results =
		(VkPerformanceCounterResultKHR *)calloc(vk_ctrl->ncounters,
			sizeof(VkPerformanceCounterResultKHR));
	if (!results)
		return PAPI_ENOMEM;

	VkResult result = (*vkGetQueryPoolResultsPtr)(
		vk_ctrl->device, vk_ctrl->queryPool, 0, 1,
		vk_ctrl->ncounters * sizeof(VkPerformanceCounterResultKHR),
		results, sizeof(VkPerformanceCounterResultKHR),
		VK_QUERY_RESULT_64_BIT);

	if (result == VK_SUCCESS || result == VK_NOT_READY)
	{
		for (int i = 0; i < vk_ctrl->addedCounters.count && i < vk_ctrl->ncounters; i++)
		{
			int idx = vk_ctrl->addedCounters.list[i];
			uint32_t devIdx = vulkan_native_table[idx].resources.deviceIndex;
			uint32_t qfIdx = vulkan_native_table[idx].resources.queueFamilyIndex;
			uint32_t cntIdx = vulkan_native_table[idx].resources.counterIndex;

			if (devIdx < (uint32_t)vulkanDeviceCount &&
				qfIdx < vulkanDevices[devIdx].queueFamilyCount &&
				cntIdx < vulkanDevices[devIdx].queueFamilies[qfIdx].counterCount)
			{
				int st = vulkanDevices[devIdx].queueFamilies[qfIdx].counters[cntIdx].storageType;
				vk_ctrl->counts[idx] += convertCounterResult(&results[i], st);
			}
		}
	}

	free(results);
	return PAPI_OK;
}

static long long
convertCounterResult(const VkPerformanceCounterResultKHR *result, int storageType)
{
	switch (storageType)
	{
	case VK_PERFORMANCE_COUNTER_STORAGE_INT32_KHR:
		return (long long)result->int32;
	case VK_PERFORMANCE_COUNTER_STORAGE_INT64_KHR:
		return (long long)result->int64;
	case VK_PERFORMANCE_COUNTER_STORAGE_UINT32_KHR:
		return (long long)result->uint32;
	case VK_PERFORMANCE_COUNTER_STORAGE_UINT64_KHR:
		return (long long)result->uint64;
	case VK_PERFORMANCE_COUNTER_STORAGE_FLOAT32_KHR:
		return (long long)result->float32;
	case VK_PERFORMANCE_COUNTER_STORAGE_FLOAT64_KHR:
		return (long long)result->float64;
	default:
		return 0;
	}
}

/******************** PAPI COMPONENT CALLBACKS ********************/

int
VULKAN_init_thread(hwd_context_t *ctx)
{
	(void)ctx;
	return PAPI_OK;
}

int
VULKAN_init_component(int cidx)
{
	SUBDBG("VULKAN_init_component: cidx=%d\n", cidx);

	if (linkVulkanLibraries() != PAPI_OK)
		return PAPI_ENOSUPP;

	if (detectVulkan() != PAPI_OK)
	{
		strncpy(_vulkan_vector.cmp_info.disabled_reason,
			"No Vulkan devices with performance counters found.",
			PAPI_MAX_STR_LEN);
		return PAPI_ENOSUPP;
	}

	NUM_EVENTS = 0;
	for (int dev = 0; dev < vulkanDeviceCount; dev++)
		for (uint32_t qf = 0; qf < vulkanDevices[dev].queueFamilyCount; qf++)
			NUM_EVENTS += vulkanDevices[dev].queueFamilies[qf].counterCount;

	if (NUM_EVENTS == 0)
	{
		strncpy(_vulkan_vector.cmp_info.disabled_reason,
			"No performance counters available on any Vulkan device.",
			PAPI_MAX_STR_LEN);
		return PAPI_ENOSUPP;
	}

	vulkan_native_table = (VULKAN_native_event_entry_t *)
		papi_calloc(sizeof(VULKAN_native_event_entry_t), NUM_EVENTS);
	if (!vulkan_native_table)
	{
		strncpy(_vulkan_vector.cmp_info.disabled_reason,
			"Failed to allocate event table.",
			PAPI_MAX_STR_LEN);
		return PAPI_ENOMEM;
	}

	if (createNativeEvents() != NUM_EVENTS)
	{
		strncpy(_vulkan_vector.cmp_info.disabled_reason,
			"Error creating native event list.",
			PAPI_MAX_STR_LEN);
		return PAPI_ENOSUPP;
	}

	_vulkan_vector.cmp_info.num_native_events = NUM_EVENTS;
	_vulkan_vector.cmp_info.CmpIdx = cidx;

	if (getenv("PAPI_VERBOSE"))
		printf("Vulkan component: %d devices, %d counters\n",
			   vulkanDeviceCount, NUM_EVENTS);

	return PAPI_OK;
}

static int
linkVulkanLibraries(void)
{
	vulkan_lib = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_GLOBAL);
	if (!vulkan_lib)
		vulkan_lib = dlopen("libvulkan.so", RTLD_NOW | RTLD_GLOBAL);
	if (!vulkan_lib)
		vulkan_lib = dlopen("libvulkan.1.dylib", RTLD_NOW | RTLD_GLOBAL);
	if (!vulkan_lib)
	{
		strncpy(_vulkan_vector.cmp_info.disabled_reason,
			"Vulkan loader library not found.",
			PAPI_MAX_STR_LEN);
		return PAPI_ENOSUPP;
	}

	vkGetInstanceProcAddrPtr = (PFN_vkGetInstanceProcAddr)
		dlsym(vulkan_lib, "vkGetInstanceProcAddr");
	if (!vkGetInstanceProcAddrPtr)
	{
		strncpy(_vulkan_vector.cmp_info.disabled_reason,
			"vkGetInstanceProcAddr not found.",
			PAPI_MAX_STR_LEN);
		return PAPI_ENOSUPP;
	}

	vkCreateInstancePtr = (PFN_vkCreateInstance)
		(*vkGetInstanceProcAddrPtr)(NULL, "vkCreateInstance");
	if (!vkCreateInstancePtr)
	{
		strncpy(_vulkan_vector.cmp_info.disabled_reason,
			"vkCreateInstance not found.",
			PAPI_MAX_STR_LEN);
		return PAPI_ENOSUPP;
	}

	VkApplicationInfo appInfo;
	memset(&appInfo, 0, sizeof(appInfo));
	appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
	appInfo.pApplicationName = "PAPI Vulkan Component";
	appInfo.applicationVersion = 1;
	appInfo.apiVersion = VK_API_VERSION_1_0;

	VkInstanceCreateInfo instInfo;
	memset(&instInfo, 0, sizeof(instInfo));
	instInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
	instInfo.pApplicationInfo = &appInfo;
	instInfo.enabledExtensionCount = 0;
	instInfo.ppEnabledExtensionNames = NULL;
	instInfo.enabledLayerCount = 0;

	VkResult result = (*vkCreateInstancePtr)(&instInfo, NULL, &vulkanInstance);
	if (result != VK_SUCCESS)
	{
		strncpy(_vulkan_vector.cmp_info.disabled_reason,
			"Failed to create Vulkan instance.",
			PAPI_MAX_STR_LEN);
		return PAPI_ENOSUPP;
	}

	VULKAN_LOAD_INSTANCE_FUNC(vkDestroyInstance);
	VULKAN_LOAD_INSTANCE_FUNC(vkEnumeratePhysicalDevices);
	VULKAN_LOAD_INSTANCE_FUNC(vkGetPhysicalDeviceProperties);
	VULKAN_LOAD_INSTANCE_FUNC(vkGetPhysicalDeviceQueueFamilyProperties);
	VULKAN_LOAD_INSTANCE_FUNC(vkGetPhysicalDeviceFeatures);
	VULKAN_LOAD_INSTANCE_FUNC(vkCreateDevice);
	VULKAN_LOAD_INSTANCE_FUNC(vkDestroyDevice);
	VULKAN_LOAD_INSTANCE_FUNC(vkDeviceWaitIdle);
	VULKAN_LOAD_INSTANCE_FUNC(vkGetDeviceQueue);
	VULKAN_LOAD_INSTANCE_FUNC(vkCreateQueryPool);
	VULKAN_LOAD_INSTANCE_FUNC(vkDestroyQueryPool);
	VULKAN_LOAD_INSTANCE_FUNC(vkGetQueryPoolResults);

	VULKAN_LOAD_OPTIONAL_INSTANCE_FUNC(vkResetQueryPool);

	VULKAN_LOAD_OPTIONAL_INSTANCE_FUNC(
		vkEnumeratePhysicalDeviceQueueFamilyPerformanceQueryCountersKHR);
	VULKAN_LOAD_OPTIONAL_INSTANCE_FUNC(
		vkGetPhysicalDeviceQueueFamilyPerformanceQueryPassesKHR);
	VULKAN_LOAD_OPTIONAL_INSTANCE_FUNC(vkAcquireProfilingLockKHR);
	VULKAN_LOAD_OPTIONAL_INSTANCE_FUNC(vkReleaseProfilingLockKHR);

	if (!vkEnumeratePhysicalDeviceQueueFamilyPerformanceQueryCountersKHRPtr)
	{
		strncpy(_vulkan_vector.cmp_info.disabled_reason,
			"VK_KHR_performance_query extension not supported.",
			PAPI_MAX_STR_LEN);
		return PAPI_ENOSUPP;
	}

	return PAPI_OK;
}

int
VULKAN_init_control_state(hwd_control_state_t *ctrl)
{
	VULKAN_control_state_t *vk_ctrl = (VULKAN_control_state_t *)ctrl;

	vk_ctrl->queryPool = VK_NULL_HANDLE;
	vk_ctrl->device = VK_NULL_HANDLE;
	vk_ctrl->ncounters = 0;
	vk_ctrl->profilingLockAcquired = 0;
	vk_ctrl->queueFamilyIndex = 0;

	memset(vk_ctrl->counts, 0, sizeof(vk_ctrl->counts));

	vk_ctrl->addedCounters.list = (int *)malloc(sizeof(int) * NUM_EVENTS);
	if (!vk_ctrl->addedCounters.list)
		return PAPI_ENOMEM;

	for (int i = 0; i < NUM_EVENTS; i++)
		vk_ctrl->addedCounters.list[i] = 0;

	return PAPI_OK;
}

int
VULKAN_start(hwd_context_t *ctx, hwd_control_state_t *ctrl)
{
	(void)ctx;
	VULKAN_control_state_t *vk_ctrl = (VULKAN_control_state_t *)ctrl;

	for (int i = 0; i < VULKAN_MAX_COUNTERS; i++)
		vk_ctrl->counts[i] = 0;

	if (vk_ctrl->queryPool == VK_NULL_HANDLE || vk_ctrl->device == VK_NULL_HANDLE)
		return PAPI_ECNFLCT;

	if (vkResetQueryPoolPtr)
		(*vkResetQueryPoolPtr)(vk_ctrl->device, vk_ctrl->queryPool, 0, 1);

	if (vkAcquireProfilingLockKHRPtr)
	{
		VkAcquireProfilingLockInfoKHR lockInfo;
		memset(&lockInfo, 0, sizeof(lockInfo));
		lockInfo.sType = VK_STRUCTURE_TYPE_ACQUIRE_PROFILING_LOCK_INFO_KHR;
		lockInfo.timeout = 1000000000ULL;

		VkResult result = (*vkAcquireProfilingLockKHRPtr)(vk_ctrl->device, &lockInfo);
		if (result == VK_SUCCESS)
			vk_ctrl->profilingLockAcquired = 1;
	}

	return PAPI_OK;
}

int
VULKAN_stop(hwd_context_t *ctx, hwd_control_state_t *ctrl)
{
	(void)ctx;
	VULKAN_control_state_t *vk_ctrl = (VULKAN_control_state_t *)ctrl;

	getQueryResults(vk_ctrl);

	if (vk_ctrl->profilingLockAcquired && vkReleaseProfilingLockKHRPtr)
	{
		(*vkReleaseProfilingLockKHRPtr)(vk_ctrl->device);
		vk_ctrl->profilingLockAcquired = 0;
	}

	return PAPI_OK;
}

int
VULKAN_read(hwd_context_t *ctx, hwd_control_state_t *ctrl,
			long long **events, int flags)
{
	(void)ctx;
	(void)flags;
	VULKAN_control_state_t *vk_ctrl = (VULKAN_control_state_t *)ctrl;

	getQueryResults(vk_ctrl);

	*events = vk_ctrl->counts;
	return PAPI_OK;
}

int
VULKAN_shutdown_thread(hwd_context_t *ctx)
{
	VULKAN_context_t *vk_ctx = (VULKAN_context_t *)ctx;
	free(vk_ctx->state.addedCounters.list);
	return PAPI_OK;
}

int
VULKAN_shutdown_component(void)
{
	if (VULKAN_FREED == 0)
	{
		VULKAN_FREED = 1;

		for (int i = 0; i < vulkanDeviceCount; i++)
		{
			if (vulkanDevices[i].initialized &&
				vulkanDevices[i].device != VK_NULL_HANDLE)
			{
				(*vkDeviceWaitIdlePtr)(vulkanDevices[i].device);
				(*vkDestroyDevicePtr)(vulkanDevices[i].device, NULL);
			}

			for (uint32_t j = 0; j < vulkanDevices[i].queueFamilyCount; j++)
				free(vulkanDevices[i].queueFamilies[j].counters);

			free(vulkanDevices[i].queueFamilies);
		}

		free(vulkanPhysicalDevices);
		vulkanPhysicalDevices = NULL;

		papi_free(vulkan_native_table);
		vulkan_native_table = NULL;

		(*vkDestroyInstancePtr)(vulkanInstance, NULL);
	}

	if (vulkan_lib)
		dlclose(vulkan_lib);

	return PAPI_OK;
}

int
VULKAN_ctl(hwd_context_t *ctx, int code, _papi_int_option_t *option)
{
	(void)ctx;
	(void)code;
	(void)option;
	return PAPI_OK;
}

int
VULKAN_update_control_state(hwd_control_state_t *ptr,
							NativeInfo_t *native, int count,
							hwd_context_t *ctx)
{
	(void)ctx;
	VULKAN_control_state_t *vk_ptr = (VULKAN_control_state_t *)ptr;

	if (vk_ptr->queryPool != VK_NULL_HANDLE)
	{
		if (vk_ptr->device != VK_NULL_HANDLE)
			(*vkDestroyQueryPoolPtr)(vk_ptr->device, vk_ptr->queryPool, NULL);
		vk_ptr->queryPool = VK_NULL_HANDLE;
	}

	if (count == 0)
		return PAPI_OK;

	uint32_t devIdx = vulkan_native_table[native[0].ni_event].resources.deviceIndex;

	if (initializeDevice(devIdx) != PAPI_OK)
		return PAPI_ENOSUPP;

	vk_ptr->device = vulkanDevices[devIdx].device;
	vk_ptr->ncounters = count;
	vk_ptr->addedCounters.count = count;

	for (int i = 0; i < count; i++)
	{
		int index = native[i].ni_event;
		native[i].ni_position = index;
		vk_ptr->addedCounters.list[i] = index;
	}

	uint32_t qfIdx = vulkan_native_table[
		vk_ptr->addedCounters.list[0]].resources.queueFamilyIndex;
	vk_ptr->queueFamilyIndex = qfIdx;

	uint32_t *counterIndices = (uint32_t *)calloc(count, sizeof(uint32_t));
	if (!counterIndices)
		return PAPI_ENOMEM;

	for (int i = 0; i < count; i++)
	{
		int idx = vk_ptr->addedCounters.list[i];
		counterIndices[i] = vulkan_native_table[idx].resources.counterIndex;
	}

	VkQueryPoolPerformanceCreateInfoKHR perfQueryInfo;
	memset(&perfQueryInfo, 0, sizeof(perfQueryInfo));
	perfQueryInfo.sType = VK_STRUCTURE_TYPE_QUERY_POOL_PERFORMANCE_CREATE_INFO_KHR;
	perfQueryInfo.queueFamilyIndex = qfIdx;
	perfQueryInfo.counterIndexCount = count;
	perfQueryInfo.pCounterIndices = counterIndices;

	VkQueryPoolCreateInfo poolInfo;
	memset(&poolInfo, 0, sizeof(poolInfo));
	poolInfo.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
	poolInfo.pNext = &perfQueryInfo;
	poolInfo.queryType = VK_QUERY_TYPE_PERFORMANCE_QUERY_KHR;
	poolInfo.queryCount = 1;

	VkResult result = (*vkCreateQueryPoolPtr)(vk_ptr->device, &poolInfo, NULL,
		&vk_ptr->queryPool);
	free(counterIndices);

	if (result != VK_SUCCESS)
		return PAPI_ENOSUPP;

	return PAPI_OK;
}

int
VULKAN_set_domain(hwd_control_state_t *cntrl, int domain)
{
	int found = 0;
	(void)cntrl;

	if (PAPI_DOM_USER & domain)
		found = 1;
	if (PAPI_DOM_KERNEL & domain)
		found = 1;
	if (PAPI_DOM_OTHER & domain)
		found = 1;
	if (!found)
		return PAPI_EINVAL;

	return PAPI_OK;
}

int
VULKAN_reset(hwd_context_t *ctx, hwd_control_state_t *ctrl)
{
	(void)ctx;
	VULKAN_control_state_t *vk_ctrl = (VULKAN_control_state_t *)ctrl;

	for (int i = 0; i < VULKAN_MAX_COUNTERS; i++)
		vk_ctrl->counts[i] = 0;

	if (vk_ctrl->queryPool != VK_NULL_HANDLE && vk_ctrl->device != VK_NULL_HANDLE)
	{
		if (vkResetQueryPoolPtr)
			(*vkResetQueryPoolPtr)(vk_ctrl->device, vk_ctrl->queryPool, 0, 1);
	}

	return PAPI_OK;
}

int
VULKAN_cleanup_eventset(hwd_control_state_t *ctrl)
{
	(void)ctrl;
	return PAPI_OK;
}

int
VULKAN_ntv_enum_events(unsigned int *EventCode, int modifier)
{
	switch (modifier)
	{
	case PAPI_ENUM_FIRST:
		*EventCode = 0;
		return PAPI_OK;
	case PAPI_ENUM_EVENTS:
	{
		int index = *EventCode;
		if (index < NUM_EVENTS - 1)
		{
			*EventCode = *EventCode + 1;
			return PAPI_OK;
		}
		return PAPI_ENOEVNT;
	}
	default:
		return PAPI_EINVAL;
	}
}

int
VULKAN_ntv_code_to_name(unsigned int EventCode, char *name, int len)
{
	int index = EventCode;
	if (index < 0 || index >= NUM_EVENTS)
		return PAPI_ENOEVNT;
	strncpy(name, vulkan_native_table[index].name, len);
	return PAPI_OK;
}

int
VULKAN_ntv_code_to_descr(unsigned int EventCode, char *descr, int len)
{
	int index = EventCode;
	if (index < 0 || index >= NUM_EVENTS)
		return PAPI_ENOEVNT;
	strncpy(descr, vulkan_native_table[index].description, len);
	return PAPI_OK;
}

int
VULKAN_ntv_code_to_bits(unsigned int EventCode, hwd_register_t *bits)
{
	int index = EventCode;
	if (index < 0 || index >= NUM_EVENTS)
		return PAPI_ENOEVNT;
	memcpy((VULKAN_register_t *)bits,
		   &(vulkan_native_table[index].resources),
		   sizeof(VULKAN_register_t));
	return PAPI_OK;
}

papi_vector_t _vulkan_vector = {
	.cmp_info = {
		.name = "vulkan",
		.short_name = "vulkan",
		.version = "1.0",
		.description =
			"Vulkan performance counters via VK_KHR_performance_query",
		.num_mpx_cntrs = VULKAN_MAX_COUNTERS,
		.num_cntrs = VULKAN_MAX_COUNTERS,
		.default_domain = PAPI_DOM_USER,
		.default_granularity = PAPI_GRN_THR,
		.available_granularities = PAPI_GRN_THR,
		.hardware_intr_sig = PAPI_INT_SIGNAL,
		.fast_real_timer = 0,
		.fast_virtual_timer = 0,
		.attach = 0,
		.attach_must_ptrace = 0,
		.available_domains = PAPI_DOM_USER | PAPI_DOM_KERNEL,
	},

	.size = {
		.context = sizeof(VULKAN_context_t),
		.control_state = sizeof(VULKAN_control_state_t),
		.reg_value = sizeof(VULKAN_register_t),
		.reg_alloc = sizeof(VULKAN_reg_alloc_t),
	},

	.init_thread = VULKAN_init_thread,
	.init_component = VULKAN_init_component,
	.init_control_state = VULKAN_init_control_state,
	.start = VULKAN_start,
	.stop = VULKAN_stop,
	.read = VULKAN_read,
	.shutdown_component = VULKAN_shutdown_component,
	.shutdown_thread = VULKAN_shutdown_thread,
	.cleanup_eventset = VULKAN_cleanup_eventset,
	.ctl = VULKAN_ctl,
	.update_control_state = VULKAN_update_control_state,
	.set_domain = VULKAN_set_domain,
	.reset = VULKAN_reset,

	.ntv_enum_events = VULKAN_ntv_enum_events,
	.ntv_code_to_name = VULKAN_ntv_code_to_name,
	.ntv_code_to_descr = VULKAN_ntv_code_to_descr,
	.ntv_code_to_bits = VULKAN_ntv_code_to_bits,
};
