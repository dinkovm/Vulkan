/*
* Vulkan Example - Deferred shading with multiple render targets (aka G-Buffer) example
*
* Copyright (C) 2016 by Sascha Willems - www.saschawillems.de
*
* This code is licensed under the MIT license (MIT) (http://opensource.org/licenses/MIT)
*/

#include "VulkanRaytracingSample.h"
#include "vulkanexamplebase.h"
#include "VulkanglTFModel.h"

#define ENABLE_VALIDATION false

// Texture properties
#define TEX_DIM 2048
#define TEX_FILTER VK_FILTER_LINEAR

// Offscreen frame buffer properties
#define FB_DIM TEX_DIM

class VulkanExample : public VulkanRaytracingSample
{
public:

    static const uint32_t LightCount = 6;

    AccelerationStructure floorAS;
    AccelerationStructure modelAS;
    AccelerationStructure lightAS[LightCount];
	AccelerationStructure topLevelAS;



	std::vector<VkRayTracingShaderGroupCreateInfoKHR> shaderGroups{};
	struct ShaderBindingTables {
		ShaderBindingTable raygen;
		ShaderBindingTable miss;
		ShaderBindingTable hit;
	} shaderBindingTables;

    struct UniformData {
        glm::mat4 viewInverse;
        glm::mat4 projInverse;
        glm::vec4 lightPos;
        int32_t vertexSize;
    } uniformData;
    vks::Buffer ubo;

	int32_t debugDisplayTarget = 0;

	struct {
		struct {
			vks::Texture2D colorMap;
			vks::Texture2D normalMap;
		} model;
		struct {
			vks::Texture2D colorMap;
			vks::Texture2D normalMap;
		} floor;
	} textures;

	struct {
		vkglTF::Model model;
		vkglTF::Model floor;
	} models;

	struct {
		glm::mat4 projection;
		glm::mat4 model;
		glm::mat4 view;
		glm::vec4 instancePos[3];
	} uboOffscreenVS;

	struct Light {
		glm::vec4 position;
		glm::vec3 color;
		float radius;
	};

	struct {
		Light lights[LightCount];
		glm::vec4 viewPos;
		int debugDisplayTarget = 0;
        uint32_t frame = 0;
	} uboComposition;

	struct {
		vks::Buffer offscreen;
		vks::Buffer composition;
	} uniformBuffers;

	struct {
		VkPipeline offscreen;
		VkPipeline composition;
        VkPipeline raytracing;
	} pipelines;
	VkPipelineLayout pipelineLayoutGfx;
    VkPipelineLayout pipelineLayoutRt;

	struct {
		VkDescriptorSet model;
		VkDescriptorSet floor;
        VkDescriptorSet accel;
	} descriptorSets;

	VkDescriptorSet descriptorSetGfx;
	VkDescriptorSetLayout descriptorSetLayoutGfx;

    VkDescriptorSet descriptorSetRt;
    VkDescriptorSetLayout descriptorSetLayoutRt;

	// Framebuffer for offscreen rendering
	struct FrameBufferAttachment {
		VkImage image;
		VkDeviceMemory mem;
		VkImageView view;
		VkFormat format;
	};
	struct FrameBuffer {
		int32_t width, height;
		VkFramebuffer frameBuffer;
		FrameBufferAttachment position, normal, albedo;
		FrameBufferAttachment depth;
		VkRenderPass renderPass;
	} offScreenFrameBuf;

	// One sampler for the frame buffer color attachments
	VkSampler colorSampler;

	VkCommandBuffer offScreenCmdBuffer = VK_NULL_HANDLE;

	// Semaphore used to synchronize between offscreen and final scene rendering
	VkSemaphore offscreenSemaphore = VK_NULL_HANDLE;

	VulkanExample() : VulkanRaytracingSample()
	{
		title = "Deferred shading";
		camera.type = Camera::CameraType::firstperson;
		camera.movementSpeed = 5.0f;
#ifndef __ANDROID__
		camera.rotationSpeed = 0.25f;
#endif
		camera.position = { 2.15f, 0.3f, -8.75f };
		camera.setRotation(glm::vec3(-0.75f, 12.5f, 0.0f));
		camera.setPerspective(60.0f, (float)width / (float)height, 0.1f, 256.0f);
		settings.overlay = false;
        enableExtensions();
	}

	~VulkanExample()
	{
		// Clean up used Vulkan resources
		// Note : Inherited destructor cleans up resources stored in base class

		vkDestroySampler(device, colorSampler, nullptr);

		// Frame buffer

		// Color attachments
		vkDestroyImageView(device, offScreenFrameBuf.position.view, nullptr);
		vkDestroyImage(device, offScreenFrameBuf.position.image, nullptr);
		vkFreeMemory(device, offScreenFrameBuf.position.mem, nullptr);

		vkDestroyImageView(device, offScreenFrameBuf.normal.view, nullptr);
		vkDestroyImage(device, offScreenFrameBuf.normal.image, nullptr);
		vkFreeMemory(device, offScreenFrameBuf.normal.mem, nullptr);

		vkDestroyImageView(device, offScreenFrameBuf.albedo.view, nullptr);
		vkDestroyImage(device, offScreenFrameBuf.albedo.image, nullptr);
		vkFreeMemory(device, offScreenFrameBuf.albedo.mem, nullptr);

		// Depth attachment
		vkDestroyImageView(device, offScreenFrameBuf.depth.view, nullptr);
		vkDestroyImage(device, offScreenFrameBuf.depth.image, nullptr);
		vkFreeMemory(device, offScreenFrameBuf.depth.mem, nullptr);

		vkDestroyFramebuffer(device, offScreenFrameBuf.frameBuffer, nullptr);

		vkDestroyPipeline(device, pipelines.composition, nullptr);
		vkDestroyPipeline(device, pipelines.offscreen, nullptr);

		vkDestroyPipelineLayout(device, pipelineLayoutGfx, nullptr);
        vkDestroyPipelineLayout(device, pipelineLayoutRt, nullptr);

		vkDestroyDescriptorSetLayout(device, descriptorSetLayoutGfx, nullptr);
        vkDestroyDescriptorSetLayout(device, descriptorSetLayoutRt, nullptr);

		// Uniform buffers
		uniformBuffers.offscreen.destroy();
		uniformBuffers.composition.destroy();

		vkDestroyRenderPass(device, offScreenFrameBuf.renderPass, nullptr);

		textures.model.colorMap.destroy();
		textures.model.normalMap.destroy();
		textures.floor.colorMap.destroy();
		textures.floor.normalMap.destroy();

        ubo.destroy();

		vkDestroySemaphore(device, offscreenSemaphore, nullptr);
	}

    void createBottomLevelAccelerationStructureTris(
        vkglTF::Model&         model,
        AccelerationStructure& accelStruct)
    {
        VkDeviceOrHostAddressConstKHR vertexBufferDeviceAddress{};
        VkDeviceOrHostAddressConstKHR indexBufferDeviceAddress{};

        vertexBufferDeviceAddress.deviceAddress = getBufferDeviceAddress(model.vertices.buffer);
        indexBufferDeviceAddress.deviceAddress = getBufferDeviceAddress(model.indices.buffer);

        uint32_t numTriangles = static_cast<uint32_t>(model.indices.count) / 3;
        VkAccelerationStructureGeometryKHR accelerationStructureGeometry = vks::initializers::accelerationStructureGeometryKHR();

        accelerationStructureGeometry.flags = VK_GEOMETRY_OPAQUE_BIT_KHR;
        accelerationStructureGeometry.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
        accelerationStructureGeometry.geometry.triangles.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
        accelerationStructureGeometry.geometry.triangles.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
        accelerationStructureGeometry.geometry.triangles.vertexData = vertexBufferDeviceAddress;
        accelerationStructureGeometry.geometry.triangles.maxVertex = model.vertices.count;
        accelerationStructureGeometry.geometry.triangles.vertexStride = sizeof(vkglTF::Vertex);
        accelerationStructureGeometry.geometry.triangles.indexType = VK_INDEX_TYPE_UINT32;
        accelerationStructureGeometry.geometry.triangles.indexData = indexBufferDeviceAddress;
        accelerationStructureGeometry.geometry.triangles.transformData.deviceAddress = 0;
        accelerationStructureGeometry.geometry.triangles.transformData.hostAddress = nullptr;

        // Get size info
        VkAccelerationStructureBuildGeometryInfoKHR accelerationStructureBuildGeometryInfo = vks::initializers::accelerationStructureBuildGeometryInfoKHR();
        accelerationStructureBuildGeometryInfo.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
        accelerationStructureBuildGeometryInfo.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
        accelerationStructureBuildGeometryInfo.geometryCount = 1;
        accelerationStructureBuildGeometryInfo.pGeometries = &accelerationStructureGeometry;

        VkAccelerationStructureBuildSizesInfoKHR accelerationStructureBuildSizesInfo = vks::initializers::accelerationStructureBuildSizesInfoKHR();
        vkGetAccelerationStructureBuildSizesKHR(
            device,
            VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR,
            &accelerationStructureBuildGeometryInfo,
            &numTriangles,
            &accelerationStructureBuildSizesInfo);

        createAccelerationStructure(accelStruct, VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR, accelerationStructureBuildSizesInfo);

        // Create a small scratch buffer used during build of the bottom level acceleration structure
        ScratchBuffer scratchBuffer = createScratchBuffer(accelerationStructureBuildSizesInfo.buildScratchSize);

        VkAccelerationStructureBuildGeometryInfoKHR accelerationBuildGeometryInfo = vks::initializers::accelerationStructureBuildGeometryInfoKHR();
        accelerationBuildGeometryInfo.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
        accelerationBuildGeometryInfo.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
        accelerationBuildGeometryInfo.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
        accelerationBuildGeometryInfo.dstAccelerationStructure = accelStruct.handle;
        accelerationBuildGeometryInfo.geometryCount = 1;
        accelerationBuildGeometryInfo.pGeometries = &accelerationStructureGeometry;
        accelerationBuildGeometryInfo.scratchData.deviceAddress = scratchBuffer.deviceAddress;

        VkAccelerationStructureBuildRangeInfoKHR accelerationStructureBuildRangeInfo = {};
        accelerationStructureBuildRangeInfo.primitiveCount = numTriangles;
        accelerationStructureBuildRangeInfo.primitiveOffset = 0;
        accelerationStructureBuildRangeInfo.firstVertex = 0;
        accelerationStructureBuildRangeInfo.transformOffset = 0;
        std::vector<VkAccelerationStructureBuildRangeInfoKHR*> accelerationBuildStructureRangeInfos = { &accelerationStructureBuildRangeInfo };

        if (accelerationStructureFeatures.accelerationStructureHostCommands)
        {
            // Implementation supports building acceleration structure building on host
            vkBuildAccelerationStructuresKHR(
                device,
                VK_NULL_HANDLE,
                1,
                &accelerationBuildGeometryInfo,
                accelerationBuildStructureRangeInfos.data());
        }
        else
        {
            // Acceleration structure needs to be build on the device
            VkCommandBuffer commandBuffer = vulkanDevice->createCommandBuffer(VK_COMMAND_BUFFER_LEVEL_PRIMARY, true);
            vkCmdBuildAccelerationStructuresKHR(
                commandBuffer,
                1,
                &accelerationBuildGeometryInfo,
                accelerationBuildStructureRangeInfos.data());
            vulkanDevice->flushCommandBuffer(commandBuffer, queue);
        }

        deleteScratchBuffer(scratchBuffer);
    }

