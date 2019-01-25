// Copyright 2018 The Android Open Source Project
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either expresso or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
#include "VkDecoderGlobalState.h"

#include "VkAndroidNativeBuffer.h"
#include "VkFormatUtils.h"
#include "VulkanDispatch.h"

#include "android/base/containers/Lookup.h"
#include "android/base/memory/LazyInstance.h"
#include "android/base/Optional.h"
#include "android/base/synchronization/Lock.h"

#include "common/goldfish_vk_deepcopy.h"

#include "emugl/common/crash_reporter.h"
#include "emugl/common/feature_control.h"
#include "emugl/common/vm_operations.h"

#include "GLcommon/etc.h"

#include <algorithm>
#include <functional>
#include <unordered_map>
#include <vector>

using android::base::AutoLock;
using android::base::LazyInstance;
using android::base::Lock;
using android::base::Optional;

namespace goldfish_vk {

static constexpr const char* const
kEmulatedExtensions[] = {
    "VK_ANDROID_native_buffer",
};

class VkDecoderGlobalState::Impl {
public:
    Impl() :
        m_vk(emugl::vkDispatch()),
        m_emu(createOrGetGlobalVkEmulation(m_vk)) { }
    ~Impl() = default;

    // A list of extensions that should not be passed to the host driver.
    // These will mainly include Vulkan features that we emulate ourselves.

    VkResult on_vkCreateInstance(
        const VkInstanceCreateInfo* pCreateInfo,
        const VkAllocationCallbacks* pAllocator,
        VkInstance* pInstance) {

        std::vector<const char*> finalExts =
            filteredExtensionNames(
                pCreateInfo->enabledExtensionCount,
                pCreateInfo->ppEnabledExtensionNames);

        VkInstanceCreateInfo createInfoFiltered = *pCreateInfo;
        createInfoFiltered.enabledExtensionCount = (uint32_t)finalExts.size();
        createInfoFiltered.ppEnabledExtensionNames = finalExts.data();

        return m_vk->vkCreateInstance(&createInfoFiltered, pAllocator, pInstance);
    }

    void on_vkGetPhysicalDeviceFeatures(VkPhysicalDevice physicalDevice,
                                        VkPhysicalDeviceFeatures* pFeatures) {
        m_vk->vkGetPhysicalDeviceFeatures(physicalDevice, pFeatures);
        pFeatures->textureCompressionETC2 = true;
    }

    void on_vkGetPhysicalDeviceProperties(
            VkPhysicalDevice physicalDevice,
            VkPhysicalDeviceProperties* pProperties) {
        m_vk->vkGetPhysicalDeviceProperties(
            physicalDevice, pProperties);

        static constexpr uint32_t kMaxSafeVersion = VK_MAKE_VERSION(1, 0, 65);

        if (pProperties->apiVersion > kMaxSafeVersion) {
            pProperties->apiVersion = kMaxSafeVersion;
        }
    }

