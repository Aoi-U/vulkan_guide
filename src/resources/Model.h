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
	Model(const std::string& id, VulkanCore* core, ResourceManager* resourceManager, const std::filesystem::path& path);
	~Model() override { Resource::unload(); }

	std::vector<std::shared_ptr<ModelNode>> rootNodes;
	std::vector<std::shared_ptr<Node>> allNodes;

protected:
	bool doLoad() override;
	bool doUnload() override;

private:
	std::filesystem::path _filePath;

	std::vector<ResourceHandle<Texture>> _textures;
	std::vector<ResourceHandle<Material>> _materials;

};