    void createBottonLevelAccelerationStructureLight(
        VkAabbPositionsKHR& aabb,
        AccelerationStructure& accelStruct)
    {
        uint32_t aabbCount = 1;

        VkDeviceOrHostAddressConstKHR dataBufferDeviceAddress{};

        // Buffer for instance data
        vks::Buffer dataBuffer;
        VK_CHECK_RESULT(vulkanDevice->createBuffer(
            VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
            &dataBuffer,
            sizeof(aabb),
            &aabb));

        VkAccelerationStructureGeometryKHR accelerationStructureGeometry = vks::initializers::accelerationStructureGeometryKHR();

        accelerationStructureGeometry.flags = VK_GEOMETRY_OPAQUE_BIT_KHR;
        accelerationStructureGeometry.geometryType = VK_GEOMETRY_TYPE_AABBS_KHR;
        accelerationStructureGeometry.geometry.aabbs.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_AABBS_DATA_KHR;
        accelerationStructureGeometry.geometry.aabbs.data.deviceAddress = getBufferDeviceAddress(dataBuffer.buffer);
        accelerationStructureGeometry.geometry.aabbs.stride = sizeof(VkAabbPositionsKHR);

        // Get size info
        VkAccelerationStructureBuildGeometryInfoKHR accelerationStructureBuildGeometryInfo = vks::initializers::accelerationStructureBuildGeometryInfoKHR();
        accelerationStructureBuildGeometryInfo.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
        accelerationStructureBuildGeometryInfo.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
        accelerationStructureBuildGeometryInfo.geometryCount = 1;
        accelerationStructureBuildGeometryInfo.pGeometries = &accelerationStructureGeometry;

        VkAccelerationStructureBuildSizesInfoKHR accelerationStructureBuildSizesInfo = vks::initializers::accelerationStructureBuildSizesInfoKHR();
        vkGetAccelerationStructureBuildSizesKHR(
            device,
            VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR,
            &accelerationStructureBuildGeometryInfo,
            &aabbCount,
            &accelerationStructureBuildSizesInfo);

        createAccelerationStructure(accelStruct, VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR, accelerationStructureBuildSizesInfo);

        // Create a small scratch buffer used during build of the bottom level acceleration structure
        ScratchBuffer scratchBuffer = createScratchBuffer(accelerationStructureBuildSizesInfo.buildScratchSize);

        VkAccelerationStructureBuildGeometryInfoKHR accelerationBuildGeometryInfo = vks::initializers::accelerationStructureBuildGeometryInfoKHR();
        accelerationBuildGeometryInfo.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
        accelerationBuildGeometryInfo.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
        accelerationBuildGeometryInfo.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
        accelerationBuildGeometryInfo.dstAccelerationStructure = accelStruct.handle;
        accelerationBuildGeometryInfo.geometryCount = 1;
        accelerationBuildGeometryInfo.pGeometries = &accelerationStructureGeometry;
        accelerationBuildGeometryInfo.scratchData.deviceAddress = scratchBuffer.deviceAddress;

        VkAccelerationStructureBuildRangeInfoKHR accelerationStructureBuildRangeInfo = {};
        accelerationStructureBuildRangeInfo.primitiveCount = aabbCount;
        accelerationStructureBuildRangeInfo.primitiveOffset = 0;
        accelerationStructureBuildRangeInfo.firstVertex = 0;
        accelerationStructureBuildRangeInfo.transformOffset = 0;
        std::vector<VkAccelerationStructureBuildRangeInfoKHR*> accelerationBuildStructureRangeInfos = { &accelerationStructureBuildRangeInfo };

        if (accelerationStructureFeatures.accelerationStructureHostCommands)
        {
            // Implementation supports building acceleration structure building on host
            vkBuildAccelerationStructuresKHR(
                device,
                VK_NULL_HANDLE,
                1,
                &accelerationBuildGeometryInfo,
                accelerationBuildStructureRangeInfos.data());
        }
        else
        {
            // Acceleration structure needs to be build on the device
            VkCommandBuffer commandBuffer = vulkanDevice->createCommandBuffer(VK_COMMAND_BUFFER_LEVEL_PRIMARY, true);
            vkCmdBuildAccelerationStructuresKHR(
                commandBuffer,
                1,
                &accelerationBuildGeometryInfo,
                accelerationBuildStructureRangeInfos.data());
            vulkanDevice->flushCommandBuffer(commandBuffer, queue);
        }

        dataBuffer.destroy();
        deleteScratchBuffer(scratchBuffer);
    }

    /*
        Create the bottom level acceleration structure contains the scene's actual geometry (vertices, triangles)
    */
    void createBottomLevelAccelerationStructure()
    {
        createBottomLevelAccelerationStructureTris(models.floor, floorAS);
        createBottomLevelAccelerationStructureTris(models.model, modelAS);

        VkAabbPositionsKHR aabb = {
            -2, -2, -2, // min X, Y, Z
             2,  2,  2  // max X, Y, X
        };
        createBottonLevelAccelerationStructureLight(aabb, lightAS[0]);
    }