    void on_vkGetPhysicalDeviceMemoryProperties(
        VkPhysicalDevice physicalDevice,
        VkPhysicalDeviceMemoryProperties* pMemoryProperties) {

        m_vk->vkGetPhysicalDeviceMemoryProperties(
            physicalDevice, pMemoryProperties);

        // Pick a max heap size that will work around
        // drivers that give bad suggestions (such as 0xFFFFFFFFFFFFFFFF for the heap size)
        // plus won't break the bank on 32-bit userspace.
        static constexpr VkDeviceSize kMaxSafeHeapSize =
            2ULL * 1024ULL * 1024ULL * 1024ULL;

        for (uint32_t i = 0; i < pMemoryProperties->memoryTypeCount; ++i) {
            uint32_t heapIndex = pMemoryProperties->memoryTypes[i].heapIndex;
            auto& heap = pMemoryProperties->memoryHeaps[heapIndex];

            if (heap.size > kMaxSafeHeapSize) {
                heap.size = kMaxSafeHeapSize;
            }

            if (!emugl::emugl_feature_is_enabled(
                    android::featurecontrol::GLDirectMem)) {
                pMemoryProperties->memoryTypes[i].propertyFlags =
                    pMemoryProperties->memoryTypes[i].propertyFlags &
                    ~(VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
            }
        }
    }

    VkResult on_vkCreateDevice(VkPhysicalDevice physicalDevice,
                           const VkDeviceCreateInfo* pCreateInfo,
                           const VkAllocationCallbacks* pAllocator,
                           VkDevice* pDevice) {

        std::vector<const char*> finalExts =
            filteredExtensionNames(
                pCreateInfo->enabledExtensionCount,
                pCreateInfo->ppEnabledExtensionNames);


        for (uint32_t i = 0; i < pCreateInfo->enabledExtensionCount; ++i) {
            auto extName =
                pCreateInfo->ppEnabledExtensionNames[i];
            if (!isEmulatedExtension(extName)) {
                finalExts.push_back(extName);
            }
        }

        // Run the underlying API call, filtering extensions.
        VkDeviceCreateInfo createInfoFiltered = *pCreateInfo;
        bool emulateTextureEtc2 = false;
        VkPhysicalDeviceFeatures featuresFiltered;

        if (pCreateInfo->pEnabledFeatures) {
            featuresFiltered = *pCreateInfo->pEnabledFeatures;
            if (featuresFiltered.textureCompressionETC2) {
                VkPhysicalDeviceFeatures physicalFeatures;
                m_vk->vkGetPhysicalDeviceFeatures(physicalDevice,
                                                &physicalFeatures);
                if (!physicalFeatures.textureCompressionETC2) {
                    emulateTextureEtc2 = true;
                    featuresFiltered.textureCompressionETC2 = false;
                }
            }
            createInfoFiltered.pEnabledFeatures = &featuresFiltered;
        }
        createInfoFiltered.enabledExtensionCount = (uint32_t)finalExts.size();
        createInfoFiltered.ppEnabledExtensionNames = finalExts.data();

        VkResult result =
            m_vk->vkCreateDevice(
                physicalDevice, &createInfoFiltered, pAllocator, pDevice);

        if (result != VK_SUCCESS) return result;

        AutoLock lock(mLock);

        mDeviceToPhysicalDevice[*pDevice] = physicalDevice;

        auto it = mPhysdevInfo.find(physicalDevice);

        // Populate physical device info for the first time.
        if (it == mPhysdevInfo.end()) {
            auto& physdevInfo = mPhysdevInfo[physicalDevice];

            VkPhysicalDeviceMemoryProperties props;
            m_vk->vkGetPhysicalDeviceMemoryProperties(physicalDevice, &props);
            physdevInfo.memoryProperties = props;

            uint32_t queueFamilyPropCount = 0;

            m_vk->vkGetPhysicalDeviceQueueFamilyProperties(
                    physicalDevice, &queueFamilyPropCount, nullptr);

            physdevInfo.queueFamilyProperties.resize((size_t)queueFamilyPropCount);

            m_vk->vkGetPhysicalDeviceQueueFamilyProperties(
                    physicalDevice, &queueFamilyPropCount,
                    physdevInfo.queueFamilyProperties.data());
        }

        // Fill out information about the logical device here.
        auto& deviceInfo = mDeviceInfo[*pDevice];
        deviceInfo.physicalDevice = physicalDevice;
        deviceInfo.emulateTextureEtc2 = emulateTextureEtc2;

        // First, get information about the queue families used by this device.
        std::unordered_map<uint32_t, uint32_t> queueFamilyIndexCounts;
        for (uint32_t i = 0; i < pCreateInfo->queueCreateInfoCount; ++i) {
            const auto& queueCreateInfo =
                pCreateInfo->pQueueCreateInfos[i];
            // Check only queues created with flags = 0 in VkDeviceQueueCreateInfo.
            auto flags = queueCreateInfo.flags;
            if (flags) continue;
            uint32_t queueFamilyIndex = queueCreateInfo.queueFamilyIndex;
            uint32_t queueCount = queueCreateInfo.queueCount;
            queueFamilyIndexCounts[queueFamilyIndex] = queueCount;
        }

        for (auto it : queueFamilyIndexCounts) {
            auto index = it.first;
            auto count = it.second;
            auto& queues = deviceInfo.queues[index];
            for (uint32_t i = 0; i < count; ++i) {
                VkQueue queueOut;
                m_vk->vkGetDeviceQueue(
                    *pDevice, index, i, &queueOut);
                queues.push_back(queueOut);
                mQueueInfo[queueOut].device = *pDevice;
                mQueueInfo[queueOut].queueFamilyIndex = index;
            }
        }

        return VK_SUCCESS;
    }

    void on_vkGetDeviceQueue(
        VkDevice device,
        uint32_t queueFamilyIndex,
        uint32_t queueIndex,
        VkQueue* pQueue) {

        AutoLock lock(mLock);

        *pQueue = VK_NULL_HANDLE;

        auto deviceInfo = android::base::find(mDeviceInfo, device);
        if (!deviceInfo) return;

        const auto& queues =
            deviceInfo->queues;

        const auto queueList =
            android::base::find(queues, queueFamilyIndex);

        if (!queueList) return;
        if (queueIndex >= queueList->size()) return;

        *pQueue = (*queueList)[queueIndex];
    }

    void on_vkDestroyDevice(
        VkDevice device,
        const VkAllocationCallbacks* pAllocator) {

        AutoLock lock(mLock);
        auto it = mDeviceInfo.find(device);
        if (it == mDeviceInfo.end()) return;

        auto eraseIt = mQueueInfo.begin();
        for(; eraseIt != mQueueInfo.end();) {
            if (eraseIt->second.device == device) {
                eraseIt = mQueueInfo.erase(eraseIt);
            } else {
                ++eraseIt;
            }
        }

        mDeviceInfo.erase(device);
        mDeviceToPhysicalDevice.erase(device);

        // Run the underlying API call.
        m_vk->vkDestroyDevice(device, pAllocator);
    }

    VkResult on_vkCreateBuffer(VkDevice device,
                               const VkBufferCreateInfo* pCreateInfo,
                               const VkAllocationCallbacks* pAllocator,
                               VkBuffer* pBuffer) {
        VkResult result =
                m_vk->vkCreateBuffer(device, pCreateInfo, pAllocator, pBuffer);
        if (result == VK_SUCCESS) {
            mBufferInfo.emplace(*pBuffer, BufferInfo());
            mBufferInfo[*pBuffer].device = device;
            mBufferInfo[*pBuffer].size = pCreateInfo->size;
        }
        return result;
    }

    VkResult on_vkBindBufferMemory(VkDevice device,
                                   VkBuffer buffer,
                                   VkDeviceMemory memory,
                                   VkDeviceSize memoryOffset) {
        VkResult result =
                m_vk->vkBindBufferMemory(device, buffer, memory, memoryOffset);
        if (result == VK_SUCCESS) {
            auto it = mBufferInfo.find(buffer);
            if (it == mBufferInfo.end()) {
                return result;
            }
            it->second.memory = memory;
            it->second.memoryOffset = memoryOffset;
        }
        return result;
    }

    VkResult on_vkCreateImage(
        VkDevice device,
        const VkImageCreateInfo* pCreateInfo,
        const VkAllocationCallbacks* pAllocator,
        VkImage* pImage) {

        CompressedImageInfo cmpInfo = createCompressedImageInfo(
            pCreateInfo->format
        );
        VkImageCreateInfo localInfo;
        if (cmpInfo.isCompressed) {
            localInfo = *pCreateInfo;
            localInfo.format = cmpInfo.dstFormat;
            pCreateInfo = &localInfo;

            cmpInfo.extent = pCreateInfo->extent;
            cmpInfo.mipLevels = pCreateInfo->mipLevels;
        }

        AndroidNativeBufferInfo anbInfo;
        bool isAndroidNativeBuffer =
            parseAndroidNativeBufferInfo(pCreateInfo, &anbInfo);

        VkResult createRes = VK_SUCCESS;

        if (isAndroidNativeBuffer) {
            AutoLock lock(mLock);

            auto memProps = memPropsOfDeviceLocked(device);

            createRes =
                prepareAndroidNativeBufferImage(
                    m_vk, device, pCreateInfo, pAllocator,
                    memProps, &anbInfo);
            if (createRes == VK_SUCCESS) {
                *pImage = anbInfo.image;
            }
        } else {
            createRes =
                m_vk->vkCreateImage(device, pCreateInfo, pAllocator, pImage);
        }

        if (createRes != VK_SUCCESS) return createRes;

        AutoLock lock(mLock);

        auto& imageInfo = mImageInfo[*pImage];
        imageInfo.anbInfo = anbInfo;
        imageInfo.cmpInfo = cmpInfo;

        return createRes;
    }

    void on_vkDestroyImage(
        VkDevice device,
        VkImage image,
        const VkAllocationCallbacks* pAllocator) {

        AutoLock lock(mLock);
        auto it = mImageInfo.find(image);

        if (it == mImageInfo.end()) return;

        auto info = it->second;

        if (info.anbInfo.image) {
            teardownAndroidNativeBufferImage(m_vk, &info.anbInfo);
        } else {
            if (info.cmpInfo.isCompressed) {
                for (const auto& buffer : info.cmpInfo.tmpBuffer) {
                    m_vk->vkDestroyBuffer(device, buffer, nullptr);
                }
                for (const auto& memory : info.cmpInfo.tmpMemory) {
                    m_vk->vkFreeMemory(device, memory, nullptr);
                }
            }
            m_vk->vkDestroyImage(device, image, pAllocator);
        }

        mImageInfo.erase(image);
    }

    VkResult on_vkCreateImageView(
        VkDevice device,
        const VkImageViewCreateInfo* pCreateInfo,
        const VkAllocationCallbacks* pAllocator,
        VkImageView* pView) {

        if (!pCreateInfo) {
            return VK_ERROR_OUT_OF_HOST_MEMORY;
        }
        CompressedImageInfo cmpInfo = createCompressedImageInfo(
            pCreateInfo->format
        );
        VkImageViewCreateInfo createInfo;
        if (cmpInfo.isCompressed) {
            createInfo = *pCreateInfo;
            createInfo.format = cmpInfo.dstFormat;
            pCreateInfo = &createInfo;
        }
        return m_vk->vkCreateImageView(device, pCreateInfo, pAllocator, pView);
    }

    void on_vkGetImageMemoryRequirements(
        VkDevice device,
        VkImage image,
        VkMemoryRequirements* pMemoryRequirements) {
        m_vk->vkGetImageMemoryRequirements(device, image, pMemoryRequirements);
    }

    void on_vkCmdCopyBufferToImage(
        VkCommandBuffer commandBuffer,
        VkBuffer srcBuffer,
        VkImage dstImage,
        VkImageLayout dstImageLayout,
        uint32_t regionCount,
        const VkBufferImageCopy* pRegions) {
        AutoLock lock(mLock);
        auto it = mImageInfo.find(dstImage);
        if (it == mImageInfo.end()) return;
        auto bufferInfoIt = mBufferInfo.find(srcBuffer);
        if (bufferInfoIt == mBufferInfo.end()) {
            return;
        }
        VkDevice device = bufferInfoIt->second.device;
        auto deviceInfoIt = mDeviceInfo.find(device);
        if (deviceInfoIt == mDeviceInfo.end()) {
            return;
        }
        if (!it->second.cmpInfo.isCompressed ||
            !deviceInfoIt->second.emulateTextureEtc2) {
            m_vk->vkCmdCopyBufferToImage(commandBuffer, srcBuffer, dstImage,
                    dstImageLayout, regionCount, pRegions);
            return;
        }
        auto cmdBufferInfoIt = mCmdBufferInfo.find(commandBuffer);
        if (cmdBufferInfoIt == mCmdBufferInfo.end()) {
            return;
        }
        CompressedImageInfo& cmp = it->second.cmpInfo;
        // regions and buffers for the decompressed data
        std::vector<VkBufferImageCopy> regions(regionCount);
        VkDeviceSize offset = 0;
        VkDeviceSize maxOffset = 0;
        const VkDeviceSize pixelSize = cmp.pixelSize();
        for (uint32_t r = 0; r < regionCount; r++) {
            VkBufferImageCopy& dstRegion = regions[r];
            dstRegion = pRegions[r];
            dstRegion.bufferOffset = offset;
            uint32_t width =
                    cmp.mipmapWidth(dstRegion.imageSubresource.mipLevel);
            uint32_t height =
                    cmp.mipmapHeight(dstRegion.imageSubresource.mipLevel);
            dstRegion.imageExtent.width =
                    std::min(dstRegion.imageExtent.width, width);
            dstRegion.imageExtent.height =
                    std::min(dstRegion.imageExtent.height, height);
            offset += dstRegion.imageExtent.width *
                      dstRegion.imageExtent.height * pixelSize;
        }

        VkBufferCreateInfo bufferInfo = {};
        bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bufferInfo.size = offset;
        bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

        VkBuffer buffer;
        if (m_vk->vkCreateBuffer(device, &bufferInfo, nullptr, &buffer) != VK_SUCCESS) {
            fprintf(stderr, "create buffer failed!\n");
            return;
        }
        cmp.tmpBuffer.push_back(buffer);

        VkMemoryRequirements memRequirements;
        m_vk->vkGetBufferMemoryRequirements(device, buffer, &memRequirements);

        VkPhysicalDevice physicalDevice = mDeviceInfo[device].physicalDevice;
        VkPhysicalDeviceMemoryProperties memProperties;
        m_vk->vkGetPhysicalDeviceMemoryProperties(physicalDevice, &memProperties);
        uint32_t memIdx = 0;
        bool foundMemIdx = false;
        VkMemoryPropertyFlags properties = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
        for (uint32_t i = 0; i < memProperties.memoryTypeCount; i++) {
            if ((memRequirements.memoryTypeBits & (1 << i)) &&
                    (memProperties.memoryTypes[i].propertyFlags & properties)
                    == properties) {
                memIdx = i;
                foundMemIdx = true;
            }
        }
        if (!foundMemIdx) {
            fprintf(stderr, "Error: cannot find memory property!\n");
            return;
        }

        VkMemoryAllocateInfo allocInfo = {};
        allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocInfo.allocationSize = memRequirements.size;
        allocInfo.memoryTypeIndex = memIdx;

        VkDeviceMemory memory;

        if (m_vk->vkAllocateMemory(device, &allocInfo, nullptr, &memory) != VK_SUCCESS) {
            fprintf(stderr, "failed to allocate vertex buffer memory!");
            return;
        }
        cmp.tmpMemory.push_back(memory);

        m_vk->vkBindBufferMemory(device, buffer, memory, 0);
        cmdBufferInfoIt->second.preprocessFuncs.push_back(
                [this, cmp, srcBuffer, memory, regions,
                 srcRegions = std::vector<VkBufferImageCopy>(
                         pRegions, pRegions + regionCount)]() {
                    decompressBufferToImage(cmp, srcBuffer, memory,
                                            srcRegions.size(),
                                            srcRegions.data(), regions.data());
                });

        m_vk->vkCmdCopyBufferToImage(commandBuffer, buffer, dstImage, dstImageLayout, regionCount,
                regions.data());
    }

    VkResult on_vkAllocateMemory(
        VkDevice device,
        const VkMemoryAllocateInfo* pAllocateInfo,
        const VkAllocationCallbacks* pAllocator,
        VkDeviceMemory* pMemory) {

        VkResult result =
            m_vk->vkAllocateMemory(device, pAllocateInfo, pAllocator, pMemory);

        if (result != VK_SUCCESS) {
            return result;
        }

        AutoLock lock(mLock);

        auto physdev = android::base::find(mDeviceToPhysicalDevice, device);

        if (!physdev) {
            // User app gave an invalid VkDevice,
            // but we don't really want to crash here.
            // We should allow invalid apps.
            return VK_ERROR_DEVICE_LOST;
        }

        auto physdevInfo =
                android::base::find(mPhysdevInfo, *physdev);

        if (!physdevInfo) {
            // If this fails, we crash, as we assume that the memory properties
            // map should have the info.
            emugl::emugl_crash_reporter(
                    "FATAL: Could not get memory properties for "
                    "VkPhysicalDevice");
        }

        // If the memory was allocated with a type index that corresponds
        // to a memory type that is host visible, let's also map the entire
        // thing.

        // First, check validity of the user's type index.
        if (pAllocateInfo->memoryTypeIndex >=
            physdevInfo->memoryProperties.memoryTypeCount) {
            // Continue allowing invalid behavior.
            return VK_ERROR_INCOMPATIBLE_DRIVER;
        }

        mMapInfo[*pMemory] = MappedMemoryInfo();
        auto& mapInfo = mMapInfo[*pMemory];
        mapInfo.size = pAllocateInfo->allocationSize;

        VkMemoryPropertyFlags flags =
                physdevInfo->
                    memoryProperties
                        .memoryTypes[pAllocateInfo->memoryTypeIndex]
                        .propertyFlags;

        bool hostVisible =
            flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
        bool shouldMapIndirect =
            hostVisible &&
            !emugl::emugl_feature_is_enabled(android::featurecontrol::GLDirectMem);

        if (!shouldMapIndirect) return result;

        VkResult mapResult =
            m_vk->vkMapMemory(device, *pMemory, 0,
                              mapInfo.size, 0, &mapInfo.ptr);


        if (mapResult != VK_SUCCESS) {
            return VK_ERROR_OUT_OF_HOST_MEMORY;
        }

        return result;
    }

    void on_vkFreeMemory(
        VkDevice device,
        VkDeviceMemory memory,
        const VkAllocationCallbacks* pAllocator) {

        AutoLock lock(mLock);

        auto info = android::base::find(mMapInfo, memory);

        if (!info) {
            // Invalid usage.
            return;
        }

        if (info->directMapped) {
            get_emugl_vm_operations().unmapUserBackedRam(
                info->guestPhysAddr,
                info->sizeToPage);
        }

        if (info->ptr) {
            m_vk->vkUnmapMemory(device, memory);
        }

        mMapInfo.erase(memory);

        m_vk->vkFreeMemory(device, memory, pAllocator);
    }

    VkResult on_vkMapMemory(VkDevice device,
                            VkDeviceMemory memory,
                            VkDeviceSize offset,
                            VkDeviceSize size,
                            VkMemoryMapFlags flags,
                            void** ppData) {
        AutoLock lock(mLock);

        auto info = android::base::find(mMapInfo, memory);

        if (!info) {
            // Invalid usage.
            return VK_ERROR_MEMORY_MAP_FAILED;
        }

        if (!info->ptr) {
            return VK_ERROR_MEMORY_MAP_FAILED;
        }

        *ppData = (void*)((uint8_t*)info->ptr + offset);

        return VK_SUCCESS;
    }

    void on_vkUnmapMemory(VkDevice device, VkDeviceMemory memory) {
        // no-op; user-level mapping does not correspond
        // to any operation here.
    }

    uint8_t* getMappedHostPointer(VkDeviceMemory memory) {
        AutoLock lock(mLock);

        auto info = android::base::find(mMapInfo, memory);

        if (!info) {
            // Invalid usage.
            return nullptr;
        }

        return (uint8_t*)(info->ptr);
    }

    VkDeviceSize getDeviceMemorySize(VkDeviceMemory memory) {
        AutoLock lock(mLock);

        auto info = android::base::find(mMapInfo, memory);

        if (!info) {
            // Invalid usage.
            return 0;
        }

        return info->size;
    }

    bool usingDirectMapping() const {
        return emugl::emugl_feature_is_enabled(
                android::featurecontrol::GLDirectMem);
    }

    // VK_ANDROID_native_buffer
    VkResult on_vkGetSwapchainGrallocUsageANDROID(
        VkDevice device,
        VkFormat format,
        VkImageUsageFlags imageUsage,
        int* grallocUsage) {
        getGralloc0Usage(format, imageUsage, grallocUsage);
        return VK_SUCCESS;
    }

    VkResult on_vkGetSwapchainGrallocUsage2ANDROID(
        VkDevice device,
        VkFormat format,
        VkImageUsageFlags imageUsage,
        VkSwapchainImageUsageFlagsANDROID swapchainImageUsage,
        uint64_t* grallocConsumerUsage,
        uint64_t* grallocProducerUsage) {
        getGralloc1Usage(format, imageUsage, swapchainImageUsage,
                         grallocConsumerUsage,
                         grallocProducerUsage);
        return VK_SUCCESS;
    }

    VkResult on_vkAcquireImageANDROID(
        VkDevice device,
        VkImage image,
        int nativeFenceFd,
        VkSemaphore semaphore,
        VkFence fence) {

        AutoLock lock(mLock);

        auto imageInfo = android::base::find(mImageInfo, image);
        if (!imageInfo) {
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        VkQueue defaultQueue;
        uint32_t defaultQueueFamilyIndex;
        if (!getDefaultQueueForDeviceLocked(
                device, &defaultQueue, &defaultQueueFamilyIndex)) {
                    fprintf(stderr, "%s: cant get the default q\n", __func__);
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        AndroidNativeBufferInfo* anbInfo = &imageInfo->anbInfo;

        return
            setAndroidNativeImageSemaphoreSignaled(
                m_vk, device,
                defaultQueue, defaultQueueFamilyIndex,
                semaphore, fence, anbInfo);
    }

    VkResult on_vkQueueSignalReleaseImageANDROID(
        VkQueue queue,
        uint32_t waitSemaphoreCount,
        const VkSemaphore* pWaitSemaphores,
        VkImage image,
        int* pNativeFenceFd) {

        AutoLock lock(mLock);

        auto queueFamilyIndex = queueFamilyIndexOfQueueLocked(queue);

        if (!queueFamilyIndex) {
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        auto imageInfo = android::base::find(mImageInfo, image);
        AndroidNativeBufferInfo* anbInfo = &imageInfo->anbInfo;

        return
            syncImageToColorBuffer(
                m_vk,
                *queueFamilyIndex,
                queue,
                waitSemaphoreCount, pWaitSemaphores,
                pNativeFenceFd, anbInfo);
    }

    VkResult on_vkMapMemoryIntoAddressSpaceGOOGLE(
        VkDevice device, VkDeviceMemory memory, uint64_t* pAddress) {

        if (!emugl::emugl_feature_is_enabled(
                android::featurecontrol::GLDirectMem)) {
            emugl::emugl_crash_reporter(
                "FATAL: Tried to use direct mapping "
                "while GLDirectMem is not enabled!");
        }

        AutoLock lock(mLock);

        auto info = android::base::find(mMapInfo, memory);

        if (!info) {
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        m_vk->vkMapMemory(
            device, memory, 0,
            info->size, 0, &info->ptr);

        info->guestPhysAddr = *pAddress;

        constexpr size_t PAGE_BITS = 12;
        constexpr size_t PAGE_SIZE = 1u << PAGE_BITS;
        constexpr size_t PAGE_OFFSET_MASK = PAGE_SIZE - 1;

        uintptr_t addr = reinterpret_cast<uintptr_t>(info->ptr);
        uintptr_t pageOffset = addr & PAGE_OFFSET_MASK;

        info->pageAlignedHva =
            reinterpret_cast<void*>(addr - pageOffset);
        info->sizeToPage =
            ((info->size + pageOffset + PAGE_SIZE - 1) >>
                 PAGE_BITS) << PAGE_BITS;

        get_emugl_vm_operations().mapUserBackedRam(
            info->guestPhysAddr,
            info->pageAlignedHva,
            info->sizeToPage);

        info->directMapped = true;

        *pAddress = (uint64_t)(uintptr_t)info->ptr;

        return VK_SUCCESS;
    }

    VkResult on_vkAllocateCommandBuffers(
            VkDevice device,
            const VkCommandBufferAllocateInfo* pAllocateInfo,
            VkCommandBuffer* pCommandBuffers) {
        VkResult result = m_vk->vkAllocateCommandBuffers(device, pAllocateInfo,
                                                 pCommandBuffers);
        if (result != VK_SUCCESS) {
            return result;
        }
        AutoLock lock(mLock);
        mCmdBufferInfo.emplace(*pCommandBuffers, CommandBufferInfo());
        return result;
    }

    void on_vkCmdExecuteCommands(VkCommandBuffer commandBuffer,
                                 uint32_t commandBufferCount,
                                 const VkCommandBuffer* pCommandBuffers) {
        m_vk->vkCmdExecuteCommands(commandBuffer, commandBufferCount,
                                      pCommandBuffers);
        AutoLock lock(mLock);
        CommandBufferInfo& cmdBuffer = mCmdBufferInfo[commandBuffer];
        cmdBuffer.subCmds.insert(cmdBuffer.subCmds.end(),
                pCommandBuffers, pCommandBuffers + commandBufferCount);
    }

    VkResult on_vkQueueSubmit(VkQueue queue,
                          uint32_t submitCount,
                          const VkSubmitInfo* pSubmits,
                          VkFence fence) {
        for (uint32_t i = 0; i < submitCount; i++) {
            const VkSubmitInfo& submit = pSubmits[i];
            for (uint32_t c = 0; c < submit.commandBufferCount; c++) {
                executePreprocessRecursive(0, submit.pCommandBuffers[c]);
            }
        }
        return m_vk->vkQueueSubmit(queue, submitCount, pSubmits, fence);
    }

    VkResult on_vkResetCommandBuffer(VkCommandBuffer commandBuffer,
                                     VkCommandBufferResetFlags flags) {
        VkResult result = m_vk->vkResetCommandBuffer(commandBuffer, flags);
        if (VK_SUCCESS == result) {
            AutoLock lock(mLock);
            mCmdBufferInfo[commandBuffer] = {};
        }
        return result;
    }

    void on_vkFreeCommandBuffers(VkDevice device,
                                 VkCommandPool commandPool,
                                 uint32_t commandBufferCount,
                                 const VkCommandBuffer* pCommandBuffers) {
        AutoLock lock(mLock);
        for (uint32_t i = 0; i < commandBufferCount; i++) {
            mCmdBufferInfo.erase(pCommandBuffers[i]);
        }
        m_vk->vkFreeCommandBuffers(device, commandPool, commandBufferCount,
                                   pCommandBuffers);
    }

private:
    bool isEmulatedExtension(const char* name) const {
        for (auto emulatedExt : kEmulatedExtensions) {
            if (!strcmp(emulatedExt, name)) return true;
        }
        return false;
    }

    std::vector<const char*>
    filteredExtensionNames(
            uint32_t count, const char* const* extNames) {
        std::vector<const char*> res;
        for (uint32_t i = 0; i < count; ++i) {
            auto extName = extNames[i];
            if (!isEmulatedExtension(extName)) {
                res.push_back(extName);
            }
        }
        return res;
    }

    VkPhysicalDeviceMemoryProperties* memPropsOfDeviceLocked(VkDevice device) {
        auto physdev = android::base::find(mDeviceToPhysicalDevice, device);
        if (!physdev) return nullptr;

        auto physdevInfo = android::base::find(mPhysdevInfo, *physdev);
        if (!physdevInfo) return nullptr;

        return &physdevInfo->memoryProperties;
    }

    Optional<uint32_t> queueFamilyIndexOfQueueLocked(VkQueue queue) {
        auto info = android::base::find(mQueueInfo, queue);
        if (!info) return {};

        return info->queueFamilyIndex;
    }

    bool getDefaultQueueForDeviceLocked(
        VkDevice device, VkQueue* queue, uint32_t* queueFamilyIndex) {

        auto deviceInfo = android::base::find(mDeviceInfo, device);
        if (!deviceInfo) return false;

        auto zeroIt = deviceInfo->queues.find(0);
        if (zeroIt == deviceInfo->queues.end() ||
            zeroIt->second.size() == 0) {
            // Get the first queue / queueFamilyIndex
            // that does show up.
            for (auto it : deviceInfo->queues) {
                auto index = it.first;
                for (auto deviceQueue : it.second) {
                    *queue = deviceQueue;
                    *queueFamilyIndex = index;
                    return true;
                }
            }
            // Didn't find anything, fail.
            return false;
        } else {
            // Use queue family index 0.
            *queue = zeroIt->second[0];
            *queueFamilyIndex = 0;
            return true;
        }

        return false;
    }

    struct CompressedImageInfo {
        bool isCompressed = false;
        VkFormat srcFormat;
        VkFormat dstFormat = VK_FORMAT_R8G8B8A8_UNORM;
        std::vector<VkBuffer> tmpBuffer = {};
        std::vector<VkDeviceMemory> tmpMemory = {};
        VkExtent3D extent;
        uint32_t mipLevels = 1;
        uint32_t mipmapWidth(uint32_t level) {
            return std::max<uint32_t>(extent.width >> level, 1);
        }
        uint32_t mipmapHeight(uint32_t level) {
            return std::max<uint32_t>(extent.height >> level, 1);
        }
        uint32_t alignSize(uint32_t inputSize) {
            if (isCompressed) {
                return (inputSize + 3) & (~0x3);
            } else {
                return inputSize;
            }
        }
        VkDeviceSize pixelSize() {
            return getLinearFormatPixelSize(dstFormat);
        }
    };

    static ETC2ImageFormat getEtc2Format(VkFormat fmt) {
        switch (fmt) {
            case VK_FORMAT_ETC2_R8G8B8_UNORM_BLOCK:
            case VK_FORMAT_ETC2_R8G8B8_SRGB_BLOCK:
                return EtcRGB8;
            case VK_FORMAT_ETC2_R8G8B8A8_UNORM_BLOCK:
            case VK_FORMAT_ETC2_R8G8B8A8_SRGB_BLOCK:
                return EtcRGBA8;
            case VK_FORMAT_ETC2_R8G8B8A1_UNORM_BLOCK:
            case VK_FORMAT_ETC2_R8G8B8A1_SRGB_BLOCK:
                return EtcRGB8A1;
            case VK_FORMAT_EAC_R11_UNORM_BLOCK:
                return EtcR11;
            case VK_FORMAT_EAC_R11_SNORM_BLOCK:
                return EtcSignedR11;
            case VK_FORMAT_EAC_R11G11_UNORM_BLOCK:
                return EtcRG11;
            case VK_FORMAT_EAC_R11G11_SNORM_BLOCK:
                return EtcSignedRG11;
            default:
                fprintf(stderr,
                        "TODO: unsupported compressed texture format %d\n",
                        fmt);
                return EtcRGB8;
        }
    }

    CompressedImageInfo createCompressedImageInfo(VkFormat srcFmt) {
        CompressedImageInfo cmpInfo;
        cmpInfo.srcFormat = srcFmt;
        cmpInfo.isCompressed = true;
        switch (srcFmt) {
            case VK_FORMAT_ETC2_R8G8B8_UNORM_BLOCK:
                cmpInfo.dstFormat = VK_FORMAT_R8G8B8A8_UNORM;
                break;
            case VK_FORMAT_ETC2_R8G8B8_SRGB_BLOCK:
                cmpInfo.dstFormat = VK_FORMAT_R8G8B8A8_SRGB;
                break;
            case VK_FORMAT_ETC2_R8G8B8A1_UNORM_BLOCK:
            case VK_FORMAT_ETC2_R8G8B8A8_UNORM_BLOCK:
                cmpInfo.dstFormat = VK_FORMAT_R8G8B8A8_UNORM;
                break;
            case VK_FORMAT_ETC2_R8G8B8A1_SRGB_BLOCK:
            case VK_FORMAT_ETC2_R8G8B8A8_SRGB_BLOCK:
                cmpInfo.dstFormat = VK_FORMAT_R8G8B8A8_SRGB;
                break;
            default:
                cmpInfo.isCompressed = false;
                cmpInfo.dstFormat = srcFmt;
                break;
        }
        if (cmpInfo.isCompressed) {
            fprintf(stderr, "WARNING: compressed texutre is not yet suppoted, "
                "rendering could be wrong.\n");
        }
        return cmpInfo;
    }

    void decompressBufferToImage(CompressedImageInfo cmp,
                                 VkBuffer srcBuffer,
                                 VkDeviceMemory dstMemory,
                                 uint32_t regionCount,
                                 const VkBufferImageCopy* srcRegions,
                                 const VkBufferImageCopy* dstRegions) {
        ETC2ImageFormat imgFmt = getEtc2Format(cmp.srcFormat);
        // regions and buffers for the decompressed data
        int decodedPixelSize = etc_get_decoded_pixel_size(imgFmt);

        auto srcBufferInfoIt = mBufferInfo.find(srcBuffer);
        if (srcBufferInfoIt == mBufferInfo.end()) {
            return;
        }
        const auto& srcBufferInfo = srcBufferInfoIt->second;
        VkDevice device = srcBufferInfo.device;
        uint8_t* srcRawData;
        uint8_t* dstRawData;
        if (VK_SUCCESS !=
            m_vk->vkMapMemory(device, srcBufferInfo.memory,
                              srcBufferInfo.memoryOffset,
                              srcBufferInfo.size, 0,
                              (void**)&srcRawData)) {
            fprintf(stderr,
                    "WARNING: map memory failure. Textures might be broken\n");
        }
        m_vk->vkMapMemory(device, dstMemory, 0, VK_WHOLE_SIZE, 0,
                          (void**)&dstRawData);

        // The software decompression work
        std::vector<uint8_t> decompBuffer;
        for (uint32_t r = 0; r < regionCount; r++) {
            const VkBufferImageCopy& dstRegion = dstRegions[r];
            const VkBufferImageCopy& srcRegion = srcRegions[r];

            const uint8_t* srcPtr = srcRawData + srcRegion.bufferOffset;
            uint8_t* dstPtr = dstRawData + dstRegion.bufferOffset;
            VkExtent3D alignedSrcImgExtent;
            alignedSrcImgExtent.width =
                    cmp.alignSize(srcRegion.imageExtent.width);
            alignedSrcImgExtent.height =
                    cmp.alignSize(srcRegion.imageExtent.height);
            alignedSrcImgExtent.depth = 1;

            decompBuffer.resize(alignedSrcImgExtent.width *
                                alignedSrcImgExtent.height * decodedPixelSize);
            int err = etc2_decode_image(
                    srcPtr, imgFmt, decompBuffer.data(),
                    alignedSrcImgExtent.width, alignedSrcImgExtent.height,
                    decodedPixelSize * alignedSrcImgExtent.width);

            for (int h = 0; h < dstRegion.imageExtent.height; h++) {
                for (int w = 0; w < dstRegion.imageExtent.width; w++) {
                    // RGB to RGBA
                    const uint8_t* srcPixel =
                            decompBuffer.data() +
                            decodedPixelSize *
                                    (w + h * alignedSrcImgExtent.width);
                    uint8_t* dstPixel =
                            dstPtr + 4 * (w + h * dstRegion.imageExtent.width);
                    // In case the source is not an RGBA format, we set all
                    // channels to default values (except for R channel)
                    dstPixel[1] = 0;
                    dstPixel[2] = 0;
                    dstPixel[3] = 255;
                    memcpy(dstPixel, srcPixel, decodedPixelSize);
                }
            }
        }
        m_vk->vkUnmapMemory(device, srcBufferInfo.memory);
        m_vk->vkUnmapMemory(device, dstMemory);
    }

    void executePreprocessRecursive(int level, VkCommandBuffer cmdBuffer) {
        auto cmdBufferIt = mCmdBufferInfo.find(cmdBuffer);
        if (cmdBufferIt == mCmdBufferInfo.end()) {
            return;
        }
        for (const auto& func : cmdBufferIt->second.preprocessFuncs) {
            func();
        }
        // TODO: fix
        // for (const auto& subCmd : cmdBufferIt->second.subCmds) {
            // executePreprocessRecursive(level + 1, subCmd);
        // }
    }

    VulkanDispatch* m_vk;
    VkEmulation* m_emu;

    Lock mLock;

    // We always map the whole size on host.
    // This makes it much easier to implement
    // the memory map API.
    struct MappedMemoryInfo {
        // When ptr is null, it means the VkDeviceMemory object
        // was not allocated with the HOST_VISIBLE property.
        void* ptr = nullptr;
        VkDeviceSize size;
        // GLDirectMem info
        bool directMapped = false;
        uint64_t guestPhysAddr = 0;
        void* pageAlignedHva = nullptr;
        uint64_t sizeToPage = 0;
    };

    struct PhysicalDeviceInfo {
        VkPhysicalDeviceMemoryProperties memoryProperties;
        std::vector<VkQueueFamilyProperties> queueFamilyProperties;
    };

    struct DeviceInfo {
        std::unordered_map<uint32_t, std::vector<VkQueue>> queues;
        bool emulateTextureEtc2 = false;
        VkPhysicalDevice physicalDevice;
    };

    struct QueueInfo {
        VkDevice device;
        uint32_t queueFamilyIndex;
    };

    struct BufferInfo {
        VkDevice device;
        VkDeviceMemory memory = 0;
        VkDeviceSize memoryOffset = 0;
        VkDeviceSize size;
    };

    struct ImageInfo {
        AndroidNativeBufferInfo anbInfo;
        CompressedImageInfo cmpInfo;
    };

    typedef std::function<void()> PreprocessFunc;
    struct CommandBufferInfo {
        std::vector<PreprocessFunc> preprocessFuncs = {};
        std::vector<VkCommandBuffer> subCmds = {};
    };

    std::unordered_map<VkPhysicalDevice, PhysicalDeviceInfo>
        mPhysdevInfo;
    std::unordered_map<VkDevice, DeviceInfo>
        mDeviceInfo;
    std::unordered_map<VkImage, ImageInfo>
        mImageInfo;
    std::unordered_map<VkCommandBuffer, CommandBufferInfo> mCmdBufferInfo;
    // TODO: release CommandBufferInfo when a command pool is reset/released

    // Back-reference to the physical device associated with a particular
    // VkDevice, and the VkDevice corresponding to a VkQueue.
    std::unordered_map<VkDevice, VkPhysicalDevice> mDeviceToPhysicalDevice;
    std::unordered_map<VkQueue, QueueInfo> mQueueInfo;
    std::unordered_map<VkBuffer, BufferInfo> mBufferInfo;

    std::unordered_map<VkDeviceMemory, MappedMemoryInfo> mMapInfo;
};

VkDecoderGlobalState::VkDecoderGlobalState()
    : mImpl(new VkDecoderGlobalState::Impl()) {}

VkDecoderGlobalState::~VkDecoderGlobalState() = default;

static LazyInstance<VkDecoderGlobalState> sGlobalDecoderState =
        LAZY_INSTANCE_INIT;

// static
VkDecoderGlobalState* VkDecoderGlobalState::get() {
    return sGlobalDecoderState.ptr();
}

VkResult VkDecoderGlobalState::on_vkCreateInstance(
    const VkInstanceCreateInfo* pCreateInfo,
    const VkAllocationCallbacks* pAllocator,
    VkInstance* pInstance) {
    return mImpl->on_vkCreateInstance(pCreateInfo, pAllocator, pInstance);
}

void VkDecoderGlobalState::on_vkGetPhysicalDeviceFeatures(
        VkPhysicalDevice physicalDevice,
        VkPhysicalDeviceFeatures* pFeatures) {
    mImpl->on_vkGetPhysicalDeviceFeatures(physicalDevice, pFeatures);
}

void VkDecoderGlobalState::on_vkGetPhysicalDeviceProperties(
        VkPhysicalDevice physicalDevice,
        VkPhysicalDeviceProperties* pProperties) {
    mImpl->on_vkGetPhysicalDeviceProperties(physicalDevice, pProperties);
}

void VkDecoderGlobalState::on_vkGetPhysicalDeviceMemoryProperties(
    VkPhysicalDevice physicalDevice,
    VkPhysicalDeviceMemoryProperties* pMemoryProperties) {
    mImpl->on_vkGetPhysicalDeviceMemoryProperties(
        physicalDevice, pMemoryProperties);
}

VkResult VkDecoderGlobalState::on_vkCreateDevice(
        VkPhysicalDevice physicalDevice,
        const VkDeviceCreateInfo* pCreateInfo,
        const VkAllocationCallbacks* pAllocator,
        VkDevice* pDevice) {
    return mImpl->on_vkCreateDevice(physicalDevice, pCreateInfo, pAllocator, pDevice);
}

void VkDecoderGlobalState::on_vkGetDeviceQueue(
    VkDevice device,
    uint32_t queueFamilyIndex,
    uint32_t queueIndex,
    VkQueue* pQueue) {
    mImpl->on_vkGetDeviceQueue(device, queueFamilyIndex, queueIndex, pQueue);
}

void VkDecoderGlobalState::on_vkDestroyDevice(
    VkDevice device,
    const VkAllocationCallbacks* pAllocator) {
    mImpl->on_vkDestroyDevice(device, pAllocator);
}

VkResult VkDecoderGlobalState::on_vkCreateBuffer(
        VkDevice device,
        const VkBufferCreateInfo* pCreateInfo,
        const VkAllocationCallbacks* pAllocator,
        VkBuffer* pBuffer) {
    return mImpl->on_vkCreateBuffer(device, pCreateInfo, pAllocator, pBuffer);
}

VkResult VkDecoderGlobalState::on_vkBindBufferMemory(
        VkDevice device,
        VkBuffer buffer,
        VkDeviceMemory memory,
        VkDeviceSize memoryOffset) {
    return mImpl->on_vkBindBufferMemory(device, buffer, memory, memoryOffset);
}

VkResult VkDecoderGlobalState::on_vkCreateImage(
    VkDevice device,
    const VkImageCreateInfo* pCreateInfo,
    const VkAllocationCallbacks* pAllocator,
    VkImage* pImage) {
    return mImpl->on_vkCreateImage(device, pCreateInfo, pAllocator, pImage);
}

void VkDecoderGlobalState::on_vkDestroyImage(
    VkDevice device,
    VkImage image,
    const VkAllocationCallbacks* pAllocator) {
    mImpl->on_vkDestroyImage(device, image, pAllocator);
}

VkResult VkDecoderGlobalState::on_vkCreateImageView(
    VkDevice device,
    const VkImageViewCreateInfo* pCreateInfo,
    const VkAllocationCallbacks* pAllocator,
    VkImageView* pView) {
    return mImpl->on_vkCreateImageView(device, pCreateInfo, pAllocator, pView);
}

void VkDecoderGlobalState::on_vkGetImageMemoryRequirements(
    VkDevice device,
    VkImage image,
    VkMemoryRequirements* pMemoryRequirements) {
    mImpl->on_vkGetImageMemoryRequirements(device, image, pMemoryRequirements);
}

void VkDecoderGlobalState::on_vkCmdCopyBufferToImage(
        VkCommandBuffer commandBuffer,
        VkBuffer srcBuffer,
        VkImage dstImage,
        VkImageLayout dstImageLayout,
        uint32_t regionCount,
        const VkBufferImageCopy* pRegions) {
    mImpl->on_vkCmdCopyBufferToImage(commandBuffer, srcBuffer, dstImage,
            dstImageLayout, regionCount, pRegions);
}

VkResult VkDecoderGlobalState::on_vkAllocateMemory(
        VkDevice device,
        const VkMemoryAllocateInfo* pAllocateInfo,
        const VkAllocationCallbacks* pAllocator,
        VkDeviceMemory* pMemory) {
    return mImpl->on_vkAllocateMemory(device, pAllocateInfo, pAllocator, pMemory);
}

void VkDecoderGlobalState::on_vkFreeMemory(
    VkDevice device,
    VkDeviceMemory memory,
    const VkAllocationCallbacks* pAllocator) {
    mImpl->on_vkFreeMemory(device, memory, pAllocator);
}

VkResult VkDecoderGlobalState::on_vkMapMemory(VkDevice device,
                                              VkDeviceMemory memory,
                                              VkDeviceSize offset,
                                              VkDeviceSize size,
                                              VkMemoryMapFlags flags,
                                              void** ppData) {
    return mImpl->on_vkMapMemory(device, memory, offset, size, flags, ppData);
}

void VkDecoderGlobalState::on_vkUnmapMemory(VkDevice device,
                                            VkDeviceMemory memory) {
    mImpl->on_vkUnmapMemory(device, memory);
}

uint8_t* VkDecoderGlobalState::getMappedHostPointer(VkDeviceMemory memory) {
    return mImpl->getMappedHostPointer(memory);
}

VkDeviceSize VkDecoderGlobalState::getDeviceMemorySize(VkDeviceMemory memory) {
    return mImpl->getDeviceMemorySize(memory);
}

bool VkDecoderGlobalState::usingDirectMapping() const {
    return mImpl->usingDirectMapping();
}

// VK_ANDROID_native_buffer
VkResult VkDecoderGlobalState::on_vkGetSwapchainGrallocUsageANDROID(
    VkDevice device,
    VkFormat format,
    VkImageUsageFlags imageUsage,
    int* grallocUsage) {
    return mImpl->on_vkGetSwapchainGrallocUsageANDROID(
        device, format, imageUsage, grallocUsage);
}

VkResult VkDecoderGlobalState::on_vkGetSwapchainGrallocUsage2ANDROID(
    VkDevice device,
    VkFormat format,
    VkImageUsageFlags imageUsage,
    VkSwapchainImageUsageFlagsANDROID swapchainImageUsage,
    uint64_t* grallocConsumerUsage,
    uint64_t* grallocProducerUsage) {
    return mImpl->on_vkGetSwapchainGrallocUsage2ANDROID(
        device, format, imageUsage,
        swapchainImageUsage,
        grallocConsumerUsage,
        grallocProducerUsage);
}

VkResult VkDecoderGlobalState::on_vkAcquireImageANDROID(
    VkDevice device,
    VkImage image,
    int nativeFenceFd,
    VkSemaphore semaphore,
    VkFence fence) {
    return mImpl->on_vkAcquireImageANDROID(
        device, image, nativeFenceFd, semaphore, fence);
}

VkResult VkDecoderGlobalState::on_vkQueueSignalReleaseImageANDROID(
    VkQueue queue,
    uint32_t waitSemaphoreCount,
    const VkSemaphore* pWaitSemaphores,
    VkImage image,
    int* pNativeFenceFd) {
    return mImpl->on_vkQueueSignalReleaseImageANDROID(
        queue, waitSemaphoreCount, pWaitSemaphores,
        image, pNativeFenceFd);
}

// VK_GOOGLE_address_space
VkResult VkDecoderGlobalState::on_vkMapMemoryIntoAddressSpaceGOOGLE(
    VkDevice device, VkDeviceMemory memory, uint64_t* pAddress) {
    return mImpl->on_vkMapMemoryIntoAddressSpaceGOOGLE(
        device, memory, pAddress);
}

VkResult VkDecoderGlobalState::on_vkAllocateCommandBuffers(
        VkDevice device,
        const VkCommandBufferAllocateInfo* pAllocateInfo,
        VkCommandBuffer* pCommandBuffers) {
    return mImpl->on_vkAllocateCommandBuffers(device, pAllocateInfo,
                                              pCommandBuffers);
}

void VkDecoderGlobalState::on_vkCmdExecuteCommands(
        VkCommandBuffer commandBuffer,
        uint32_t commandBufferCount,
        const VkCommandBuffer* pCommandBuffers) {
    return mImpl->on_vkCmdExecuteCommands(commandBuffer, commandBufferCount,
                                          pCommandBuffers);
}

VkResult VkDecoderGlobalState::on_vkQueueSubmit(VkQueue queue,
                                            uint32_t submitCount,
                                            const VkSubmitInfo* pSubmits,
                                            VkFence fence) {
    return mImpl->on_vkQueueSubmit(queue, submitCount, pSubmits, fence);
}

VkResult VkDecoderGlobalState::on_vkResetCommandBuffer(
        VkCommandBuffer commandBuffer,
        VkCommandBufferResetFlags flags) {
    return mImpl->on_vkResetCommandBuffer(commandBuffer, flags);
}

void VkDecoderGlobalState::on_vkFreeCommandBuffers(
        VkDevice device,
        VkCommandPool commandPool,
        uint32_t commandBufferCount,
        const VkCommandBuffer* pCommandBuffers) {
    return mImpl->on_vkFreeCommandBuffers(device, commandPool,
                                          commandBufferCount, pCommandBuffers);
}

}  // namespace goldfish_vk