#pragma once

#include <core/vk_types.h>
#include <resources/ResourceManager.h>
#include <resources/Mesh.h>
#include <resources/Material.h>
#include <filesystem>
#include <vector>
#include <memory>
#include <string>

struct ModelSubMesh
{
	uint32_t startIndex;
	uint32_t count;
	Bounds bounds;
	ResourceHandle<Mesh> mesh;
	ResourceHandle<Material> material;
};

struct ModelNode : public Node
{
	std::vector<ModelSubMesh> submeshes;
};

class Model : public Resource
{
public:
	Model(const std::string& id, const std::filesystem::path& path, ResourceManager* manager);
	~Model() override = default;

	std::vector<std::shared_ptr<ModelNode>> rootNodes;
	std::vector<std::shared_ptr<Node>> allNodes;

protected:
	bool doLoad(VulkanCore* core) override;
	bool doUnload(VulkanCore* core) override;

private:
	std::filesystem::path _filePath;
	ResourceManager* _resourceManager;

};