    /*
        The top level acceleration structure contains the scene's object instances
    */
    void createTopLevelAccelerationStructure()
    {
        constexpr uint32_t InstanceCount = 4;

        VkAccelerationStructureInstanceKHR instances[InstanceCount] = {};
        {
            VkTransformMatrixKHR transformMatrix = {
                1.0f, 0.0f, 0.0f, 0.0f,
                0.0f, 1.0f, 0.0f, 0.0f,
                0.0f, 0.0f, 1.0f, 0.0f };

            instances[0].transform = transformMatrix;
            instances[0].instanceCustomIndex = 0;
            instances[0].mask = 0xFF;
            instances[0].instanceShaderBindingTableRecordOffset = 0;
            instances[0].flags = VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR;
            instances[0].accelerationStructureReference = floorAS.deviceAddress;
        }
        {
            VkTransformMatrixKHR transformMatrix = {
                1.0f, 0.0f, 0.0f, 0.0f,
                0.0f, 1.0f, 0.0f, 0.0f,
                0.0f, 0.0f, 1.0f, 0.0f };

            instances[1].transform = transformMatrix;
            instances[1].instanceCustomIndex = 0;
            instances[1].mask = 0xFF;
            instances[1].instanceShaderBindingTableRecordOffset = 1;
            instances[1].flags = VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR;
            instances[1].accelerationStructureReference = modelAS.deviceAddress;
        }
        {
            VkTransformMatrixKHR transformMatrix = {
                1.0f, 0.0f, 0.0f, -4.0f,
                0.0f, 1.0f, 0.0f, 0.0f,
                0.0f, 0.0f, 1.0f, -4.0f };

            instances[2].transform = transformMatrix;
            instances[2].instanceCustomIndex = 0;
            instances[2].mask = 0xFF;
            instances[2].instanceShaderBindingTableRecordOffset = 1;
            instances[2].flags = VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR;
            instances[2].accelerationStructureReference = modelAS.deviceAddress;
        }
        {
            VkTransformMatrixKHR transformMatrix = {
                1.0f, 0.0f, 0.0f, 4.0f,
                0.0f, 1.0f, 0.0f, 0.0f,
                0.0f, 0.0f, 1.0f, -4.0f };

            instances[3].transform = transformMatrix;
            instances[3].instanceCustomIndex = 0;
            instances[3].mask = 0xFF;
            //instances[3].instanceShaderBindingTableRecordOffset = 2;
            instances[3].instanceShaderBindingTableRecordOffset = 1;
            instances[3].flags = VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR;
            instances[3].accelerationStructureReference = modelAS.deviceAddress;
            //instances[3].accelerationStructureReference = lightAS[0].deviceAddress;
        }

        // Buffer for instance data
        vks::Buffer instancesBuffer;
        VK_CHECK_RESULT(vulkanDevice->createBuffer(
            VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
            &instancesBuffer,
            sizeof(instances),
            instances));

        VkDeviceOrHostAddressConstKHR instanceDataDeviceAddress{};
        instanceDataDeviceAddress.deviceAddress = getBufferDeviceAddress(instancesBuffer.buffer);

        VkAccelerationStructureGeometryKHR accelerationStructureGeometry = vks::initializers::accelerationStructureGeometryKHR();
        accelerationStructureGeometry.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
        accelerationStructureGeometry.flags = VK_GEOMETRY_OPAQUE_BIT_KHR;
        accelerationStructureGeometry.geometry.instances.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR;
        accelerationStructureGeometry.geometry.instances.arrayOfPointers = VK_FALSE;
        accelerationStructureGeometry.geometry.instances.data = instanceDataDeviceAddress;

        // Get size info
        VkAccelerationStructureBuildGeometryInfoKHR accelerationStructureBuildGeometryInfo = vks::initializers::accelerationStructureBuildGeometryInfoKHR();
        accelerationStructureBuildGeometryInfo.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
        accelerationStructureBuildGeometryInfo.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
        accelerationStructureBuildGeometryInfo.geometryCount = 1;
        accelerationStructureBuildGeometryInfo.pGeometries = &accelerationStructureGeometry;

        VkAccelerationStructureBuildSizesInfoKHR accelerationStructureBuildSizesInfo = vks::initializers::accelerationStructureBuildSizesInfoKHR();
        vkGetAccelerationStructureBuildSizesKHR(
            device,
            VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR,
            &accelerationStructureBuildGeometryInfo,
            &InstanceCount,
            &accelerationStructureBuildSizesInfo);

        // @todo: as return value?
        createAccelerationStructure(topLevelAS, VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR, accelerationStructureBuildSizesInfo);

        // Create a small scratch buffer used during build of the top level acceleration structure
        ScratchBuffer scratchBuffer = createScratchBuffer(accelerationStructureBuildSizesInfo.buildScratchSize);

        VkAccelerationStructureBuildGeometryInfoKHR accelerationBuildGeometryInfo = vks::initializers::accelerationStructureBuildGeometryInfoKHR();
        accelerationBuildGeometryInfo.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
        accelerationBuildGeometryInfo.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
        accelerationBuildGeometryInfo.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
        accelerationBuildGeometryInfo.dstAccelerationStructure = topLevelAS.handle;
        accelerationBuildGeometryInfo.geometryCount = 1;
        accelerationBuildGeometryInfo.pGeometries = &accelerationStructureGeometry;
        accelerationBuildGeometryInfo.scratchData.deviceAddress = scratchBuffer.deviceAddress;

        VkAccelerationStructureBuildRangeInfoKHR accelerationStructureBuildRangeInfo{};
        accelerationStructureBuildRangeInfo.primitiveCount = InstanceCount;
        accelerationStructureBuildRangeInfo.primitiveOffset = 0;
        accelerationStructureBuildRangeInfo.firstVertex = 0;
        accelerationStructureBuildRangeInfo.transformOffset = 0;
        std::vector<VkAccelerationStructureBuildRangeInfoKHR*> accelerationBuildStructureRangeInfos = { &accelerationStructureBuildRangeInfo };

        if (accelerationStructureFeatures.accelerationStructureHostCommands)
        {
            // Implementation supports building acceleration structure building on host
            vkBuildAccelerationStructuresKHR(
                device,
                VK_NULL_HANDLE,
                1,
                &accelerationBuildGeometryInfo,
                accelerationBuildStructureRangeInfos.data());
        }
        else
        {
            // Acceleration structure needs to be build on the device
            VkCommandBuffer commandBuffer = vulkanDevice->createCommandBuffer(VK_COMMAND_BUFFER_LEVEL_PRIMARY, true);
            vkCmdBuildAccelerationStructuresKHR(
                commandBuffer,
                1,
                &accelerationBuildGeometryInfo,
                accelerationBuildStructureRangeInfos.data());
            vulkanDevice->flushCommandBuffer(commandBuffer, queue);
        }

        deleteScratchBuffer(scratchBuffer);
        instancesBuffer.destroy();
    }

    /*
        Create the Shader Binding Tables that binds the programs and top-level acceleration structure

        SBT Layout used in this sample:

            /-----------\
            | raygen    |
            |-----------|
            | miss      |
            |-----------|
            | hit       |
            \-----------/

    */
    void createShaderBindingTables() {

        const uint32_t handleSize = rayTracingPipelineProperties.shaderGroupHandleSize;
        const uint32_t handleSizeAligned = vks::tools::alignedSize(rayTracingPipelineProperties.shaderGroupHandleSize, rayTracingPipelineProperties.shaderGroupHandleAlignment);
        const uint32_t groupCount = static_cast<uint32_t>(shaderGroups.size());
        const uint32_t sbtSize = groupCount * handleSizeAligned;

        std::vector<uint8_t> shaderHandleStorage(sbtSize);
        VK_CHECK_RESULT(vkGetRayTracingShaderGroupHandlesKHR(device, pipelines.raytracing, 0, groupCount, sbtSize, shaderHandleStorage.data()));

        createShaderBindingTable(shaderBindingTables.raygen, 1);
        // We are using two miss shaders
        createShaderBindingTable(shaderBindingTables.miss, 2);
        createShaderBindingTable(shaderBindingTables.hit, 3);

        // Copy handles
        memcpy(shaderBindingTables.raygen.mapped, shaderHandleStorage.data(), handleSize);
        // We are using two miss shaders, so we need to get two handles for the miss shader binding table
        memcpy(shaderBindingTables.miss.mapped, shaderHandleStorage.data() + handleSizeAligned, handleSize * 2);
        memcpy(shaderBindingTables.hit.mapped, shaderHandleStorage.data() + handleSizeAligned * 3, handleSize * 3);
    }

    /*
        Create the uniform buffer used to pass matrices to the ray tracing ray generation shader
    */
    void createUniformBufferRayTracing()
    {
        VK_CHECK_RESULT(vulkanDevice->createBuffer(
            VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
            &ubo,
            sizeof(uniformData),
            &uniformData));
        VK_CHECK_RESULT(ubo.map());

        updateUniformBufferRayTracing();
    }

    void updateUniformBufferRayTracing()
    {
        uniformData.projInverse = glm::inverse(camera.matrices.perspective);
        uniformData.viewInverse = glm::inverse(camera.matrices.view);
        uniformData.lightPos = glm::vec4(cos(glm::radians(timer * 360.0f)) * 40.0f, -50.0f + sin(glm::radians(timer * 360.0f)) * 20.0f, 25.0f + sin(glm::radians(timer * 360.0f)) * 5.0f, 0.0f);
        // Pass the vertex size to the shader for unpacking vertices
        uniformData.vertexSize = sizeof(vkglTF::Vertex);
        memcpy(ubo.mapped, &uniformData, sizeof(uniformData));
    }

	// Enable physical device features required for this example
	virtual void getEnabledFeatures()
	{
        // Enable features required for ray tracing using feature chaining via pNext
        enabledBufferDeviceAddresFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES;
        enabledBufferDeviceAddresFeatures.bufferDeviceAddress = VK_TRUE;

        enabledRayTracingPipelineFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_FEATURES_KHR;
        enabledRayTracingPipelineFeatures.rayTracingPipeline = VK_TRUE;
        enabledRayTracingPipelineFeatures.pNext = &enabledBufferDeviceAddresFeatures;

        enabledAccelerationStructureFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR;
        enabledAccelerationStructureFeatures.accelerationStructure = VK_TRUE;
        enabledAccelerationStructureFeatures.pNext = &enabledRayTracingPipelineFeatures;

        deviceCreatepNextChain = &enabledAccelerationStructureFeatures;

		// Enable anisotropic filtering if supported
		if (deviceFeatures.samplerAnisotropy) {
			enabledFeatures.samplerAnisotropy = VK_TRUE;
		}
	};

