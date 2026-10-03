#include "graphics/RenderGraph.h"
#include <queue>
#include <unordered_set>
#include <fmt/core.h>

RGPassBuilder RenderGraph::add_pass(const std::string& name)
{
	_passes.push_back(RGPass{ .name = name });
	return RGPassBuilder(_passes.back());
}

void RenderGraph::compile()
{
	const size_t passCount = _passes.size();
	_executionOrder.clear();
	_executionOrder.reserve(passCount);

	if (passCount == 0) return;

	// build adjacency list for DAG topological sort
	std::vector<std::vector<uint32_t>> adj(passCount);
	std::vector<uint32_t> inDegree(passCount, 0);

	// track writes and reads for each resource
	std::unordered_map<std::string, std::vector<uint32_t>> resourceWrites;
	std::unordered_map<std::string, std::vector<uint32_t>> resourceReads;

	for (uint32_t i = 0; i < passCount; i++) {
		for (const RGResourceUsage& write : _passes[i].writes) {
			// write after read
			if (resourceReads.contains(write.name)) {
				for (uint32_t readIndex : resourceReads[write.name]) {
					if (readIndex != i) {
						adj[readIndex].push_back(i);
						inDegree[i]++;
					}
				}
			}

			// write after write
			if (resourceWrites.contains(write.name)) {
				for (uint32_t writeIndex : resourceWrites[write.name]) {
					if (writeIndex != i) {
						adj[writeIndex].push_back(i);
						inDegree[i]++;
					}
				}
			}
			resourceWrites[write.name].push_back(i);
		}

		for (const RGResourceUsage& read : _passes[i].reads) {
			// read after write
			if (resourceWrites.contains(read.name)) {
				for (uint32_t writeIndex : resourceWrites[read.name]) {
					if (writeIndex != i) {
						adj[writeIndex].push_back(i);
						inDegree[i]++;
					}
				}
			}
			resourceReads[read.name].push_back(i);
		}
	}

	// topological sort using kahns algorithm
	std::priority_queue<uint32_t, std::vector<uint32_t>, std::greater<uint32_t>> readyQueue;
	for (uint32_t i = 0; i < passCount; i++) {
		if (inDegree[i] == 0) {
			readyQueue.push(i);
		}
	}

	while (!readyQueue.empty()) {
		uint32_t u = readyQueue.top();
		readyQueue.pop();
		_executionOrder.push_back(u);

		for (uint32_t v : adj[u]) {
			if (--inDegree[v] == 0) {
				readyQueue.push(v);
			}
		}
	}

	// fallback to original order if sort contains a cycle
	if (_executionOrder.size() != passCount) {
		fmt::println("RenderGraph: Dependency cycle detected. Falling back to original pass order");
		_executionOrder.resize(passCount);
		for (uint32_t i = 0; i < passCount; i++) {
			_executionOrder[i] = i;
		}
	}
}

void RenderGraph::execute(RenderContext & ctx)
{
	VkCommandBuffer cmd = ctx.get_cmd();

	for (uint32_t passIndex : _executionOrder) {
		RGPass& pass = _passes[passIndex];

		// determine resource transitions needed for this pass
		std::unordered_map<std::string, ResourceState> targetStates;
		for (const RGResourceUsage& read : pass.reads) {
			targetStates[read.name] = read.state;
		}
		for (const RGResourceUsage& write : pass.writes) {
			targetStates[write.name] = write.state;
		}

		// create barriers 
		std::vector<ImageBarrierInfo> barriers;
		for (const auto& [name, targetState] : targetStates) {
			if (!_resources.has_target(name))
				continue;

			ResourceState currentState = _resources.get_state(name);
			// no transition needed if current state is the same as target state
			if (currentState == targetState)
				continue;

			const RenderTargetDescription& desc = _resources.get_description(name);
			AllocatedImage& img = _resources.get_image(name);

			// create an image barrier for the transition
			ImageBarrierInfo barrier{};
			barrier.image = img.image;
			barrier.oldState = currentState;
			barrier.newState = targetState;
			barrier.aspectMask = desc.aspectMask;
			barrier.baseMipLevel = 0;
			barrier.levelCount = desc.mipLevels;
			barrier.baseArrayLayer = 0;
			barrier.layerCount = desc.arrayLayers;
			barriers.push_back(barrier);

			_resources.set_state(name, targetState);
		}

		// execute barriers
		if (!barriers.empty()) {
			RenderSync::transition_images(cmd, barriers);
		}

		// setup dynamic rendering scope if attachment exists
		const bool hasColor = !pass.colorAttachments.empty();
		const bool hasDepth = pass.depthAttachments.has_value();
		if (hasColor || hasDepth) {
			VkExtent2D renderExtent{};
			if (pass.renderExtent.has_value()) {
				renderExtent = pass.renderExtent.value();
			}
			else if (hasColor) {
				renderExtent = _resources.get_extent(pass.colorAttachments[0].name);
			}
			else if (hasDepth) {
				renderExtent = _resources.get_extent(pass.depthAttachments->name);
			}

			RenderDesc renderDesc{ .renderExtent = renderExtent };
			// setup color attachments
			for (const auto& col : pass.colorAttachments) {
				renderDesc.colorAttachments.push_back({
					.view = _resources.get_view(col.name),
					.loadOp = col.loadOp,
					.storeOp = col.storeOp,
					.clearColor = col.clearColor,
					.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL
					});
			}

			// setup depth attachment
			if (hasDepth) {
				VkImageLayout depthLayout = (pass.depthAttachments->storeOp == VK_ATTACHMENT_STORE_OP_NONE) ? VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
				renderDesc.depthAttachment = DepthAttachmentDesc{
					.view = _resources.get_view(pass.depthAttachments->name),
					.loadOp = pass.depthAttachments->loadOp,
					.storeOp = pass.depthAttachments->storeOp,
					.clearDepth = pass.depthAttachments->clearDepth,
					.layout = depthLayout
				};
			}

			ctx.begin_rendering(renderDesc);
			ctx.set_viewport_scissor(renderExtent);

			if (pass.executeCallback) {
				pass.executeCallback(ctx);
			}

			ctx.end_rendering();
		}
		else { // no attachments, just execute the callback
			if (pass.executeCallback) {
				pass.executeCallback(ctx);
			}
		}
	}
}
