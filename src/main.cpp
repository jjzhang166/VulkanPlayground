/*
* Vulkan Playground
*
* Copyright (C) Sascha Willems - www.saschawillems.de
*
* This code is licensed under the MIT license (MIT) (http://opensource.org/licenses/MIT)
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <vector>

#define GLM_FORCE_RADIANS
#define GLM_FORCE_DEPTH_ZERO_TO_ONE
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <vulkan/vulkan.h>
#include "vulkanexamplebase.h"
#include "VulkanTexture.hpp"
#include "VulkanglTFModel.hpp"
#include "VulkanBuffer.hpp"
#include "VulkanHeightmap.hpp"

#include "Pipeline.hpp"
#include "PipelineLayout.hpp"
#include "DescriptorSet.hpp"
#include "DescriptorSetLayout.hpp"

#define ENABLE_VALIDATION false

#define FB_DIM 1024

#define TERRAIN_LAYER_COUNT 6

#if defined(__ANDROID__)
#define SHADOWMAP_DIM 2048
#else
#define SHADOWMAP_DIM 4096
#endif

#define SHADOW_MAP_CASCADE_COUNT 4

class VulkanExample : public VulkanExampleBase
{
public:
	bool debugDisplayReflection = false;
	bool debugDisplayRefraction = false;

	vks::HeightMap* heightMap;

	glm::vec4 lightPos;

	enum class SceneDrawType { sceneDrawTypeRefract, sceneDrawTypeReflect, sceneDrawTypeDisplay };

	struct CascadeDebug {
		bool enabled = false;
		int32_t cascadeIndex = 0;
		Pipeline* pipeline;
		PipelineLayout* pipelineLayout;
		DescriptorSet* descriptorSet;
		DescriptorSetLayout* descriptorSetLayout;
	} cascadeDebug;

	struct {
		Pipeline* debug;
		Pipeline* mirror;
		Pipeline* terrain;
		Pipeline* sky;
		Pipeline* depthpass;
	} pipelines;

	struct Textures {
		vks::Texture2D heightMap;
		vks::Texture2D skySphere;
		vks::Texture2D waterNormalMap;
		vks::Texture2DArray terrainArray;
	} textures;

	std::vector<vks::Texture2D> skyspheres;
	int32_t skysphereIndex;

	struct Models {
		vkglTF::Model skysphere;
		vkglTF::Model plane;
		vkglTF::Model testscene;
	} models;

	struct {
		vks::Buffer vsShared;
		vks::Buffer vsMirror;
		vks::Buffer vsOffScreen;
		vks::Buffer vsDebugQuad;
		vks::Buffer terrain;
		vks::Buffer sky;
		vks::Buffer CSM;
	} uniformBuffers;

	struct UBO {
		glm::mat4 projection;
		glm::mat4 model;
		glm::vec4 lightDir = glm::vec4(10.0f, 10.0f, 10.0f, 1.0f);
	} uboShared, uboSky;

	struct UBOTerrain {
		glm::mat4 projection;
		glm::mat4 model;
		glm::vec4 lightDir = glm::vec4(10.0f, 10.0f, 10.0f, 1.0f);
		glm::vec4 layers[TERRAIN_LAYER_COUNT];
	} uboTerrain;

	struct UBOCSM {
		float cascadeSplits[SHADOW_MAP_CASCADE_COUNT];
		glm::mat4 cascadeViewProjMat[SHADOW_MAP_CASCADE_COUNT];
		glm::mat4 inverseViewMat;
		glm::vec3 lightDir;
	} uboCSM;

	struct UBOWaterPlane {
		glm::mat4 projection;
		glm::mat4 model;
		glm::vec4 cameraPos;
		glm::vec4 lightDir;
		float time;
	} uboWaterPlane;

	struct {
		PipelineLayout* debug;
		PipelineLayout* textured;
		PipelineLayout* terrain;
		PipelineLayout* sky;
	} pipelineLayouts;

	struct DescriptorSets {
		DescriptorSet* waterplane;
		DescriptorSet* debugquad;
		DescriptorSet* terrain;
		DescriptorSet* skysphere;
	} descriptorSets;

	struct {
		DescriptorSetLayout* textured;
		DescriptorSetLayout* terrain;
		DescriptorSetLayout* skysphere;
	} descriptorSetLayouts;

	// Framebuffer for offscreen rendering
	struct FrameBufferAttachment {
		VkFramebuffer frameBuffer;		
		VkDeviceMemory mem;
		VkImage image;
		VkImageView view;
		VkDescriptorImageInfo descriptor;
	};
	struct OffscreenPass {
		int32_t width, height;
		FrameBufferAttachment reflection, refraction, depth;
		VkRenderPass renderPass;
		VkSampler sampler;
	} offscreenPass;

	/* CSM */

	float cascadeSplitLambda = 0.95f;

	float zNear = 0.5f;
	float zFar = 48.0f;

	// Resources of the depth map generation pass
	struct CascadePushConstBlock {
		glm::vec4 position;
		uint32_t cascadeIndex;
	};
	struct DepthPass {
		VkRenderPass renderPass;
		PipelineLayout *pipelineLayout;
		VkPipeline pipeline;
		vks::Buffer uniformBuffer;
		DescriptorSetLayout *descriptorSetLayout;
		DescriptorSet* descriptorSet;
		struct UniformBlock {
			std::array<glm::mat4, SHADOW_MAP_CASCADE_COUNT> cascadeViewProjMat;
		} ubo;
	} depthPass;
	// Layered depth image containing the shadow cascade depths
	struct DepthImage {
		VkImage image;
		VkDeviceMemory mem;
		VkImageView view;
		VkSampler sampler;
		void destroy(VkDevice device) {
			vkDestroyImageView(device, view, nullptr);
			vkDestroyImage(device, image, nullptr);
			vkFreeMemory(device, mem, nullptr);
			vkDestroySampler(device, sampler, nullptr);
		}
	} depth;

	// Contains all resources required for a single shadow map cascade
	struct Cascade {
		VkFramebuffer frameBuffer;
		DescriptorSet* descriptorSet;
		VkImageView view;
		float splitDepth;
		glm::mat4 viewProjMatrix;
		void destroy(VkDevice device) {
			vkDestroyImageView(device, view, nullptr);
			vkDestroyFramebuffer(device, frameBuffer, nullptr);
		}
		VkDescriptorImageInfo descriptor;
	};
	std::array<Cascade, SHADOW_MAP_CASCADE_COUNT> cascades;

	VulkanExample() : VulkanExampleBase(ENABLE_VALIDATION)
	{
		title = "Vulkan Playground";
		camera.type = Camera::CameraType::firstperson;
		camera.setPerspective(45.0f, (float)width / (float)height, zNear, zFar);
		camera.setRotation(glm::vec3(0.0f, 0.0f, 0.0f));
		//camera.setTranslation(glm::vec3(18.0f, 22.5f, 57.5f));
		camera.setTranslation(glm::vec3(0.0f, 1.0f, -6.0f));
		camera.movementSpeed = 7.5f;
		settings.overlay = true;
		camera.setRotation({ -27.0000000, 0.000000000, 0.000000000 });
		camera.setPosition({ -0.0402765162, 7.17239332, -15.7546043 });
		timerSpeed *= 0.05f;
		// @todo
		camera.setPosition(glm::vec3(-0.12f, 1.14f, -2.25f));
		camera.setRotation(glm::vec3(-17.0f, 7.0f, 0.0f));
		// The scene shader uses a clipping plane, so this feature has to be enabled
		enabledFeatures.shaderClipDistance = VK_TRUE;
		enabledFeatures.samplerAnisotropy = VK_TRUE;
		enabledFeatures.depthClamp = VK_TRUE;	

		// @todo
		float radius = 20.0f;
		lightPos = glm::vec4(20.0f, -15.0f, -15.0f, 0.0f) * radius;
		lightPos = glm::vec4(-20.0f, -15.0f, -15.0f, 0.0f) * radius;
		uboTerrain.lightDir = glm::normalize(lightPos);

		// Terrain layers (x = start, y = range)
		uboTerrain.layers[0] = glm::vec4(12.5f, 45.0f, glm::vec2(0.0));
		uboTerrain.layers[1] = glm::vec4(50.0f, 30.0f, glm::vec2(0.0));
		uboTerrain.layers[2] = glm::vec4(62.5f, 35.0f, glm::vec2(0.0));
		uboTerrain.layers[3] = glm::vec4(87.5f, 25.0f, glm::vec2(0.0));
		uboTerrain.layers[4] = glm::vec4(117.5f, 45.0f, glm::vec2(0.0));
		uboTerrain.layers[5] = glm::vec4(165.0f, 50.0f, glm::vec2(0.0));
	}

	~VulkanExample()
	{
		// Clean up used Vulkan resources 
		// Note : Inherited destructor cleans up resources stored in base class

		// Frame buffer

		// Color attachment
		vkDestroyImageView(device, offscreenPass.refraction.view, nullptr);
		vkDestroyImage(device, offscreenPass.refraction.image, nullptr);
		vkFreeMemory(device, offscreenPass.refraction.mem, nullptr);

		// Depth attachment
		vkDestroyImageView(device, offscreenPass.depth.view, nullptr);
		vkDestroyImage(device, offscreenPass.depth.image, nullptr);
		vkFreeMemory(device, offscreenPass.depth.mem, nullptr);

		vkDestroyRenderPass(device, offscreenPass.renderPass, nullptr);
		vkDestroySampler(device, offscreenPass.sampler, nullptr);
		//vkDestroyFramebuffer(device, offscreenPass.frameBuffer, nullptr);

		// Uniform buffers
		uniformBuffers.vsShared.destroy();
		uniformBuffers.vsMirror.destroy();
		uniformBuffers.vsOffScreen.destroy();
		uniformBuffers.vsDebugQuad.destroy();
	}

	void createFrameBuffer(FrameBufferAttachment& target)
	{
		VkImageCreateInfo image = vks::initializers::imageCreateInfo();
		image.imageType = VK_IMAGE_TYPE_2D;
		image.format = swapChain.colorFormat;
		image.extent.width = offscreenPass.width;
		image.extent.height = offscreenPass.height;
		image.extent.depth = 1;
		image.mipLevels = 1;
		image.arrayLayers = 1;
		image.samples = VK_SAMPLE_COUNT_1_BIT;
		image.tiling = VK_IMAGE_TILING_OPTIMAL;
		image.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
		VK_CHECK_RESULT(vkCreateImage(device, &image, nullptr, &target.image));

		VkMemoryRequirements memReqs;
		vkGetImageMemoryRequirements(device, target.image, &memReqs);
		VkMemoryAllocateInfo memAlloc = vks::initializers::memoryAllocateInfo();
		memAlloc.allocationSize = memReqs.size;
		memAlloc.memoryTypeIndex = vulkanDevice->getMemoryType(memReqs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
		VK_CHECK_RESULT(vkAllocateMemory(device, &memAlloc, nullptr, &target.mem));
		VK_CHECK_RESULT(vkBindImageMemory(device, target.image, target.mem, 0));

		VkImageViewCreateInfo colorImageView = vks::initializers::imageViewCreateInfo();
		colorImageView.viewType = VK_IMAGE_VIEW_TYPE_2D;
		colorImageView.format = swapChain.colorFormat;
		colorImageView.subresourceRange = {};
		colorImageView.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		colorImageView.subresourceRange.baseMipLevel = 0;
		colorImageView.subresourceRange.levelCount = 1;
		colorImageView.subresourceRange.baseArrayLayer = 0;
		colorImageView.subresourceRange.layerCount = 1;
		colorImageView.image = target.image;
		VK_CHECK_RESULT(vkCreateImageView(device, &colorImageView, nullptr, &target.view));

		target.descriptor = { offscreenPass.sampler, target.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
	}

	// Setup the offscreen framebuffer for rendering the mirrored scene
	// The color attachment of this framebuffer will then be used to sample from in the fragment shader of the final pass
	void prepareOffscreen()
	{
		offscreenPass.width = FB_DIM;
		offscreenPass.height = FB_DIM;

		// Find a suitable depth format
		VkFormat fbDepthFormat;
		VkBool32 validDepthFormat = vks::tools::getSupportedDepthFormat(physicalDevice, &fbDepthFormat);
		assert(validDepthFormat);

		/* Renderpass */

		std::array<VkAttachmentDescription, 2> attchmentDescriptions = {};
		// Color attachment
		attchmentDescriptions[0].format = swapChain.colorFormat;
		attchmentDescriptions[0].samples = VK_SAMPLE_COUNT_1_BIT;
		attchmentDescriptions[0].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
		attchmentDescriptions[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
		attchmentDescriptions[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
		attchmentDescriptions[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
		attchmentDescriptions[0].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		attchmentDescriptions[0].finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		// Depth attachment
		attchmentDescriptions[1].format = fbDepthFormat;
		attchmentDescriptions[1].samples = VK_SAMPLE_COUNT_1_BIT;
		attchmentDescriptions[1].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
		attchmentDescriptions[1].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
		attchmentDescriptions[1].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
		attchmentDescriptions[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
		attchmentDescriptions[1].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		attchmentDescriptions[1].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

		VkAttachmentReference colorReference = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
		VkAttachmentReference depthReference = { 1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL };

		VkSubpassDescription subpassDescription = {};
		subpassDescription.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
		subpassDescription.colorAttachmentCount = 1;
		subpassDescription.pColorAttachments = &colorReference;
		subpassDescription.pDepthStencilAttachment = &depthReference;

		// Use subpass dependencies for layout transitions
		std::array<VkSubpassDependency, 2> dependencies;

		dependencies[0].srcSubpass = VK_SUBPASS_EXTERNAL;
		dependencies[0].dstSubpass = 0;
		dependencies[0].srcStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
		dependencies[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
		dependencies[0].srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
		dependencies[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
		dependencies[0].dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT;

		dependencies[1].srcSubpass = 0;
		dependencies[1].dstSubpass = VK_SUBPASS_EXTERNAL;
		dependencies[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
		dependencies[1].dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
		dependencies[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
		dependencies[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
		dependencies[1].dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT;

		VkRenderPassCreateInfo renderPassInfo = {};
		renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
		renderPassInfo.attachmentCount = static_cast<uint32_t>(attchmentDescriptions.size());
		renderPassInfo.pAttachments = attchmentDescriptions.data();
		renderPassInfo.subpassCount = 1;
		renderPassInfo.pSubpasses = &subpassDescription;
		renderPassInfo.dependencyCount = static_cast<uint32_t>(dependencies.size());
		renderPassInfo.pDependencies = dependencies.data();
		VK_CHECK_RESULT(vkCreateRenderPass(device, &renderPassInfo, nullptr, &offscreenPass.renderPass));
		
		/* Shared sampler */

		VkSamplerCreateInfo samplerInfo = vks::initializers::samplerCreateInfo();
		samplerInfo.magFilter = VK_FILTER_LINEAR;
		samplerInfo.minFilter = VK_FILTER_LINEAR;
		samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
		samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
		samplerInfo.addressModeV = samplerInfo.addressModeU;
		samplerInfo.addressModeW = samplerInfo.addressModeU;
		samplerInfo.mipLodBias = 0.0f;
		samplerInfo.maxAnisotropy = 1.0f;
		samplerInfo.minLod = 0.0f;
		samplerInfo.maxLod = 1.0f;
		samplerInfo.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
		VK_CHECK_RESULT(vkCreateSampler(device, &samplerInfo, nullptr, &offscreenPass.sampler));

		/* Color frame buffers */

		createFrameBuffer(offscreenPass.refraction);
		createFrameBuffer(offscreenPass.reflection);

		// Depth stencil attachment
		VkImageCreateInfo image = vks::initializers::imageCreateInfo();
		image.imageType = VK_IMAGE_TYPE_2D;
		image.format = fbDepthFormat;
		image.extent.width = offscreenPass.width;
		image.extent.height = offscreenPass.height;
		image.extent.depth = 1;
		image.mipLevels = 1;
		image.arrayLayers = 1;
		image.samples = VK_SAMPLE_COUNT_1_BIT;
		image.tiling = VK_IMAGE_TILING_OPTIMAL;
		image.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
		VK_CHECK_RESULT(vkCreateImage(device, &image, nullptr, &offscreenPass.depth.image));

		VkMemoryRequirements memReqs;
		vkGetImageMemoryRequirements(device, offscreenPass.depth.image, &memReqs);
		VkMemoryAllocateInfo memAlloc = vks::initializers::memoryAllocateInfo();
		memAlloc.allocationSize = memReqs.size;
		memAlloc.memoryTypeIndex = vulkanDevice->getMemoryType(memReqs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
		VK_CHECK_RESULT(vkAllocateMemory(device, &memAlloc, nullptr, &offscreenPass.depth.mem));
		VK_CHECK_RESULT(vkBindImageMemory(device, offscreenPass.depth.image, offscreenPass.depth.mem, 0));

		VkImageViewCreateInfo depthStencilView = vks::initializers::imageViewCreateInfo();
		depthStencilView.viewType = VK_IMAGE_VIEW_TYPE_2D;
		depthStencilView.format = fbDepthFormat;
		depthStencilView.flags = 0;
		depthStencilView.subresourceRange = {};
		depthStencilView.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
		depthStencilView.subresourceRange.baseMipLevel = 0;
		depthStencilView.subresourceRange.levelCount = 1;
		depthStencilView.subresourceRange.baseArrayLayer = 0;
		depthStencilView.subresourceRange.layerCount = 1;
		depthStencilView.image = offscreenPass.depth.image;
		VK_CHECK_RESULT(vkCreateImageView(device, &depthStencilView, nullptr, &offscreenPass.depth.view));

		/* Framebuffers */

		VkImageView attachments[2];
		attachments[0] = offscreenPass.refraction.view;
		attachments[1] = offscreenPass.depth.view;

		VkFramebufferCreateInfo frameBufferCI = vks::initializers::framebufferCreateInfo();
		frameBufferCI.renderPass = offscreenPass.renderPass;
		frameBufferCI.attachmentCount = 2;
		frameBufferCI.pAttachments = attachments;
		frameBufferCI.width = offscreenPass.width;
		frameBufferCI.height = offscreenPass.height;
		frameBufferCI.layers = 1;
		VK_CHECK_RESULT(vkCreateFramebuffer(device, &frameBufferCI, nullptr, &offscreenPass.refraction.frameBuffer));

		attachments[0] = offscreenPass.reflection.view;
		VK_CHECK_RESULT(vkCreateFramebuffer(device, &frameBufferCI, nullptr, &offscreenPass.reflection.frameBuffer));
	}

	void drawScene(CommandBuffer* cb, SceneDrawType drawType)
	{
		// @todo: rename to localMat
		struct PushConst {
			glm::mat4 scale = glm::mat4(1.0f);
			glm::vec4 clipPlane = glm::vec4(0.0f);
			uint32_t shadows = 1;
		} pushConst;
		if (drawType == SceneDrawType::sceneDrawTypeReflect) {
			pushConst.scale = glm::scale(pushConst.scale, glm::vec3(1.0f, -1.0f, 1.0f));
		}

		switch (drawType) {
		case SceneDrawType::sceneDrawTypeRefract:
			pushConst.clipPlane = glm::vec4(0.0, 1.0, 0.0, 0.0);
			pushConst.shadows = 0;
			break;
		case SceneDrawType::sceneDrawTypeReflect:
			pushConst.clipPlane = glm::vec4(0.0, 1.0, 0.0, 0.0);
			pushConst.shadows = 0;
			break;
		}

		// Skysphere
		cb->bindPipeline(pipelines.sky);
		cb->bindDescriptorSets(pipelineLayouts.sky, { descriptorSets.skysphere }, 0);
		cb->updatePushConstant(pipelineLayouts.sky, 0, &pushConst);
		models.skysphere.draw(cb->handle);
		
		// Terrain
		cb->bindPipeline(pipelines.terrain);
		cb->bindDescriptorSets(pipelineLayouts.terrain, { descriptorSets.terrain }, 0);
		cb->updatePushConstant(pipelineLayouts.terrain, 0, &pushConst);
		heightMap->draw(cb->handle);
	}

	void drawShadowCasters(CommandBuffer* cb, uint32_t cascadeIndex = 0) {
		const CascadePushConstBlock pushConst = { glm::vec4(0.0f), cascadeIndex };
		cb->bindPipeline(pipelines.depthpass);
		cb->bindDescriptorSets(depthPass.pipelineLayout, { depthPass.descriptorSet }, 0);
		cb->updatePushConstant(depthPass.pipelineLayout, 0, &pushConst);
		heightMap->draw(cb->handle);
	}

	/*
		CSM
	*/

	void prepareCSM()
	{
		VkFormat depthFormat;
		vks::tools::getSupportedDepthFormat(physicalDevice, &depthFormat);

		/*
			Depth map renderpass
		*/

		VkAttachmentDescription attachmentDescription{};
		attachmentDescription.format = depthFormat;
		attachmentDescription.samples = VK_SAMPLE_COUNT_1_BIT;
		attachmentDescription.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
		attachmentDescription.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
		attachmentDescription.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
		attachmentDescription.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
		attachmentDescription.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		attachmentDescription.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;

		VkAttachmentReference depthReference = {};
		depthReference.attachment = 0;
		depthReference.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

		VkSubpassDescription subpass = {};
		subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
		subpass.colorAttachmentCount = 0;
		subpass.pDepthStencilAttachment = &depthReference;

		// Use subpass dependencies for layout transitions
		std::array<VkSubpassDependency, 2> dependencies;

		dependencies[0].srcSubpass = VK_SUBPASS_EXTERNAL;
		dependencies[0].dstSubpass = 0;
		dependencies[0].srcStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
		dependencies[0].dstStageMask = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
		dependencies[0].srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
		dependencies[0].dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
		dependencies[0].dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT;

		dependencies[1].srcSubpass = 0;
		dependencies[1].dstSubpass = VK_SUBPASS_EXTERNAL;
		dependencies[1].srcStageMask = VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
		dependencies[1].dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
		dependencies[1].srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
		dependencies[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
		dependencies[1].dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT;

		VkRenderPassCreateInfo renderPassCreateInfo = vks::initializers::renderPassCreateInfo();
		renderPassCreateInfo.attachmentCount = 1;
		renderPassCreateInfo.pAttachments = &attachmentDescription;
		renderPassCreateInfo.subpassCount = 1;
		renderPassCreateInfo.pSubpasses = &subpass;
		renderPassCreateInfo.dependencyCount = static_cast<uint32_t>(dependencies.size());
		renderPassCreateInfo.pDependencies = dependencies.data();

		VK_CHECK_RESULT(vkCreateRenderPass(device, &renderPassCreateInfo, nullptr, &depthPass.renderPass));

		/*
			Layered depth image and views
		*/

		VkImageCreateInfo imageInfo = vks::initializers::imageCreateInfo();
		imageInfo.imageType = VK_IMAGE_TYPE_2D;
		imageInfo.extent.width = SHADOWMAP_DIM;
		imageInfo.extent.height = SHADOWMAP_DIM;
		imageInfo.extent.depth = 1;
		imageInfo.mipLevels = 1;
		imageInfo.arrayLayers = SHADOW_MAP_CASCADE_COUNT;
		imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
		imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
		imageInfo.format = depthFormat;
		imageInfo.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
		VK_CHECK_RESULT(vkCreateImage(device, &imageInfo, nullptr, &depth.image));
		VkMemoryAllocateInfo memAlloc = vks::initializers::memoryAllocateInfo();
		VkMemoryRequirements memReqs;
		vkGetImageMemoryRequirements(device, depth.image, &memReqs);
		memAlloc.allocationSize = memReqs.size;
		memAlloc.memoryTypeIndex = vulkanDevice->getMemoryType(memReqs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
		VK_CHECK_RESULT(vkAllocateMemory(device, &memAlloc, nullptr, &depth.mem));
		VK_CHECK_RESULT(vkBindImageMemory(device, depth.image, depth.mem, 0));
		// Full depth map view (all layers)
		VkImageViewCreateInfo viewInfo = vks::initializers::imageViewCreateInfo();
		viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
		viewInfo.format = depthFormat;
		viewInfo.subresourceRange = {};
		viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
		viewInfo.subresourceRange.baseMipLevel = 0;
		viewInfo.subresourceRange.levelCount = 1;
		viewInfo.subresourceRange.baseArrayLayer = 0;
		viewInfo.subresourceRange.layerCount = SHADOW_MAP_CASCADE_COUNT;
		viewInfo.image = depth.image;
		VK_CHECK_RESULT(vkCreateImageView(device, &viewInfo, nullptr, &depth.view));

		// One image and framebuffer per cascade
		for (uint32_t i = 0; i < SHADOW_MAP_CASCADE_COUNT; i++) {
			// Image view for this cascade's layer (inside the depth map)
			// This view is used to render to that specific depth image layer
			VkImageViewCreateInfo viewInfo = vks::initializers::imageViewCreateInfo();
			viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
			viewInfo.format = depthFormat;
			viewInfo.subresourceRange = {};
			viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
			viewInfo.subresourceRange.baseMipLevel = 0;
			viewInfo.subresourceRange.levelCount = 1;
			viewInfo.subresourceRange.baseArrayLayer = i;
			viewInfo.subresourceRange.layerCount = 1;
			viewInfo.image = depth.image;
			VK_CHECK_RESULT(vkCreateImageView(device, &viewInfo, nullptr, &cascades[i].view));
			// Framebuffer
			VkFramebufferCreateInfo framebufferInfo = vks::initializers::framebufferCreateInfo();
			framebufferInfo.renderPass = depthPass.renderPass;
			framebufferInfo.attachmentCount = 1;
			framebufferInfo.pAttachments = &cascades[i].view;
			framebufferInfo.width = SHADOWMAP_DIM;
			framebufferInfo.height = SHADOWMAP_DIM;
			framebufferInfo.layers = 1;
			VK_CHECK_RESULT(vkCreateFramebuffer(device, &framebufferInfo, nullptr, &cascades[i].frameBuffer));
		}

		// Shared sampler for cascade deoth reads
		VkSamplerCreateInfo sampler = vks::initializers::samplerCreateInfo();
		sampler.magFilter = VK_FILTER_LINEAR;
		sampler.minFilter = VK_FILTER_LINEAR;
		sampler.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
		sampler.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
		sampler.addressModeV = sampler.addressModeU;
		sampler.addressModeW = sampler.addressModeU;
		sampler.mipLodBias = 0.0f;
		sampler.maxAnisotropy = 1.0f;
		sampler.minLod = 0.0f;
		sampler.maxLod = 1.0f;
		sampler.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
		VK_CHECK_RESULT(vkCreateSampler(device, &sampler, nullptr, &depth.sampler));
	}

	/*
		Calculate frustum split depths and matrices for the shadow map cascades
		Based on https://johanmedestrom.wordpress.com/2016/03/18/opengl-cascaded-shadow-maps/
	*/
	void updateCascades()
	{
		float cascadeSplits[SHADOW_MAP_CASCADE_COUNT];

		float nearClip = camera.getNearClip();
		float farClip = camera.getFarClip();
		float clipRange = farClip - nearClip;

		float minZ = nearClip;
		float maxZ = nearClip + clipRange;

		float range = maxZ - minZ;
		float ratio = maxZ / minZ;

		// Calculate split depths based on view camera furstum
		// Based on method presentd in https://developer.nvidia.com/gpugems/GPUGems3/gpugems3_ch10.html
		for (uint32_t i = 0; i < SHADOW_MAP_CASCADE_COUNT; i++) {
			float p = (i + 1) / static_cast<float>(SHADOW_MAP_CASCADE_COUNT);
			float log = minZ * std::pow(ratio, p);
			float uniform = minZ + range * p;
			float d = cascadeSplitLambda * (log - uniform) + uniform;
			cascadeSplits[i] = (d - nearClip) / clipRange;
		}

		// Calculate orthographic projection matrix for each cascade
		float lastSplitDist = 0.0;
		for (uint32_t i = 0; i < SHADOW_MAP_CASCADE_COUNT; i++) {
			float splitDist = cascadeSplits[i];

			glm::vec3 frustumCorners[8] = {
				glm::vec3(-1.0f,  1.0f, -1.0f),
				glm::vec3(1.0f,  1.0f, -1.0f),
				glm::vec3(1.0f, -1.0f, -1.0f),
				glm::vec3(-1.0f, -1.0f, -1.0f),
				glm::vec3(-1.0f,  1.0f,  1.0f),
				glm::vec3(1.0f,  1.0f,  1.0f),
				glm::vec3(1.0f, -1.0f,  1.0f),
				glm::vec3(-1.0f, -1.0f,  1.0f),
			};

			// Project frustum corners into world space
			glm::mat4 invCam = glm::inverse(camera.matrices.perspective * camera.matrices.view);
			for (uint32_t i = 0; i < 8; i++) {
				glm::vec4 invCorner = invCam * glm::vec4(frustumCorners[i], 1.0f);
				frustumCorners[i] = invCorner / invCorner.w;
			}

			for (uint32_t i = 0; i < 4; i++) {
				glm::vec3 dist = frustumCorners[i + 4] - frustumCorners[i];
				frustumCorners[i + 4] = frustumCorners[i] + (dist * splitDist);
				frustumCorners[i] = frustumCorners[i] + (dist * lastSplitDist);
			}

			// Get frustum center
			glm::vec3 frustumCenter = glm::vec3(0.0f);
			for (uint32_t i = 0; i < 8; i++) {
				frustumCenter += frustumCorners[i];
			}
			frustumCenter /= 8.0f;

			float radius = 0.0f;
			for (uint32_t i = 0; i < 8; i++) {
				float distance = glm::length(frustumCorners[i] - frustumCenter);
				radius = glm::max(radius, distance);
			}
			radius = std::ceil(radius * 16.0f) / 16.0f;

			glm::vec3 maxExtents = glm::vec3(radius);
			glm::vec3 minExtents = -maxExtents;

			glm::vec3 lightDir = glm::normalize(-lightPos);
			glm::mat4 lightViewMatrix = glm::lookAt(frustumCenter - lightDir * -minExtents.z, frustumCenter, glm::vec3(0.0f, 1.0f, 0.0f));
			glm::mat4 lightOrthoMatrix = glm::ortho(minExtents.x, maxExtents.x, minExtents.y, maxExtents.y, 0.0f, maxExtents.z - minExtents.z);

			// Store split distance and matrix in cascade
			cascades[i].splitDepth = (camera.getNearClip() + splitDist * clipRange) * -1.0f;
			cascades[i].viewProjMatrix = lightOrthoMatrix * lightViewMatrix;

			lastSplitDist = cascadeSplits[i];
		}
	}

	void drawCSM(CommandBuffer *cb) {
		/*
			Generate depth map cascades

			Uses multiple passes with each pass rendering the scene to the cascade's depth image layer
			Could be optimized using a geometry shader (and layered frame buffer) on devices that support geometry shaders
		*/
		VkClearValue clearValues[1];
		clearValues[0].depthStencil = { 1.0f, 0 };

		VkRenderPassBeginInfo renderPassBeginInfo = vks::initializers::renderPassBeginInfo();
		renderPassBeginInfo.renderPass = depthPass.renderPass;
		renderPassBeginInfo.renderArea.offset.x = 0;
		renderPassBeginInfo.renderArea.offset.y = 0;
		renderPassBeginInfo.renderArea.extent.width = SHADOWMAP_DIM;
		renderPassBeginInfo.renderArea.extent.height = SHADOWMAP_DIM;
		renderPassBeginInfo.clearValueCount = 1;
		renderPassBeginInfo.pClearValues = clearValues;

		VkViewport viewport = vks::initializers::viewport((float)SHADOWMAP_DIM, (float)SHADOWMAP_DIM, 0.0f, 1.0f);
		vkCmdSetViewport(cb->handle, 0, 1, &viewport);

		VkRect2D scissor = vks::initializers::rect2D(SHADOWMAP_DIM, SHADOWMAP_DIM, 0, 0);
		vkCmdSetScissor(cb->handle, 0, 1, &scissor);

		// One pass per cascade
		// The layer that this pass renders to is defined by the cascade's image view (selected via the cascade's decsriptor set)
		for (uint32_t j = 0; j < SHADOW_MAP_CASCADE_COUNT; j++) {
			renderPassBeginInfo.framebuffer = cascades[j].frameBuffer;
			vkCmdBeginRenderPass(cb->handle, &renderPassBeginInfo, VK_SUBPASS_CONTENTS_INLINE);
			drawShadowCasters(cb, j);
			vkCmdEndRenderPass(cb->handle);
		}
	}

	/*
		Sample
	*/

	void buildCommandBuffers()
	{
		VkCommandBufferBeginInfo cmdBufInfo = vks::initializers::commandBufferBeginInfo();

		VkClearValue clearValues[2];
		VkViewport viewport;
		VkRect2D scissor;
		VkDeviceSize offsets[1] = { 0 };

		for (int32_t i = 0; i < commandBuffers.size(); i++) {
			CommandBuffer *cb = commandBuffers[i];
			cb->begin();

			/*
				CSM
			*/
			drawCSM(cb);

			/*
				Render refraction
			*/	
			{
				VkClearValue clearValues[2];
				clearValues[0].color = { { 0.0f, 0.0f, 0.0f, 0.0f } };
				clearValues[1].depthStencil = { 1.0f, 0 };

				VkRenderPassBeginInfo renderPassBeginInfo = vks::initializers::renderPassBeginInfo();
				renderPassBeginInfo.renderPass = offscreenPass.renderPass;
				renderPassBeginInfo.framebuffer = offscreenPass.refraction.frameBuffer;
				renderPassBeginInfo.renderArea.extent.width = offscreenPass.width;
				renderPassBeginInfo.renderArea.extent.height = offscreenPass.height;
				renderPassBeginInfo.clearValueCount = 2;
				renderPassBeginInfo.pClearValues = clearValues;

				vkCmdBeginRenderPass(cb->handle, &renderPassBeginInfo, VK_SUBPASS_CONTENTS_INLINE);
				cb->setViewport(0.0f, 0.0f, (float)offscreenPass.width, (float)offscreenPass.height, 0.0f, 1.0f);
				cb->setScissor(0, 0, offscreenPass.width, offscreenPass.height);
				drawScene(cb, SceneDrawType::sceneDrawTypeRefract);
				vkCmdEndRenderPass(cb->handle);
			}

			/*
				Render reflection
			*/
			{
				VkClearValue clearValues[2];
				clearValues[0].color = { { 0.0f, 0.0f, 0.0f, 0.0f } };
				clearValues[1].depthStencil = { 1.0f, 0 };

				VkRenderPassBeginInfo renderPassBeginInfo = vks::initializers::renderPassBeginInfo();
				renderPassBeginInfo.renderPass = offscreenPass.renderPass;
				renderPassBeginInfo.framebuffer = offscreenPass.reflection.frameBuffer;
				renderPassBeginInfo.renderArea.extent.width = offscreenPass.width;
				renderPassBeginInfo.renderArea.extent.height = offscreenPass.height;
				renderPassBeginInfo.clearValueCount = 2;
				renderPassBeginInfo.pClearValues = clearValues;

				vkCmdBeginRenderPass(cb->handle, &renderPassBeginInfo, VK_SUBPASS_CONTENTS_INLINE);
				cb->setViewport(0.0f, 0.0f, (float)offscreenPass.width, (float)offscreenPass.height, 0.0f, 1.0f);
				cb->setScissor(0, 0, offscreenPass.width, offscreenPass.height);
				drawScene(cb, SceneDrawType::sceneDrawTypeReflect);
				vkCmdEndRenderPass(cb->handle);
			}

			/*
				Scene rendering with reflection, refraction and shadows
			*/
			{
				clearValues[0].color = defaultClearColor;
				clearValues[1].depthStencil = { 1.0f, 0 };

				VkRenderPassBeginInfo renderPassBeginInfo = vks::initializers::renderPassBeginInfo();
				renderPassBeginInfo.renderPass = renderPass;
				renderPassBeginInfo.framebuffer = frameBuffers[i];
				renderPassBeginInfo.renderArea.extent.width = width;
				renderPassBeginInfo.renderArea.extent.height = height;
				renderPassBeginInfo.clearValueCount = 2;
				renderPassBeginInfo.pClearValues = clearValues;

				vkCmdBeginRenderPass(cb->handle, &renderPassBeginInfo, VK_SUBPASS_CONTENTS_INLINE);
				cb->setViewport(0.0f, 0.0f, (float)width, (float)height, 0.0f, 1.0f);
				cb->setScissor(0, 0, width, height);			
				drawScene(cb, SceneDrawType::sceneDrawTypeDisplay);
				// Reflection plane
				cb->bindDescriptorSets(pipelineLayouts.textured, { descriptorSets.waterplane }, 0);
				cb->bindPipeline(pipelines.mirror);
				models.plane.draw(cb->handle);

				if (debugDisplayReflection) {
					uint32_t val0 = 0;
					cb->bindDescriptorSets(pipelineLayouts.textured, { descriptorSets.debugquad }, 0);
					cb->bindPipeline(pipelines.debug);
					cb->updatePushConstant(pipelineLayouts.debug, 0, &val0);
					cb->draw(6, 1, 0, 0);
				}

				if (debugDisplayRefraction) {
					uint32_t val1 = 1;
					cb->bindDescriptorSets(pipelineLayouts.textured, { descriptorSets.debugquad }, 0);
					cb->bindPipeline(pipelines.debug);
					cb->updatePushConstant(pipelineLayouts.debug, 0, &val1);
					cb->draw(6, 1, 0, 0);
				}

				if (cascadeDebug.enabled) {
					const CascadePushConstBlock pushConst = { glm::vec4(0.0f), cascadeDebug.cascadeIndex };
					cb->bindDescriptorSets(cascadeDebug.pipelineLayout, { cascadeDebug.descriptorSet }, 0);
					cb->bindPipeline(cascadeDebug.pipeline);
					cb->updatePushConstant(cascadeDebug.pipelineLayout, 0, &pushConst);
					cb->draw(6, 1, 0, 0);
				}

				drawUI(cb->handle);

				vkCmdEndRenderPass(cb->handle);
			}
			cb->end();
		}
	}

	void loadAssets()
	{
		models.skysphere.loadFromFile(getAssetPath() + "scenes/geosphere.gltf", vulkanDevice, queue);
		models.plane.loadFromFile(getAssetPath() + "scenes/plane.gltf", vulkanDevice, queue);
		models.testscene.loadFromFile(getAssetPath() + "scenes/testscene.gltf", vulkanDevice, queue);
				
		textures.skySphere.loadFromFile(getAssetPath() + "textures/skysphere_02.ktx", VK_FORMAT_R8G8B8A8_UNORM, vulkanDevice, queue);
		textures.terrainArray.loadFromFile(getAssetPath() + "textures/terrain_layers_01_rgba.ktx", VK_FORMAT_R8G8B8A8_UNORM, vulkanDevice, queue);
		textures.heightMap.loadFromFile(getAssetPath() + "heightmap.ktx", VK_FORMAT_R16_UNORM, vulkanDevice, queue);
		textures.waterNormalMap.loadFromFile(getAssetPath() + "textures/water_normal_rgba.ktx", VK_FORMAT_R8G8B8A8_UNORM, vulkanDevice, queue);

		VkSamplerCreateInfo samplerInfo = vks::initializers::samplerCreateInfo();

		// Setup a mirroring sampler for the height map
		vkDestroySampler(device, textures.heightMap.sampler, nullptr);
		samplerInfo.magFilter = VK_FILTER_LINEAR;
		samplerInfo.minFilter = VK_FILTER_LINEAR;
		samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
		samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
		samplerInfo.addressModeV = samplerInfo.addressModeU;
		samplerInfo.addressModeW = samplerInfo.addressModeU;
		samplerInfo.compareOp = VK_COMPARE_OP_NEVER;
		samplerInfo.minLod = 0.0f;
		samplerInfo.maxLod = (float)textures.heightMap.mipLevels;
		samplerInfo.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
		VK_CHECK_RESULT(vkCreateSampler(device, &samplerInfo, nullptr, &textures.heightMap.sampler));
		textures.heightMap.descriptor.sampler = textures.heightMap.sampler;

		// Setup a repeating sampler for the terrain texture layers
		vkDestroySampler(device, textures.terrainArray.sampler, nullptr);
		samplerInfo = vks::initializers::samplerCreateInfo();
		samplerInfo.magFilter = VK_FILTER_LINEAR;
		samplerInfo.minFilter = VK_FILTER_LINEAR;
		samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
		samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
		samplerInfo.addressModeV = samplerInfo.addressModeU;
		samplerInfo.addressModeW = samplerInfo.addressModeU;
		samplerInfo.compareOp = VK_COMPARE_OP_NEVER;
		samplerInfo.minLod = 0.0f;
		samplerInfo.maxLod = (float)textures.terrainArray.mipLevels;
		samplerInfo.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
		if (deviceFeatures.samplerAnisotropy)
		{
			samplerInfo.maxAnisotropy = 4.0f;
			samplerInfo.anisotropyEnable = VK_TRUE;
		}
		VK_CHECK_RESULT(vkCreateSampler(device, &samplerInfo, nullptr, &textures.terrainArray.sampler));
		textures.terrainArray.descriptor.sampler = textures.terrainArray.sampler;
	}

	// Generate a terrain quad patch for feeding to the tessellation control shader
	void generateTerrain()
	{
		const glm::vec3 scale = glm::vec3(0.15f * 0.25f, 1.0f, 0.15f * 0.25f);
		const uint32_t patchSize = 256;
		heightMap = new vks::HeightMap(vulkanDevice, queue);
#if defined(__ANDROID__)
		heightMap->loadFromFile(getAssetPath() + "heightmap.ktx", patchSize, androidApp->activity->assetManager, scale, vks::HeightMap::topologyTriangles);
#else
		heightMap->loadFromFile(getAssetPath() + "heightmap.ktx", patchSize, scale, vks::HeightMap::topologyTriangles);
#endif
	}

	void setupDescriptorPool()
	{
		// @todo
		std::vector<VkDescriptorPoolSize> poolSizes = {
			vks::initializers::descriptorPoolSize(VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 6 * 25),
			vks::initializers::descriptorPoolSize(VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 8 * 25)
		};
		VkDescriptorPoolCreateInfo descriptorPoolInfo = vks::initializers::descriptorPoolCreateInfo( poolSizes.size(), poolSizes.data(), 5 * 10);
		VK_CHECK_RESULT(vkCreateDescriptorPool(device, &descriptorPoolInfo, nullptr, &descriptorPool));
	}

	void setupDescriptorSetLayout()
	{
		// Shared (use all layout bindings)
		descriptorSetLayouts.textured = new DescriptorSetLayout(device);
		descriptorSetLayouts.textured->addBinding(0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT);
		descriptorSetLayouts.textured->addBinding(1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_FRAGMENT_BIT);
		descriptorSetLayouts.textured->addBinding(2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_FRAGMENT_BIT);
		descriptorSetLayouts.textured->addBinding(3, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_FRAGMENT_BIT);
		descriptorSetLayouts.textured->addBinding(4, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_FRAGMENT_BIT);
		descriptorSetLayouts.textured->addBinding(5, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, VK_SHADER_STAGE_FRAGMENT_BIT);
		descriptorSetLayouts.textured->create();

		pipelineLayouts.textured = new PipelineLayout(device);
		pipelineLayouts.textured->addLayout(descriptorSetLayouts.textured);
		pipelineLayouts.textured->create();

		// Debug
		pipelineLayouts.debug = new PipelineLayout(device);
		pipelineLayouts.debug->addLayout(descriptorSetLayouts.textured);
		pipelineLayouts.debug->addPushConstantRange(sizeof(uint32_t), 0, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT);
		pipelineLayouts.debug->create();

		// Terrain
		descriptorSetLayouts.terrain = new DescriptorSetLayout(device);
		descriptorSetLayouts.terrain->addBinding(0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT);
		descriptorSetLayouts.terrain->addBinding(1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_FRAGMENT_BIT);
		descriptorSetLayouts.terrain->addBinding(2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_FRAGMENT_BIT);
		descriptorSetLayouts.terrain->addBinding(3, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_FRAGMENT_BIT);
		descriptorSetLayouts.terrain->addBinding(4, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, VK_SHADER_STAGE_FRAGMENT_BIT);
		descriptorSetLayouts.terrain->create();

		pipelineLayouts.terrain = new PipelineLayout(device);
		pipelineLayouts.terrain->addLayout(descriptorSetLayouts.terrain);
		pipelineLayouts.terrain->addPushConstantRange(sizeof(glm::mat4) + sizeof(glm::vec4) + sizeof(uint32_t), 0, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT);
		pipelineLayouts.terrain->create();

		// Skysphere
		descriptorSetLayouts.skysphere = new DescriptorSetLayout(device);
		descriptorSetLayouts.skysphere->addBinding(0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, VK_SHADER_STAGE_VERTEX_BIT);
		descriptorSetLayouts.skysphere->addBinding(1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_FRAGMENT_BIT);
		descriptorSetLayouts.skysphere->create();

		pipelineLayouts.sky = new PipelineLayout(device);
		pipelineLayouts.sky->addLayout(descriptorSetLayouts.skysphere);
		pipelineLayouts.sky->addPushConstantRange(sizeof(glm::mat4) + sizeof(glm::vec4) + sizeof(uint32_t), 0, VK_SHADER_STAGE_VERTEX_BIT);
		pipelineLayouts.sky->create();

		// Depth pass
		depthPass.descriptorSetLayout = new DescriptorSetLayout(device);
		depthPass.descriptorSetLayout->addBinding(0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, VK_SHADER_STAGE_VERTEX_BIT);
		depthPass.descriptorSetLayout->create();

		depthPass.pipelineLayout = new PipelineLayout(device);
		depthPass.pipelineLayout->addLayout(depthPass.descriptorSetLayout);
		depthPass.pipelineLayout->addPushConstantRange(sizeof(CascadePushConstBlock), 0, VK_SHADER_STAGE_VERTEX_BIT);
		depthPass.pipelineLayout->create();

		// Cascade debug
		cascadeDebug.descriptorSetLayout = new DescriptorSetLayout(device);
		cascadeDebug.descriptorSetLayout->addBinding(0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_FRAGMENT_BIT);
		cascadeDebug.descriptorSetLayout->create();

		cascadeDebug.pipelineLayout = new PipelineLayout(device);
		cascadeDebug.pipelineLayout->addLayout(cascadeDebug.descriptorSetLayout);
		cascadeDebug.pipelineLayout->addPushConstantRange(sizeof(glm::vec4) + sizeof(uint32_t), 0, VK_SHADER_STAGE_VERTEX_BIT);
		cascadeDebug.pipelineLayout->create();

	}

	void setupDescriptorSet()
	{
		VkDescriptorImageInfo depthMapDescriptor = vks::initializers::descriptorImageInfo(depth.sampler, depth.view, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL);

		// Water plane
		descriptorSets.waterplane = new DescriptorSet(device);
		descriptorSets.waterplane->setPool(descriptorPool);
		descriptorSets.waterplane->addLayout(descriptorSetLayouts.textured);
		descriptorSets.waterplane->addDescriptor(0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, &uniformBuffers.vsMirror.descriptor);
		descriptorSets.waterplane->addDescriptor(1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &offscreenPass.refraction.descriptor);
		descriptorSets.waterplane->addDescriptor(2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &offscreenPass.reflection.descriptor);
		descriptorSets.waterplane->addDescriptor(3, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &textures.waterNormalMap.descriptor);
		descriptorSets.waterplane->addDescriptor(4, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &depthMapDescriptor);
		descriptorSets.waterplane->addDescriptor(5, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, &uniformBuffers.CSM.descriptor);
		descriptorSets.waterplane->create();
			   			   		
		// Debug quad
		descriptorSets.debugquad = new DescriptorSet(device);
		descriptorSets.debugquad->setPool(descriptorPool);
		descriptorSets.debugquad->addLayout(descriptorSetLayouts.textured);
		descriptorSets.debugquad->addDescriptor(1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &offscreenPass.reflection.descriptor);
		descriptorSets.debugquad->addDescriptor(2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &offscreenPass.refraction.descriptor);
		descriptorSets.debugquad->create();

		// Terrain
		descriptorSets.terrain = new DescriptorSet(device);
		descriptorSets.terrain->setPool(descriptorPool);
		descriptorSets.terrain->addLayout(descriptorSetLayouts.terrain);
		descriptorSets.terrain->addDescriptor(0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, &uniformBuffers.terrain.descriptor);
		descriptorSets.terrain->addDescriptor(1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &textures.heightMap.descriptor);
		descriptorSets.terrain->addDescriptor(2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &textures.terrainArray.descriptor);
		descriptorSets.terrain->addDescriptor(3, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &depthMapDescriptor);
		descriptorSets.terrain->addDescriptor(4, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, &uniformBuffers.CSM.descriptor);
		descriptorSets.terrain->create();

		// Skysphere
		descriptorSets.skysphere = new DescriptorSet(device);
		descriptorSets.skysphere->setPool(descriptorPool);
		descriptorSets.skysphere->addLayout(descriptorSetLayouts.skysphere);
		descriptorSets.skysphere->addDescriptor(0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, &uniformBuffers.sky.descriptor);
		descriptorSets.skysphere->addDescriptor(1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &textures.skySphere.descriptor);
		descriptorSets.skysphere->create();

		// Shadow map cascades (one set per cascade)
		// @todo: Doesn't make sense, all refer to same depth
		for (auto i = 0; i < cascades.size(); i++) {
			VkDescriptorImageInfo cascadeImageInfo = vks::initializers::descriptorImageInfo(depth.sampler, depth.view, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL);
			cascades[i].descriptorSet = new DescriptorSet(device);
			cascades[i].descriptorSet->setPool(descriptorPool);
			cascades[i].descriptorSet->addLayout(descriptorSetLayouts.textured);
			cascades[i].descriptorSet->addDescriptor(0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, &depthPass.uniformBuffer.descriptor);
			cascades[i].descriptorSet->addDescriptor(1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &cascadeImageInfo);
			cascades[i].descriptorSet->create();
		}

		// Depth pass
		depthPass.descriptorSet = new DescriptorSet(device);
		depthPass.descriptorSet->setPool(descriptorPool);
		depthPass.descriptorSet->addLayout(depthPass.descriptorSetLayout);
		depthPass.descriptorSet->addDescriptor(0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, &depthPass.uniformBuffer.descriptor);
		depthPass.descriptorSet->create();

		// Cascade debug
		cascadeDebug.descriptorSet = new DescriptorSet(device);
		cascadeDebug.descriptorSet->setPool(descriptorPool);
		cascadeDebug.descriptorSet->addLayout(cascadeDebug.descriptorSetLayout);
		cascadeDebug.descriptorSet->addDescriptor(0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &depthMapDescriptor);
		cascadeDebug.descriptorSet->create();
	}

	void preparePipelines()
	{
		VkPipelineInputAssemblyStateCreateInfo inputAssemblyState = vks::initializers::pipelineInputAssemblyStateCreateInfo(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, 0, VK_FALSE);
		VkPipelineRasterizationStateCreateInfo rasterizationState = vks::initializers::pipelineRasterizationStateCreateInfo(VK_POLYGON_MODE_FILL, VK_CULL_MODE_FRONT_BIT, VK_FRONT_FACE_CLOCKWISE, 0);
		VkPipelineColorBlendAttachmentState blendAttachmentState = vks::initializers::pipelineColorBlendAttachmentState(0xf, VK_FALSE);
		VkPipelineColorBlendStateCreateInfo colorBlendState = vks::initializers::pipelineColorBlendStateCreateInfo(1, &blendAttachmentState);
		VkPipelineDepthStencilStateCreateInfo depthStencilState = vks::initializers::pipelineDepthStencilStateCreateInfo(VK_TRUE, VK_TRUE,VK_COMPARE_OP_LESS_OR_EQUAL);
		VkPipelineViewportStateCreateInfo viewportState = vks::initializers::pipelineViewportStateCreateInfo(1, 1, 0);
		VkPipelineMultisampleStateCreateInfo multisampleState = vks::initializers::pipelineMultisampleStateCreateInfo(VK_SAMPLE_COUNT_1_BIT, 0);
		std::vector<VkDynamicState> dynamicStateEnables = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
		VkPipelineDynamicStateCreateInfo dynamicState = vks::initializers::pipelineDynamicStateCreateInfo(dynamicStateEnables);

		// Vertex bindings and attributes
		const std::vector<VkVertexInputBindingDescription> vertexInputBindings = {
			vks::initializers::vertexInputBindingDescription(0, sizeof(vkglTF::Model::Vertex), VK_VERTEX_INPUT_RATE_VERTEX),
		};
		const std::vector<VkVertexInputAttributeDescription> vertexInputAttributes = {
			vks::initializers::vertexInputAttributeDescription(0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(vkglTF::Model::Vertex, pos)),
			vks::initializers::vertexInputAttributeDescription(0, 1, VK_FORMAT_R32G32B32_SFLOAT, offsetof(vkglTF::Model::Vertex, normal)),
			vks::initializers::vertexInputAttributeDescription(0, 2, VK_FORMAT_R32G32_SFLOAT, offsetof(vkglTF::Model::Vertex, uv))
		};
		VkPipelineVertexInputStateCreateInfo vertexInputState = vks::initializers::pipelineVertexInputStateCreateInfo();
		vertexInputState.vertexBindingDescriptionCount = static_cast<uint32_t>(vertexInputBindings.size());
		vertexInputState.pVertexBindingDescriptions = vertexInputBindings.data();
		vertexInputState.vertexAttributeDescriptionCount = static_cast<uint32_t>(vertexInputAttributes.size());
		vertexInputState.pVertexAttributeDescriptions = vertexInputAttributes.data();

		VkGraphicsPipelineCreateInfo pipelineCI{};
		pipelineCI.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
		pipelineCI.pVertexInputState = &vertexInputState;
		pipelineCI.pInputAssemblyState = &inputAssemblyState;
		pipelineCI.pRasterizationState = &rasterizationState;
		pipelineCI.pColorBlendState = &colorBlendState;
		pipelineCI.pMultisampleState = &multisampleState;
		pipelineCI.pViewportState = &viewportState;
		pipelineCI.pDepthStencilState = &depthStencilState;
		pipelineCI.pDynamicState = &dynamicState;

		rasterizationState.cullMode = VK_CULL_MODE_NONE;
		depthStencilState.depthTestEnable = VK_FALSE;

		// Debug
		pipelines.debug = new Pipeline(device);
		pipelines.debug->setCreateInfo(pipelineCI);
		pipelines.debug->setCache(pipelineCache);
		pipelines.debug->setLayout(pipelineLayouts.debug);
		pipelines.debug->setRenderPass(renderPass);
		pipelines.debug->addShader(getAssetPath() + "shaders/quad.vert.spv");
		pipelines.debug->addShader(getAssetPath() + "shaders/quad.frag.spv");
		pipelines.debug->create();
		// Debug cascades
		cascadeDebug.pipeline = new Pipeline(device);
		cascadeDebug.pipeline->setCreateInfo(pipelineCI);
		cascadeDebug.pipeline->setCache(pipelineCache);
		cascadeDebug.pipeline->setLayout(cascadeDebug.pipelineLayout);
		cascadeDebug.pipeline->setRenderPass(renderPass);
		cascadeDebug.pipeline->addShader(getAssetPath() + "shaders/debug_csm.vert.spv");
		cascadeDebug.pipeline->addShader(getAssetPath() + "shaders/debug_csm.frag.spv");
		cascadeDebug.pipeline->create();

		depthStencilState.depthTestEnable = VK_TRUE;

		// Mirror
		rasterizationState.cullMode = VK_CULL_MODE_NONE;
		pipelines.mirror = new Pipeline(device);
		pipelines.mirror->setCreateInfo(pipelineCI);
		pipelines.mirror->setCache(pipelineCache);
		pipelines.mirror->setLayout(pipelineLayouts.textured);
		pipelines.mirror->setRenderPass(renderPass);
		pipelines.mirror->addShader(getAssetPath() + "shaders/mirror.vert.spv");
		pipelines.mirror->addShader(getAssetPath() + "shaders/mirror.frag.spv");
		pipelines.mirror->create();

		// Terrain
		pipelines.terrain = new Pipeline(device);
		pipelines.terrain->setCreateInfo(pipelineCI);
		pipelines.terrain->setCache(pipelineCache);
		pipelines.terrain->setLayout(pipelineLayouts.terrain);
		pipelines.terrain->setRenderPass(renderPass);
		pipelines.terrain->addShader(getAssetPath() + "shaders/terrain.vert.spv");
		pipelines.terrain->addShader(getAssetPath() + "shaders/terrain.frag.spv");
		pipelines.terrain->create();

		// Sky
		rasterizationState.cullMode = VK_CULL_MODE_NONE;
		depthStencilState.depthWriteEnable = VK_FALSE;
		pipelines.sky = new Pipeline(device);
		pipelines.sky->setCreateInfo(pipelineCI);
		pipelines.sky->setCache(pipelineCache);
		pipelines.sky->setLayout(pipelineLayouts.sky);
		pipelines.sky->setRenderPass(renderPass);
		pipelines.sky->addShader(getAssetPath() + "shaders/skysphere.vert.spv");
		pipelines.sky->addShader(getAssetPath() + "shaders/skysphere.frag.spv");
		pipelines.sky->create();

		depthStencilState.depthWriteEnable = VK_TRUE;

		// Shadow map depth pass
		colorBlendState.attachmentCount = 0;
		depthStencilState.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
		// Enable depth clamp (if available)
		rasterizationState.depthClampEnable = deviceFeatures.depthClamp;
		pipelines.depthpass = new Pipeline(device);
		pipelines.depthpass->setCreateInfo(pipelineCI);
		pipelines.depthpass->setCache(pipelineCache);
		pipelines.depthpass->setLayout(depthPass.pipelineLayout);
		pipelines.depthpass->setRenderPass(depthPass.renderPass);
		pipelines.depthpass->addShader(getAssetPath() + "shaders/depthpass.vert.spv");
		pipelines.depthpass->addShader(getAssetPath() + "shaders/terrain_depthpass.frag.spv");
		pipelines.depthpass->create();
	}

	// Prepare and initialize uniform buffer containing shader uniforms
	void prepareUniformBuffers()
	{		
		VK_CHECK_RESULT(vulkanDevice->createBuffer(VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &uniformBuffers.vsShared, sizeof(uboShared)));
		VK_CHECK_RESULT(vulkanDevice->createBuffer(VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &uniformBuffers.vsMirror, sizeof(uboWaterPlane)));
		VK_CHECK_RESULT(vulkanDevice->createBuffer(VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &uniformBuffers.vsOffScreen, sizeof(uboShared)));
		VK_CHECK_RESULT(vulkanDevice->createBuffer(VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &uniformBuffers.vsDebugQuad, sizeof(uboShared)));
		VK_CHECK_RESULT(vulkanDevice->createBuffer(VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &uniformBuffers.terrain, sizeof(uboShared)));
		VK_CHECK_RESULT(vulkanDevice->createBuffer(VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &uniformBuffers.sky, sizeof(uboShared)));
		VK_CHECK_RESULT(vulkanDevice->createBuffer(VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &depthPass.uniformBuffer, sizeof(depthPass.ubo)));
		VK_CHECK_RESULT(vulkanDevice->createBuffer(VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &uniformBuffers.CSM, sizeof(uboCSM)));

		// Map persistent
		VK_CHECK_RESULT(uniformBuffers.vsShared.map());
		VK_CHECK_RESULT(uniformBuffers.vsMirror.map());
		VK_CHECK_RESULT(uniformBuffers.vsOffScreen.map());
		VK_CHECK_RESULT(uniformBuffers.vsDebugQuad.map());
		VK_CHECK_RESULT(uniformBuffers.terrain.map());
		VK_CHECK_RESULT(uniformBuffers.sky.map());
		VK_CHECK_RESULT(depthPass.uniformBuffer.map());
		VK_CHECK_RESULT(uniformBuffers.CSM.map());

		updateUniformBuffers();
		updateUniformBufferOffscreen();
	}

	void updateUniformBuffers()
	{
		float radius = 50.0f;
		lightPos = glm::vec4(20.0f, -15.0f, -15.0f, 0.0f) * radius;
		lightPos = glm::vec4(-20.0f, -15.0f, -15.0f, 0.0f) * radius;
		lightPos = glm::vec4(-20.0f, -15.0f, 20.0f, 0.0f) * radius;
		// @todo
		lightPos = glm::vec4(20.0f, -10.0f, 20.0f, 0.0f);

		//float angle = glm::radians(timer * 360.0f);
		//lightPos = glm::vec4(cos(angle) * radius, -15.0f, sin(angle) * radius, 0.0f);

		uboTerrain.lightDir = glm::normalize(-lightPos);
		uboWaterPlane.lightDir = glm::normalize(-lightPos);

		uboShared.projection = camera.matrices.perspective;
		uboShared.model = camera.matrices.view * glm::mat4(1.0f);

		// Mesh
		memcpy(uniformBuffers.vsShared.mapped, &uboShared, sizeof(uboShared));

		// Mirror
		uboWaterPlane.projection = camera.matrices.perspective;
		uboWaterPlane.model = camera.matrices.view * glm::mat4(1.0f);
		uboWaterPlane.cameraPos = glm::vec4(camera.position, 0.0f);
		uboWaterPlane.time = sin(glm::radians(timer * 360.0f));
		memcpy(uniformBuffers.vsMirror.mapped, &uboWaterPlane, sizeof(uboWaterPlane));

		// Debug quad
		uboShared.projection = glm::ortho(4.0f, 0.0f, 0.0f, 4.0f*(float)height / (float)width, -1.0f, 1.0f);
		uboShared.model = glm::mat4(1.0f);
		memcpy(uniformBuffers.vsDebugQuad.mapped, &uboShared, sizeof(uboShared));

		updateUniformBufferTerrain();
		updateUniformBufferCSM();

		// Sky
		uboSky.projection = camera.matrices.perspective;
		uboSky.model = glm::mat4(glm::mat3(camera.matrices.view));
		uniformBuffers.sky.copyTo(&uboSky, sizeof(uboSky));
	}

	void updateUniformBufferTerrain() {
		uboTerrain.projection = camera.matrices.perspective;
		uboTerrain.model = camera.matrices.view;
		uniformBuffers.terrain.copyTo(&uboTerrain, sizeof(uboTerrain));
	}

	void updateUniformBufferCSM() {
		for (auto i = 0; i < cascades.size(); i++) {
			depthPass.ubo.cascadeViewProjMat[i] = cascades[i].viewProjMatrix;
		}
		memcpy(depthPass.uniformBuffer.mapped, &depthPass.ubo, sizeof(depthPass.ubo));

		for (auto i = 0; i < cascades.size(); i++) {
			uboCSM.cascadeSplits[i] = cascades[i].splitDepth;
			uboCSM.cascadeViewProjMat[i] = cascades[i].viewProjMatrix;
		}
		uboCSM.inverseViewMat = glm::inverse(camera.matrices.view);
		uboCSM.lightDir = normalize(-lightPos);
		memcpy(uniformBuffers.CSM.mapped, &uboCSM, sizeof(uboCSM));
	}

	void updateUniformBufferOffscreen()
	{
		uboShared.projection = camera.matrices.perspective;
		uboShared.model = camera.matrices.view * glm::mat4(1.0f);
		uboShared.model = glm::scale(uboShared.model, glm::vec3(1.0f, -1.0f, 1.0f));
		memcpy(uniformBuffers.vsOffScreen.mapped, &uboShared, sizeof(uboShared));
	}

	void draw()
	{
		VulkanExampleBase::prepareFrame();

		// Command buffer to be sumitted to the queue
		submitInfo.commandBufferCount = 1;
		submitInfo.pCommandBuffers = &commandBuffers[currentBuffer]->handle;

		// Submit to queue
		VK_CHECK_RESULT(vkQueueSubmit(queue, 1, &submitInfo, VK_NULL_HANDLE));

		VulkanExampleBase::submitFrame();
	}

	void prepare()
	{
		VulkanExampleBase::prepare();
		loadAssets();
		generateTerrain();
		prepareOffscreen();
		prepareCSM();
		prepareUniformBuffers();
		setupDescriptorSetLayout();
		preparePipelines();
		setupDescriptorPool();
		setupDescriptorSet();
		buildCommandBuffers();
		prepared = true;
	}

	virtual void render()
	{
		if (!prepared)
			return;
		draw();
		if (!paused || camera.updated)
		{
			updateCascades();
			updateUniformBuffers();
			updateUniformBufferOffscreen();
		}
	}

	virtual void viewChanged()
	{
		updateUniformBuffers();
		updateUniformBufferOffscreen();
	}

	virtual void OnUpdateUIOverlay(vks::UIOverlay *overlay)
	{
		bool updateTerrain = false;
		if (overlay->header("Debugging")) {
			if (overlay->checkBox("Display reflection", &debugDisplayReflection)) {
				buildCommandBuffers();
			}
			if (overlay->checkBox("Display refraction", &debugDisplayRefraction)) {
				buildCommandBuffers();
			}
			if (overlay->checkBox("Display cascades", &cascadeDebug.enabled)) {
				buildCommandBuffers();
			}
			if (cascadeDebug.enabled) {
				if (overlay->sliderInt("Cascade", &cascadeDebug.cascadeIndex, 0, SHADOW_MAP_CASCADE_COUNT - 1)) {
					buildCommandBuffers();
				}
			}
			if (overlay->sliderFloat("Split lambda", &cascadeSplitLambda, 0.1f, 1.0f)) {
				updateCascades();
				updateUniformBuffers();
			}
		}
		if (overlay->header("Terrain layers")) {
			for (uint32_t i = 0; i < TERRAIN_LAYER_COUNT; i++) {
				if (overlay->sliderFloat2(("##layer_x" + std::to_string(i)).c_str(), uboTerrain.layers[i].x, uboTerrain.layers[i].y, 0.0f, 200.0f)) {
					updateTerrain = true;
				}
			}
		}
			//if (overlay->sliderInt("Skysphere", &skysphereIndex, 0, skyspheres.size() - 1)) {
		//	buildCommandBuffers();
		//}
	}
};

VULKAN_EXAMPLE_MAIN()