	// Create a frame buffer attachment
	void createAttachment(
		VkFormat format,
		VkImageUsageFlagBits usage,
		FrameBufferAttachment *attachment)
	{
		VkImageAspectFlags aspectMask = 0;
		VkImageLayout imageLayout;

		attachment->format = format;

		if (usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT)
		{
			aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
			imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
		}
		if (usage & VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT)
		{
			aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
			imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
		}

		assert(aspectMask > 0);

		VkImageCreateInfo image = vks::initializers::imageCreateInfo();
		image.imageType = VK_IMAGE_TYPE_2D;
		image.format = format;
		image.extent.width = offScreenFrameBuf.width;
		image.extent.height = offScreenFrameBuf.height;
		image.extent.depth = 1;
		image.mipLevels = 1;
		image.arrayLayers = 1;
		image.samples = VK_SAMPLE_COUNT_1_BIT;
		image.tiling = VK_IMAGE_TILING_OPTIMAL;
		image.usage = usage | VK_IMAGE_USAGE_SAMPLED_BIT;

		VkMemoryAllocateInfo memAlloc = vks::initializers::memoryAllocateInfo();
		VkMemoryRequirements memReqs;

		VK_CHECK_RESULT(vkCreateImage(device, &image, nullptr, &attachment->image));
		vkGetImageMemoryRequirements(device, attachment->image, &memReqs);
		memAlloc.allocationSize = memReqs.size;
		memAlloc.memoryTypeIndex = vulkanDevice->getMemoryType(memReqs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
		VK_CHECK_RESULT(vkAllocateMemory(device, &memAlloc, nullptr, &attachment->mem));
		VK_CHECK_RESULT(vkBindImageMemory(device, attachment->image, attachment->mem, 0));

		VkImageViewCreateInfo imageView = vks::initializers::imageViewCreateInfo();
		imageView.viewType = VK_IMAGE_VIEW_TYPE_2D;
		imageView.format = format;
		imageView.subresourceRange = {};
		imageView.subresourceRange.aspectMask = aspectMask;
		imageView.subresourceRange.baseMipLevel = 0;
		imageView.subresourceRange.levelCount = 1;
		imageView.subresourceRange.baseArrayLayer = 0;
		imageView.subresourceRange.layerCount = 1;
		imageView.image = attachment->image;
		VK_CHECK_RESULT(vkCreateImageView(device, &imageView, nullptr, &attachment->view));
	}

	// Prepare a new framebuffer and attachments for offscreen rendering (G-Buffer)
	void prepareOffscreenFramebuffer()
	{
		offScreenFrameBuf.width = FB_DIM;
		offScreenFrameBuf.height = FB_DIM;

		// Color attachments

		// (World space) Positions
		createAttachment(
			VK_FORMAT_R16G16B16A16_SFLOAT,
			VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
			&offScreenFrameBuf.position);

		// (World space) Normals
		createAttachment(
			VK_FORMAT_R16G16B16A16_SFLOAT,
			VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
			&offScreenFrameBuf.normal);

		// Albedo (color)
		createAttachment(
			VK_FORMAT_R8G8B8A8_UNORM,
			VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
			&offScreenFrameBuf.albedo);

		// Depth attachment

		// Find a suitable depth format
		VkFormat attDepthFormat;
		VkBool32 validDepthFormat = vks::tools::getSupportedDepthFormat(physicalDevice, &attDepthFormat);
		assert(validDepthFormat);

		createAttachment(
			attDepthFormat,
			VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
			&offScreenFrameBuf.depth);

		// Set up separate renderpass with references to the color and depth attachments
		std::array<VkAttachmentDescription, 4> attachmentDescs = {};

		// Init attachment properties
		for (uint32_t i = 0; i < 4; ++i)
		{
			attachmentDescs[i].samples = VK_SAMPLE_COUNT_1_BIT;
			attachmentDescs[i].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
			attachmentDescs[i].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
			attachmentDescs[i].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
			attachmentDescs[i].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
			if (i == 3)
			{
				attachmentDescs[i].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
				attachmentDescs[i].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
			}
			else
			{
				attachmentDescs[i].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
				attachmentDescs[i].finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
			}
		}

		// Formats
		attachmentDescs[0].format = offScreenFrameBuf.position.format;
		attachmentDescs[1].format = offScreenFrameBuf.normal.format;
		attachmentDescs[2].format = offScreenFrameBuf.albedo.format;
		attachmentDescs[3].format = offScreenFrameBuf.depth.format;

		std::vector<VkAttachmentReference> colorReferences;
		colorReferences.push_back({ 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL });
		colorReferences.push_back({ 1, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL });
		colorReferences.push_back({ 2, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL });

		VkAttachmentReference depthReference = {};
		depthReference.attachment = 3;
		depthReference.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

		VkSubpassDescription subpass = {};
		subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
		subpass.pColorAttachments = colorReferences.data();
		subpass.colorAttachmentCount = static_cast<uint32_t>(colorReferences.size());
		subpass.pDepthStencilAttachment = &depthReference;

		// Use subpass dependencies for attachment layout transitions
		std::array<VkSubpassDependency, 2> dependencies;

		dependencies[0].srcSubpass = VK_SUBPASS_EXTERNAL;
		dependencies[0].dstSubpass = 0;
		dependencies[0].srcStageMask = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
		dependencies[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
		dependencies[0].srcAccessMask = VK_ACCESS_MEMORY_READ_BIT;
		dependencies[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
		dependencies[0].dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT;

		dependencies[1].srcSubpass = 0;
		dependencies[1].dstSubpass = VK_SUBPASS_EXTERNAL;
		dependencies[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
		dependencies[1].dstStageMask = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
		dependencies[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
		dependencies[1].dstAccessMask = VK_ACCESS_MEMORY_READ_BIT;
		dependencies[1].dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT;

		VkRenderPassCreateInfo renderPassInfo = {};
		renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
		renderPassInfo.pAttachments = attachmentDescs.data();
		renderPassInfo.attachmentCount = static_cast<uint32_t>(attachmentDescs.size());
		renderPassInfo.subpassCount = 1;
		renderPassInfo.pSubpasses = &subpass;
		renderPassInfo.dependencyCount = 2;
		renderPassInfo.pDependencies = dependencies.data();

		VK_CHECK_RESULT(vkCreateRenderPass(device, &renderPassInfo, nullptr, &offScreenFrameBuf.renderPass));

		std::array<VkImageView,4> attachments;
		attachments[0] = offScreenFrameBuf.position.view;
		attachments[1] = offScreenFrameBuf.normal.view;
		attachments[2] = offScreenFrameBuf.albedo.view;
		attachments[3] = offScreenFrameBuf.depth.view;

		VkFramebufferCreateInfo fbufCreateInfo = {};
		fbufCreateInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
		fbufCreateInfo.pNext = NULL;
		fbufCreateInfo.renderPass = offScreenFrameBuf.renderPass;
		fbufCreateInfo.pAttachments = attachments.data();
		fbufCreateInfo.attachmentCount = static_cast<uint32_t>(attachments.size());
		fbufCreateInfo.width = offScreenFrameBuf.width;
		fbufCreateInfo.height = offScreenFrameBuf.height;
		fbufCreateInfo.layers = 1;
		VK_CHECK_RESULT(vkCreateFramebuffer(device, &fbufCreateInfo, nullptr, &offScreenFrameBuf.frameBuffer));

		// Create sampler to sample from the color attachments
		VkSamplerCreateInfo sampler = vks::initializers::samplerCreateInfo();
		sampler.magFilter = VK_FILTER_NEAREST;
		sampler.minFilter = VK_FILTER_NEAREST;
		sampler.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
		sampler.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
		sampler.addressModeV = sampler.addressModeU;
		sampler.addressModeW = sampler.addressModeU;
		sampler.mipLodBias = 0.0f;
		sampler.maxAnisotropy = 1.0f;
		sampler.minLod = 0.0f;
		sampler.maxLod = 1.0f;
		sampler.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
		VK_CHECK_RESULT(vkCreateSampler(device, &sampler, nullptr, &colorSampler));
	}

	// Build command buffer for rendering the scene to the offscreen frame buffer attachments
	void buildDeferredCommandBuffer()
	{
		if (offScreenCmdBuffer == VK_NULL_HANDLE)
		{
			offScreenCmdBuffer = vulkanDevice->createCommandBuffer(VK_COMMAND_BUFFER_LEVEL_PRIMARY, false);
		}

		// Create a semaphore used to synchronize offscreen rendering and usage
		VkSemaphoreCreateInfo semaphoreCreateInfo = vks::initializers::semaphoreCreateInfo();
		VK_CHECK_RESULT(vkCreateSemaphore(device, &semaphoreCreateInfo, nullptr, &offscreenSemaphore));

		VkCommandBufferBeginInfo cmdBufInfo = vks::initializers::commandBufferBeginInfo();

		// Clear values for all attachments written in the fragment shader
		std::array<VkClearValue,4> clearValues;
		clearValues[0].color = { { 0.0f, 0.0f, 0.0f, 0.0f } };
		clearValues[1].color = { { 0.0f, 0.0f, 0.0f, 0.0f } };
		clearValues[2].color = { { 0.0f, 0.0f, 0.0f, 0.0f } };
		clearValues[3].depthStencil = { 1.0f, 0 };

		VkRenderPassBeginInfo renderPassBeginInfo = vks::initializers::renderPassBeginInfo();
		renderPassBeginInfo.renderPass =  offScreenFrameBuf.renderPass;
		renderPassBeginInfo.framebuffer = offScreenFrameBuf.frameBuffer;
		renderPassBeginInfo.renderArea.extent.width = offScreenFrameBuf.width;
		renderPassBeginInfo.renderArea.extent.height = offScreenFrameBuf.height;
		renderPassBeginInfo.clearValueCount = static_cast<uint32_t>(clearValues.size());
		renderPassBeginInfo.pClearValues = clearValues.data();

		VK_CHECK_RESULT(vkBeginCommandBuffer(offScreenCmdBuffer, &cmdBufInfo));

		vkCmdBeginRenderPass(offScreenCmdBuffer, &renderPassBeginInfo, VK_SUBPASS_CONTENTS_INLINE);

		VkViewport viewport = vks::initializers::viewport((float)offScreenFrameBuf.width, (float)offScreenFrameBuf.height, 0.0f, 1.0f);
		vkCmdSetViewport(offScreenCmdBuffer, 0, 1, &viewport);

		VkRect2D scissor = vks::initializers::rect2D(offScreenFrameBuf.width, offScreenFrameBuf.height, 0, 0);
		vkCmdSetScissor(offScreenCmdBuffer, 0, 1, &scissor);

		vkCmdBindPipeline(offScreenCmdBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelines.offscreen);

		// Background
		vkCmdBindDescriptorSets(offScreenCmdBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayoutGfx, 0, 1, &descriptorSets.floor, 0, nullptr);
		models.floor.draw(offScreenCmdBuffer);

		// Instanced object
		vkCmdBindDescriptorSets(offScreenCmdBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayoutGfx, 0, 1, &descriptorSets.model, 0, nullptr);
		models.model.bindBuffers(offScreenCmdBuffer);
		vkCmdDrawIndexed(offScreenCmdBuffer, models.model.indices.count, 3, 0, 0, 0);

		vkCmdEndRenderPass(offScreenCmdBuffer);

		VK_CHECK_RESULT(vkEndCommandBuffer(offScreenCmdBuffer));
	}

	void loadAssets()
	{
		const uint32_t glTFLoadingFlags = vkglTF::FileLoadingFlags::PreTransformVertices | vkglTF::FileLoadingFlags::PreMultiplyVertexColors | vkglTF::FileLoadingFlags::FlipY;
		models.model.loadFromFile(getAssetPath() + "models/armor/armor.gltf", vulkanDevice, queue, glTFLoadingFlags);
		models.floor.loadFromFile(getAssetPath() + "models/deferred_box2.gltf", vulkanDevice, queue, glTFLoadingFlags);
		textures.model.colorMap.loadFromFile(getAssetPath() + "models/armor/colormap_rgba.ktx", VK_FORMAT_R8G8B8A8_UNORM, vulkanDevice, queue);
		textures.model.normalMap.loadFromFile(getAssetPath() + "models/armor/normalmap_rgba.ktx", VK_FORMAT_R8G8B8A8_UNORM, vulkanDevice, queue);
		textures.floor.colorMap.loadFromFile(getAssetPath() + "textures/stonefloor01_color_rgba.ktx", VK_FORMAT_R8G8B8A8_UNORM, vulkanDevice, queue);
		textures.floor.normalMap.loadFromFile(getAssetPath() + "textures/stonefloor01_normal_rgba.ktx", VK_FORMAT_R8G8B8A8_UNORM, vulkanDevice, queue);
	}

	void buildCommandBuffers(int mode)
	{
		VkCommandBufferBeginInfo cmdBufInfo = vks::initializers::commandBufferBeginInfo();

		VkClearValue clearValues[2];
		clearValues[0].color = { { 0.0f, 0.0f, 0.2f, 0.0f } };
		clearValues[1].depthStencil = { 1.0f, 0 };

		VkRenderPassBeginInfo renderPassBeginInfo = vks::initializers::renderPassBeginInfo();
		renderPassBeginInfo.renderPass = renderPass;
		renderPassBeginInfo.renderArea.offset.x = 0;
		renderPassBeginInfo.renderArea.offset.y = 0;
		renderPassBeginInfo.renderArea.extent.width = width;
		renderPassBeginInfo.renderArea.extent.height = height;
		renderPassBeginInfo.clearValueCount = 2;
		renderPassBeginInfo.pClearValues = clearValues;

        VkImageSubresourceRange subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };

		for (int32_t i = 0; i < drawCmdBuffers.size(); ++i)
		{
            vkResetCommandBuffer(drawCmdBuffers[i], 0);

			VK_CHECK_RESULT(vkBeginCommandBuffer(drawCmdBuffers[i], &cmdBufInfo));

            if (mode == 1)
            {
                /*
                    Dispatch the ray tracing commands
                */
                vkCmdBindPipeline(drawCmdBuffers[i], VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR, pipelines.raytracing);
                vkCmdBindDescriptorSets(drawCmdBuffers[i], VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR, pipelineLayoutRt, 0, 1, &descriptorSetRt, 0, 0);

                VkStridedDeviceAddressRegionKHR emptySbtEntry = {};
                vkCmdTraceRaysKHR(
                    drawCmdBuffers[i],
                    &shaderBindingTables.raygen.stridedDeviceAddressRegion,
                    &shaderBindingTables.miss.stridedDeviceAddressRegion,
                    &shaderBindingTables.hit.stridedDeviceAddressRegion,
                    &emptySbtEntry,
                    width,
                    height,
                    1);

                /*
                    Copy ray tracing output to swap chain image
                */

                // Prepare current swap chain image as transfer destination
                vks::tools::setImageLayout(
                    drawCmdBuffers[i],
                    swapChain.images[i],
                    VK_IMAGE_LAYOUT_UNDEFINED,
                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    subresourceRange);

                // Prepare ray tracing output image as transfer source
                vks::tools::setImageLayout(
                    drawCmdBuffers[i],
                    storageImage.image,
                    VK_IMAGE_LAYOUT_GENERAL,
                    VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                    subresourceRange);

                VkImageCopy copyRegion{};
                copyRegion.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
                copyRegion.srcOffset = { 0, 0, 0 };
                copyRegion.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
                copyRegion.dstOffset = { 0, 0, 0 };
                copyRegion.extent = { width, height, 1 };
                vkCmdCopyImage(drawCmdBuffers[i], storageImage.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, swapChain.images[i], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copyRegion);

                // Transition swap chain image back for presentation
                vks::tools::setImageLayout(
                    drawCmdBuffers[i],
                    swapChain.images[i],
                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                    subresourceRange);

                // Transition ray tracing output image back to general layout
                vks::tools::setImageLayout(
                    drawCmdBuffers[i],
                    storageImage.image,
                    VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                    VK_IMAGE_LAYOUT_GENERAL,
                    subresourceRange);
            }
            else if (mode == 0)
            {
                renderPassBeginInfo.framebuffer = frameBuffers[i];

                vkCmdBeginRenderPass(drawCmdBuffers[i], &renderPassBeginInfo, VK_SUBPASS_CONTENTS_INLINE);

                VkViewport viewport = vks::initializers::viewport((float)width, (float)height, 0.0f, 1.0f);
                vkCmdSetViewport(drawCmdBuffers[i], 0, 1, &viewport);

                VkRect2D scissor = vks::initializers::rect2D(width, height, 0, 0);
                vkCmdSetScissor(drawCmdBuffers[i], 0, 1, &scissor);

                vkCmdBindDescriptorSets(drawCmdBuffers[i], VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayoutGfx, 0, 1, &descriptorSetGfx, 0, nullptr);

                vkCmdBindPipeline(drawCmdBuffers[i], VK_PIPELINE_BIND_POINT_GRAPHICS, pipelines.composition);
                // Final composition as full screen quad
                // Note: Also used for debug display if debugDisplayTarget > 0
                vkCmdDraw(drawCmdBuffers[i], 3, 1, 0, 0);

                drawUI(drawCmdBuffers[i]);

                vkCmdEndRenderPass(drawCmdBuffers[i]);
            }

			VK_CHECK_RESULT(vkEndCommandBuffer(drawCmdBuffers[i]));
		}
	}

	void setupDescriptorPool()
	{
		std::vector<VkDescriptorPoolSize> poolSizes = {
			vks::initializers::descriptorPoolSize(VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 12),
			vks::initializers::descriptorPoolSize(VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 13),
            vks::initializers::descriptorPoolSize(VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 1),
            vks::initializers::descriptorPoolSize(VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1)
		};

		VkDescriptorPoolCreateInfo descriptorPoolInfo = vks::initializers::descriptorPoolCreateInfo(poolSizes, 4);
		VK_CHECK_RESULT(vkCreateDescriptorPool(device, &descriptorPoolInfo, nullptr, &descriptorPool));
	}

    void setupDescriptorSetLayoutRt()
    {
        constexpr auto RtShaderStages = VK_SHADER_STAGE_RAYGEN_BIT_KHR | VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR | VK_SHADER_STAGE_MISS_BIT_KHR;

        std::vector<VkDescriptorSetLayoutBinding> setLayoutBindings = {
            // Binding 0: Acceleration structure
            vks::initializers::descriptorSetLayoutBinding(VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, VK_SHADER_STAGE_RAYGEN_BIT_KHR | VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR, 0),
            // Binding 1: Storage image
            vks::initializers::descriptorSetLayoutBinding(VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_SHADER_STAGE_RAYGEN_BIT_KHR, 1),
            // Binding 2: Uniform buffer
            vks::initializers::descriptorSetLayoutBinding(VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, RtShaderStages, 2),
            // Binding 3: Vertex buffer (model)
            vks::initializers::descriptorSetLayoutBinding(VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR, 3),
            // Binding 4: Index buffer (model)
            vks::initializers::descriptorSetLayoutBinding(VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR, 4),
            // Binding 5 : Position texture target (model)
            vks::initializers::descriptorSetLayoutBinding(VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_FRAGMENT_BIT, 5),
            // Binding 6 : Normals texture target (model)
            vks::initializers::descriptorSetLayoutBinding(VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_FRAGMENT_BIT, 6),
            // Binding 7: Vertex buffer (floor)
            vks::initializers::descriptorSetLayoutBinding(VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR, 7),
            // Binding 8: Index buffer (floor)
            vks::initializers::descriptorSetLayoutBinding(VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR, 8),
            // Binding 9 : Position texture target (floor)
            vks::initializers::descriptorSetLayoutBinding(VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_FRAGMENT_BIT, 9),
            // Binding 10 : Normals texture target (floor)
		    vks::initializers::descriptorSetLayoutBinding(VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_FRAGMENT_BIT, 10),
		    // Binding 11 : Fragment shader uniform buffer
		    vks::initializers::descriptorSetLayoutBinding(VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, VK_SHADER_STAGE_FRAGMENT_BIT, 11),
        };

        VkDescriptorSetLayoutCreateInfo descriptorLayout = vks::initializers::descriptorSetLayoutCreateInfo(setLayoutBindings);
        VK_CHECK_RESULT(vkCreateDescriptorSetLayout(device, &descriptorLayout, nullptr, &descriptorSetLayoutRt));

        // Shared pipeline layout used by all graphics pipelines
        VkPipelineLayoutCreateInfo pPipelineLayoutCreateInfo = vks::initializers::pipelineLayoutCreateInfo(&descriptorSetLayoutRt, 1);
        VK_CHECK_RESULT(vkCreatePipelineLayout(device, &pPipelineLayoutCreateInfo, nullptr, &pipelineLayoutRt));
    }

	void setupDescriptorSetLayoutGfx()
	{
		// Deferred shading layout
		std::vector<VkDescriptorSetLayoutBinding> setLayoutBindings = {
			// Binding 0 : Vertex shader uniform buffer
			vks::initializers::descriptorSetLayoutBinding( VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, VK_SHADER_STAGE_VERTEX_BIT, 0),
			// Binding 1 : Position texture target / Scene colormap
			vks::initializers::descriptorSetLayoutBinding(VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_FRAGMENT_BIT, 1),
			// Binding 2 : Normals texture target
			vks::initializers::descriptorSetLayoutBinding(VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_FRAGMENT_BIT, 2),
			// Binding 3 : Albedo texture target
			vks::initializers::descriptorSetLayoutBinding(VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_FRAGMENT_BIT, 3),
			// Binding 4 : Fragment shader uniform buffer
			vks::initializers::descriptorSetLayoutBinding(VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, VK_SHADER_STAGE_FRAGMENT_BIT, 4),
		};

		VkDescriptorSetLayoutCreateInfo descriptorLayout = vks::initializers::descriptorSetLayoutCreateInfo(setLayoutBindings);
		VK_CHECK_RESULT(vkCreateDescriptorSetLayout(device, &descriptorLayout, nullptr, &descriptorSetLayoutGfx));

		// Shared pipeline layout used by all graphics pipelines
		VkPipelineLayoutCreateInfo pPipelineLayoutCreateInfo = vks::initializers::pipelineLayoutCreateInfo(&descriptorSetLayoutGfx, 1);
		VK_CHECK_RESULT(vkCreatePipelineLayout(device, &pPipelineLayoutCreateInfo, nullptr, &pipelineLayoutGfx));
	}

	void setupDescriptorSetGfx()
	{
		std::vector<VkWriteDescriptorSet> writeDescriptorSets;
		VkDescriptorSetAllocateInfo allocInfo = vks::initializers::descriptorSetAllocateInfo(descriptorPool, &descriptorSetLayoutGfx, 1);

		// Image descriptors for the offscreen color attachments
		VkDescriptorImageInfo texDescriptorPosition =
			vks::initializers::descriptorImageInfo(
				colorSampler,
				offScreenFrameBuf.position.view,
				VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

		VkDescriptorImageInfo texDescriptorNormal =
			vks::initializers::descriptorImageInfo(
				colorSampler,
				offScreenFrameBuf.normal.view,
				VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

		VkDescriptorImageInfo texDescriptorAlbedo =
			vks::initializers::descriptorImageInfo(
				colorSampler,
				offScreenFrameBuf.albedo.view,
				VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

		// Deferred composition
		VK_CHECK_RESULT(vkAllocateDescriptorSets(device, &allocInfo, &descriptorSetGfx));
		writeDescriptorSets = {
			// Binding 1 : Position texture target
			vks::initializers::writeDescriptorSet(descriptorSetGfx, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, &texDescriptorPosition),
			// Binding 2 : Normals texture target
			vks::initializers::writeDescriptorSet(descriptorSetGfx, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2, &texDescriptorNormal),
			// Binding 3 : Albedo texture target
			vks::initializers::writeDescriptorSet(descriptorSetGfx, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 3, &texDescriptorAlbedo),
			// Binding 4 : Fragment shader uniform buffer
			vks::initializers::writeDescriptorSet(descriptorSetGfx, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 4, &uniformBuffers.composition.descriptor),
		};
		vkUpdateDescriptorSets(device, static_cast<uint32_t>(writeDescriptorSets.size()), writeDescriptorSets.data(), 0, nullptr);

		// Offscreen (scene)

		// Model
		VK_CHECK_RESULT(vkAllocateDescriptorSets(device, &allocInfo, &descriptorSets.model));
		writeDescriptorSets = {
			// Binding 0: Vertex shader uniform buffer
		    vks::initializers::writeDescriptorSet(descriptorSets.model, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 0, &uniformBuffers.offscreen.descriptor),
			// Binding 1: Color map
			vks::initializers::writeDescriptorSet(descriptorSets.model, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, &textures.model.colorMap.descriptor),
			// Binding 2: Normal map
			vks::initializers::writeDescriptorSet(descriptorSets.model, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2, &textures.model.normalMap.descriptor)
		};
		vkUpdateDescriptorSets(device, static_cast<uint32_t>(writeDescriptorSets.size()), writeDescriptorSets.data(), 0, nullptr);

		// Background
		VK_CHECK_RESULT(vkAllocateDescriptorSets(device, &allocInfo, &descriptorSets.floor));
		writeDescriptorSets = {
			// Binding 0: Vertex shader uniform buffer
		    vks::initializers::writeDescriptorSet(descriptorSets.floor, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 0, &uniformBuffers.offscreen.descriptor),
			// Binding 1: Color map
			vks::initializers::writeDescriptorSet(descriptorSets.floor, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, &textures.floor.colorMap.descriptor),
			// Binding 2: Normal map
			vks::initializers::writeDescriptorSet(descriptorSets.floor, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2, &textures.floor.normalMap.descriptor)
		};
        vkUpdateDescriptorSets(device, static_cast<uint32_t>(writeDescriptorSets.size()), writeDescriptorSets.data(), 0, nullptr);
	}

    void setupDescriptorSetRt()
    {
        std::vector<VkWriteDescriptorSet> writeDescriptorSets;
        VkDescriptorSetAllocateInfo allocInfo = vks::initializers::descriptorSetAllocateInfo(descriptorPool, &descriptorSetLayoutRt, 1);

        // RayTracing
        VkResult res = vkAllocateDescriptorSets(device, &allocInfo, &descriptorSetRt);

        VkWriteDescriptorSetAccelerationStructureKHR descriptorAccelerationStructureInfo = vks::initializers::writeDescriptorSetAccelerationStructureKHR();
        descriptorAccelerationStructureInfo.accelerationStructureCount = 1;
        descriptorAccelerationStructureInfo.pAccelerationStructures = &topLevelAS.handle;

        VkWriteDescriptorSet accelerationStructureWrite{};
        accelerationStructureWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        // The specialized acceleration structure descriptor has to be chained
        accelerationStructureWrite.pNext = &descriptorAccelerationStructureInfo;
        accelerationStructureWrite.dstSet = descriptorSetRt;
        accelerationStructureWrite.dstBinding = 0;
        accelerationStructureWrite.descriptorCount = 1;
        accelerationStructureWrite.descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;

        VkDescriptorImageInfo storageImageDescriptor{ VK_NULL_HANDLE, storageImage.view, VK_IMAGE_LAYOUT_GENERAL };
        VkDescriptorBufferInfo vertexBufferDescriptorModel{ models.model.vertices.buffer, 0, VK_WHOLE_SIZE };
        VkDescriptorBufferInfo indexBufferDescriptorModel{ models.model.indices.buffer, 0, VK_WHOLE_SIZE };
        VkDescriptorBufferInfo vertexBufferDescriptorFloor{ models.floor.vertices.buffer, 0, VK_WHOLE_SIZE };
        VkDescriptorBufferInfo indexBufferDescriptorFloor{ models.floor.indices.buffer, 0, VK_WHOLE_SIZE };

        writeDescriptorSets = {
            // Binding 0: Top level acceleration structure
            accelerationStructureWrite,
            // Binding 1: Ray tracing result image
            vks::initializers::writeDescriptorSet(descriptorSetRt, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, &storageImageDescriptor),
            // Binding 2: Uniform data
            vks::initializers::writeDescriptorSet(descriptorSetRt, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 2, &ubo.descriptor),
            // Binding 3: Scene vertex buffer
            vks::initializers::writeDescriptorSet(descriptorSetRt, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 3, &vertexBufferDescriptorModel),
            // Binding 4: Scene index buffer
            vks::initializers::writeDescriptorSet(descriptorSetRt, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 4, &indexBufferDescriptorModel),
            // Binding 5: Color map
            vks::initializers::writeDescriptorSet(descriptorSetRt, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 5, &textures.model.colorMap.descriptor),
            // Binding 6: Normal map
            vks::initializers::writeDescriptorSet(descriptorSetRt, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 6, &textures.model.normalMap.descriptor),
            // Binding 7: Scene vertex buffer (floor)
            vks::initializers::writeDescriptorSet(descriptorSetRt, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 7, &vertexBufferDescriptorFloor),
            // Binding 8: Scene index buffer (floor)
            vks::initializers::writeDescriptorSet(descriptorSetRt, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 8, &indexBufferDescriptorFloor),
            // Binding 9: Color map (floor)
            vks::initializers::writeDescriptorSet(descriptorSetRt, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 9, &textures.floor.colorMap.descriptor),
            // Binding 10: Normal map (floor)
		    vks::initializers::writeDescriptorSet(descriptorSetRt, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 10, &textures.floor.normalMap.descriptor),
		    // Binding 11 : Fragment shader uniform buffer
		    vks::initializers::writeDescriptorSet(descriptorSetRt, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 11, &uniformBuffers.composition.descriptor),
        };

        vkUpdateDescriptorSets(device, static_cast<uint32_t>(writeDescriptorSets.size()), writeDescriptorSets.data(), 0, nullptr);
    }

	void preparePipelines()
	{
		VkPipelineInputAssemblyStateCreateInfo inputAssemblyState = vks::initializers::pipelineInputAssemblyStateCreateInfo(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, 0, VK_FALSE);
		VkPipelineRasterizationStateCreateInfo rasterizationState = vks::initializers::pipelineRasterizationStateCreateInfo(VK_POLYGON_MODE_FILL, VK_CULL_MODE_BACK_BIT, VK_FRONT_FACE_COUNTER_CLOCKWISE, 0);
		VkPipelineColorBlendAttachmentState blendAttachmentState = vks::initializers::pipelineColorBlendAttachmentState(0xf, VK_FALSE);
		VkPipelineColorBlendStateCreateInfo colorBlendState = vks::initializers::pipelineColorBlendStateCreateInfo(1, &blendAttachmentState);
		VkPipelineDepthStencilStateCreateInfo depthStencilState = vks::initializers::pipelineDepthStencilStateCreateInfo(VK_TRUE, VK_TRUE, VK_COMPARE_OP_LESS_OR_EQUAL);
		VkPipelineViewportStateCreateInfo viewportState = vks::initializers::pipelineViewportStateCreateInfo(1, 1, 0);
		VkPipelineMultisampleStateCreateInfo multisampleState = vks::initializers::pipelineMultisampleStateCreateInfo(VK_SAMPLE_COUNT_1_BIT, 0);
		std::vector<VkDynamicState> dynamicStateEnables = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
		VkPipelineDynamicStateCreateInfo dynamicState = vks::initializers::pipelineDynamicStateCreateInfo(dynamicStateEnables);
		std::array<VkPipelineShaderStageCreateInfo, 2> shaderStages;

		VkGraphicsPipelineCreateInfo pipelineCI = vks::initializers::pipelineCreateInfo(pipelineLayoutGfx, renderPass);
		pipelineCI.pInputAssemblyState = &inputAssemblyState;
		pipelineCI.pRasterizationState = &rasterizationState;
		pipelineCI.pColorBlendState = &colorBlendState;
		pipelineCI.pMultisampleState = &multisampleState;
		pipelineCI.pViewportState = &viewportState;
		pipelineCI.pDepthStencilState = &depthStencilState;
		pipelineCI.pDynamicState = &dynamicState;
		pipelineCI.stageCount = static_cast<uint32_t>(shaderStages.size());
		pipelineCI.pStages = shaderStages.data();

		// Final fullscreen composition pass pipeline
		rasterizationState.cullMode = VK_CULL_MODE_FRONT_BIT;
		shaderStages[0] = loadShader(getShadersPath() + "deferred/deferred.vert.spv", VK_SHADER_STAGE_VERTEX_BIT);
		shaderStages[1] = loadShader(getShadersPath() + "deferred/deferred.frag.spv", VK_SHADER_STAGE_FRAGMENT_BIT);
		// Empty vertex input state, vertices are generated by the vertex shader
		VkPipelineVertexInputStateCreateInfo emptyInputState = vks::initializers::pipelineVertexInputStateCreateInfo();
		pipelineCI.pVertexInputState = &emptyInputState;
		VK_CHECK_RESULT(vkCreateGraphicsPipelines(device, pipelineCache, 1, &pipelineCI, nullptr, &pipelines.composition));

		// Vertex input state from glTF model for pipeline rendering models
		pipelineCI.pVertexInputState = vkglTF::Vertex::getPipelineVertexInputState({vkglTF::VertexComponent::Position, vkglTF::VertexComponent::UV, vkglTF::VertexComponent::Color, vkglTF::VertexComponent::Normal, vkglTF::VertexComponent::Tangent});
		rasterizationState.cullMode = VK_CULL_MODE_BACK_BIT;

		// Offscreen pipeline
		shaderStages[0] = loadShader(getShadersPath() + "deferred/mrt.vert.spv", VK_SHADER_STAGE_VERTEX_BIT);
		shaderStages[1] = loadShader(getShadersPath() + "deferred/mrt.frag.spv", VK_SHADER_STAGE_FRAGMENT_BIT);

		// Separate render pass
		pipelineCI.renderPass = offScreenFrameBuf.renderPass;

		// Blend attachment states required for all color attachments
		// This is important, as color write mask will otherwise be 0x0 and you
		// won't see anything rendered to the attachment
		std::array<VkPipelineColorBlendAttachmentState, 3> blendAttachmentStates = {
			vks::initializers::pipelineColorBlendAttachmentState(0xf, VK_FALSE),
			vks::initializers::pipelineColorBlendAttachmentState(0xf, VK_FALSE),
			vks::initializers::pipelineColorBlendAttachmentState(0xf, VK_FALSE)
		};

		colorBlendState.attachmentCount = static_cast<uint32_t>(blendAttachmentStates.size());
		colorBlendState.pAttachments = blendAttachmentStates.data();

		VK_CHECK_RESULT(vkCreateGraphicsPipelines(device, pipelineCache, 1, &pipelineCI, nullptr, &pipelines.offscreen));
	}

    void createRayTracingPipeline()
    {
        /*
            Setup ray tracing shader groups
        */
        std::vector<VkPipelineShaderStageCreateInfo> shaderStages;

        // Ray generation group
        {
            shaderStages.push_back(loadShader(getShadersPath() + "deferred/raygen.rgen.spv", VK_SHADER_STAGE_RAYGEN_BIT_KHR));
            VkRayTracingShaderGroupCreateInfoKHR shaderGroup{};
            shaderGroup.sType = VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR;
            shaderGroup.type = VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR;
            shaderGroup.generalShader = static_cast<uint32_t>(shaderStages.size()) - 1;
            shaderGroup.closestHitShader = VK_SHADER_UNUSED_KHR;
            shaderGroup.anyHitShader = VK_SHADER_UNUSED_KHR;
            shaderGroup.intersectionShader = VK_SHADER_UNUSED_KHR;
            shaderGroups.push_back(shaderGroup);
        }

        // Miss group
        {
            shaderStages.push_back(loadShader(getShadersPath() + "deferred/miss.rmiss.spv", VK_SHADER_STAGE_MISS_BIT_KHR));
            VkRayTracingShaderGroupCreateInfoKHR shaderGroup{};
            shaderGroup.sType = VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR;
            shaderGroup.type = VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR;
            shaderGroup.generalShader = static_cast<uint32_t>(shaderStages.size()) - 1;
            shaderGroup.closestHitShader = VK_SHADER_UNUSED_KHR;
            shaderGroup.anyHitShader = VK_SHADER_UNUSED_KHR;
            shaderGroup.intersectionShader = VK_SHADER_UNUSED_KHR;
            shaderGroups.push_back(shaderGroup);
            // Second shader for shadows
            shaderStages.push_back(loadShader(getShadersPath() + "deferred/shadow.rmiss.spv", VK_SHADER_STAGE_MISS_BIT_KHR));
            shaderGroup.generalShader = static_cast<uint32_t>(shaderStages.size()) - 1;
            shaderGroups.push_back(shaderGroup);
        }

        // Closest hit group
        {
            // Floor shader
            shaderStages.push_back(loadShader(getShadersPath() + "deferred/chit_floor.rchit.spv", VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR));
            VkRayTracingShaderGroupCreateInfoKHR shaderGroup{};
            shaderGroup.sType = VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR;
            shaderGroup.type = VK_RAY_TRACING_SHADER_GROUP_TYPE_TRIANGLES_HIT_GROUP_KHR;
            shaderGroup.generalShader = VK_SHADER_UNUSED_KHR;
            shaderGroup.closestHitShader = static_cast<uint32_t>(shaderStages.size()) - 1;
            shaderGroup.anyHitShader = VK_SHADER_UNUSED_KHR;
            shaderGroup.intersectionShader = VK_SHADER_UNUSED_KHR;
            shaderGroups.push_back(shaderGroup);
            // Model shader
            shaderStages.push_back(loadShader(getShadersPath() + "deferred/chit_model.rchit.spv", VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR));
            shaderGroup.closestHitShader = static_cast<uint32_t>(shaderStages.size()) - 1;
            shaderGroups.push_back(shaderGroup);
            // Lights shader
            shaderGroup.type = VK_RAY_TRACING_SHADER_GROUP_TYPE_PROCEDURAL_HIT_GROUP_KHR;
            shaderStages.push_back(loadShader(getShadersPath() + "deferred/chit_light.rchit.spv", VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR));
            shaderGroup.closestHitShader = static_cast<uint32_t>(shaderStages.size()) - 1;
            shaderStages.push_back(loadShader(getShadersPath() + "deferred/rint_light.rint.spv", VK_SHADER_STAGE_INTERSECTION_BIT_KHR));
            shaderGroup.intersectionShader = static_cast<uint32_t>(shaderStages.size()) - 1;
            shaderGroups.push_back(shaderGroup);
        }

        VkRayTracingPipelineCreateInfoKHR rayTracingPipelineCI = vks::initializers::rayTracingPipelineCreateInfoKHR();
        rayTracingPipelineCI.stageCount = static_cast<uint32_t>(shaderStages.size());
        rayTracingPipelineCI.pStages = shaderStages.data();
        rayTracingPipelineCI.groupCount = static_cast<uint32_t>(shaderGroups.size());
        rayTracingPipelineCI.pGroups = shaderGroups.data();
        rayTracingPipelineCI.maxPipelineRayRecursionDepth = 1;
        rayTracingPipelineCI.layout = pipelineLayoutRt;
        VK_CHECK_RESULT(vkCreateRayTracingPipelinesKHR(device, VK_NULL_HANDLE, VK_NULL_HANDLE, 1, &rayTracingPipelineCI, nullptr, &pipelines.raytracing));
    }

	// Prepare and initialize uniform buffer containing shader uniforms
	void prepareUniformBuffers()
	{
		// Offscreen vertex shader
		VK_CHECK_RESULT(vulkanDevice->createBuffer(
			VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
			VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
		    &uniformBuffers.offscreen,
			sizeof(uboOffscreenVS)));

		// Deferred fragment shader
		VK_CHECK_RESULT(vulkanDevice->createBuffer(
			VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
			VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
		    &uniformBuffers.composition,
			sizeof(uboComposition)));

		// Map persistent
		VK_CHECK_RESULT(uniformBuffers.offscreen.map());
		VK_CHECK_RESULT(uniformBuffers.composition.map());

		// Setup instanced model positions
		uboOffscreenVS.instancePos[0] = glm::vec4(0.0f);
		uboOffscreenVS.instancePos[1] = glm::vec4(-4.0f, 0.0, -4.0f, 0.0f);
		uboOffscreenVS.instancePos[2] = glm::vec4(4.0f, 0.0, -4.0f, 0.0f);

		// Update
		updateUniformBufferOffscreen();
		updateUniformBufferComposition();
	}

	// Update matrices used for the offscreen rendering of the scene
	void updateUniformBufferOffscreen()
	{
		uboOffscreenVS.projection = camera.matrices.perspective;
		uboOffscreenVS.view = camera.matrices.view;
		uboOffscreenVS.model = glm::mat4(1.0f);
		memcpy(uniformBuffers.offscreen.mapped, &uboOffscreenVS, sizeof(uboOffscreenVS));
	}

	// Update lights and parameters passed to the composition shaders
	void updateUniformBufferComposition()
	{
		// White
		uboComposition.lights[0].position = glm::vec4(0.0f, 0.0f, 1.0f, 0.0f);
		uboComposition.lights[0].color = glm::vec3(1.5f);
		uboComposition.lights[0].radius = 15.0f * 0.25f;
		// Red
		uboComposition.lights[1].position = glm::vec4(-2.0f, 0.0f, 0.0f, 0.0f);
		uboComposition.lights[1].color = glm::vec3(1.0f, 0.0f, 0.0f);
		uboComposition.lights[1].radius = 15.0f;
		// Blue
		uboComposition.lights[2].position = glm::vec4(2.0f, -1.0f, 0.0f, 0.0f);
		uboComposition.lights[2].color = glm::vec3(0.0f, 0.0f, 2.5f);
		uboComposition.lights[2].radius = 5.0f;
		// Yellow
		uboComposition.lights[3].position = glm::vec4(0.0f, -0.9f, 0.5f, 0.0f);
		uboComposition.lights[3].color = glm::vec3(1.0f, 1.0f, 0.0f);
		uboComposition.lights[3].radius = 2.0f;
		// Green
		uboComposition.lights[4].position = glm::vec4(0.0f, -0.5f, 0.0f, 0.0f);
		uboComposition.lights[4].color = glm::vec3(0.0f, 1.0f, 0.2f);
		uboComposition.lights[4].radius = 5.0f;
		// Yellow
		uboComposition.lights[5].position = glm::vec4(0.0f, -1.0f, 0.0f, 0.0f);
		uboComposition.lights[5].color = glm::vec3(1.0f, 0.7f, 0.3f);
		uboComposition.lights[5].radius = 25.0f;

		uboComposition.lights[0].position.x = sin(glm::radians(360.0f * timer)) * 2.0f;
		uboComposition.lights[0].position.z = cos(glm::radians(360.0f * timer)) * 2.0f;

		uboComposition.lights[1].position.x = -4.0f + sin(glm::radians(360.0f * timer) + 45.0f) * 2.0f;
		uboComposition.lights[1].position.z =  0.0f + cos(glm::radians(360.0f * timer) + 45.0f) * 2.0f;

		uboComposition.lights[2].position.x = 4.0f + sin(glm::radians(360.0f * timer)) * 2.0f;
		uboComposition.lights[2].position.z = 0.0f + cos(glm::radians(360.0f * timer)) * 2.0f;

		uboComposition.lights[4].position.x = 0.0f + sin(glm::radians(360.0f * timer + 90.0f)) * 5.0f;
		uboComposition.lights[4].position.z = 0.0f - cos(glm::radians(360.0f * timer + 45.0f)) * 5.0f;

		uboComposition.lights[5].position.x = 0.0f + sin(glm::radians(-360.0f * timer + 135.0f)) * 10.0f;
		uboComposition.lights[5].position.z = 0.0f - cos(glm::radians(-360.0f * timer - 45.0f)) * 10.0f;

		// Current view position
		uboComposition.viewPos = glm::vec4(camera.position, 0.0f) * glm::vec4(-1.0f, 1.0f, -1.0f, 1.0f);

		uboComposition.debugDisplayTarget = debugDisplayTarget;

        uboComposition.frame++;

		memcpy(uniformBuffers.composition.mapped, &uboComposition, sizeof(uboComposition));
	}

	void draw()
	{
		VulkanExampleBase::prepareFrame();

		// The scene render command buffer has to wait for the offscreen
		// rendering to be finished before we can use the framebuffer
		// color image for sampling during final rendering
		// To ensure this we use a dedicated offscreen synchronization
		// semaphore that will be signaled when offscreen renderin
		// has been finished
		// This is necessary as an implementation may start both
		// command buffers at the same time, there is no guarantee
		// that command buffers will be executed in the order they
		// have been submitted by the application

		// Offscreen rendering

		// Wait for swap chain presentation to finish
		submitInfo.pWaitSemaphores = &semaphores.presentComplete;
		// Signal ready with offscreen semaphore
		submitInfo.pSignalSemaphores = &offscreenSemaphore;

		// Submit work
		submitInfo.commandBufferCount = 1;
		submitInfo.pCommandBuffers = &offScreenCmdBuffer;
		VK_CHECK_RESULT(vkQueueSubmit(queue, 1, &submitInfo, VK_NULL_HANDLE));

		// Scene rendering

		// Wait for offscreen semaphore
		submitInfo.pWaitSemaphores = &offscreenSemaphore;
		// Signal ready with render complete semaphore
		submitInfo.pSignalSemaphores = &semaphores.renderComplete;

		// Submit work
		submitInfo.pCommandBuffers = &drawCmdBuffers[currentBuffer];
		VK_CHECK_RESULT(vkQueueSubmit(queue, 1, &submitInfo, VK_NULL_HANDLE));

		VulkanExampleBase::submitFrame();
	}

	void prepare()
	{
        VulkanRaytracingSample::prepare();

        loadAssets();

        // Create the acceleration structures used to render the ray traced scene
        createBottomLevelAccelerationStructure();
        createTopLevelAccelerationStructure();

        createStorageImage(swapChain.colorFormat, { width, height, 1 });
        createUniformBufferRayTracing();

		prepareOffscreenFramebuffer();
		prepareUniformBuffers();
		setupDescriptorSetLayoutGfx();
        setupDescriptorSetLayoutRt();
		preparePipelines();

        createRayTracingPipeline();
        createShaderBindingTables();

		setupDescriptorPool();
		setupDescriptorSetGfx();
        setupDescriptorSetRt();
		buildCommandBuffers(0);
		buildDeferredCommandBuffer();
		prepared = true;
	}

	virtual void render()
	{
		if (!prepared)
			return;
		draw();
		if (!paused)
		{
			updateUniformBufferComposition();
            updateUniformBufferRayTracing();
		}
		if (camera.updated)
		{
			updateUniformBufferOffscreen();
		}
	}

    virtual void keyPressed(uint32_t key)
    {
        switch (key)
        {
        case KEY_B:
            buildCommandBuffers(0);
            break;
        case KEY_N:
            buildCommandBuffers(1);
            break;
        default:
            break;
        }
    }

	virtual void OnUpdateUIOverlay(vks::UIOverlay *overlay)
	{
		if (overlay->header("Settings")) {
			if (overlay->comboBox("Display", &debugDisplayTarget, {"Final composition", "Position", "Normals", "Albedo", "Specular" }))
			{
				updateUniformBufferComposition();
			}
		}
	}
};

VULKAN_EXAMPLE_MAIN()
