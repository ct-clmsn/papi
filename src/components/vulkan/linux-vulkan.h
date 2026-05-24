#ifndef _PAPI_VULKAN_H
#define _PAPI_VULKAN_H

#include <vulkan/vulkan.h>

#define VULKAN_MAX_COUNTERS 512
#define VULKAN_MAX_DEVICES 16
#define VULKAN_MAX_QUEUE_FAMILIES 16
#define VULKAN_MAX_PHYSICAL_DEVICES 16

typedef struct VULKAN_counter_data
{
	uint32_t counterIndex;
	char name[PAPI_MAX_STR_LEN];
	char category[PAPI_MAX_STR_LEN];
	char description[PAPI_2MAX_STR_LEN];
	int storageType;
} VULKAN_counter_data_t;

typedef struct VULKAN_queue_family_data
{
	uint32_t queueFamilyIndex;
	uint32_t counterCount;
	VULKAN_counter_data_t *counters;
	char name[PAPI_MIN_STR_LEN];
} VULKAN_queue_family_data_t;

typedef struct VULKAN_device_data
{
	VkPhysicalDevice physicalDevice;
	VkDevice device;
	char name[PAPI_MIN_STR_LEN];
	uint32_t queueFamilyCount;
	VULKAN_queue_family_data_t *queueFamilies;
	int initialized;
} VULKAN_device_data_t;

typedef struct VULKAN_register
{
	unsigned int selector;
	uint32_t deviceIndex;
	uint32_t queueFamilyIndex;
	uint32_t counterIndex;
} VULKAN_register_t;

typedef struct VULKAN_native_event_entry
{
	VULKAN_register_t resources;
	char name[PAPI_MAX_STR_LEN];
	char description[PAPI_2MAX_STR_LEN];
} VULKAN_native_event_entry_t;

typedef struct VULKAN_reg_alloc
{
	VULKAN_register_t ra_bits;
} VULKAN_reg_alloc_t;

typedef struct VULKAN_added_counters
{
	int count;
	int *list;
} VULKAN_added_counters_t;

typedef struct VULKAN_control_state
{
	VkQueryPool queryPool;
	VkDevice device;
	VULKAN_added_counters_t addedCounters;
	long long counts[VULKAN_MAX_COUNTERS];
	int ncounters;
	int profilingLockAcquired;
	uint32_t queueFamilyIndex;
} VULKAN_control_state_t;

typedef struct VULKAN_context
{
	VULKAN_control_state_t state;
} VULKAN_context_t;

static int detectVulkan(void);
static int queueFamilyTypeString(VkQueueFlags flags, char *str, int len);

static VULKAN_native_event_entry_t *vulkan_native_table;
static int NUM_EVENTS = 0;
static VULKAN_device_data_t vulkanDevices[VULKAN_MAX_PHYSICAL_DEVICES];
static int vulkanDeviceCount = 0;
static int VULKAN_FREED = 0;

static VkInstance vulkanInstance;
static VkPhysicalDevice *vulkanPhysicalDevices;

#endif
