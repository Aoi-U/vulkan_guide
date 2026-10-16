#pragma once

#include <yaml-cpp/yaml.h>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <graphics/utils/vk_types.h>

// yaml converters for glm types
namespace YAML
{
	template<>
	struct convert<glm::vec2>
	{
		static Node encode(const glm::vec2& rhs)
		{
			Node node;
			node.push_back(rhs.x);
			node.push_back(rhs.y);
			node.SetStyle(EmitterStyle::Flow);
			return node;
		}

		static bool decode(const Node& node, glm::vec2& rhs)
		{
			if (!node.IsSequence() || node.size() != 2)
				return false;
			rhs.x = node[0].as<float>();
			rhs.y = node[1].as<float>();
			return true;
		}
	};

	template<> 
	struct convert<glm::vec3>
	{
		static Node encode(const glm::vec3& rhs)
		{
			Node node;
			node.push_back(rhs.x);
			node.push_back(rhs.y);
			node.push_back(rhs.z);
			node.SetStyle(EmitterStyle::Flow);
			return node;
		}
		static bool decode(const Node& node, glm::vec3& rhs)
		{
			if (!node.IsSequence() || node.size() != 3)
				return false;
			rhs.x = node[0].as<float>();
			rhs.y = node[1].as<float>();
			rhs.z = node[2].as<float>();
			return true;
		}
	};

	template<>
	struct convert<glm::vec4>
	{
		static Node encode(const glm::vec4& rhs)
		{
			Node node;
			node.push_back(rhs.x);
			node.push_back(rhs.y);
			node.push_back(rhs.z);
			node.push_back(rhs.w);
			node.SetStyle(EmitterStyle::Flow);
			return node;
		}
		static bool decode(const Node& node, glm::vec4& rhs)
		{
			if (!node.IsSequence() || node.size() != 4)
				return false;
			rhs.x = node[0].as<float>();
			rhs.y = node[1].as<float>();
			rhs.z = node[2].as<float>();
			rhs.w = node[3].as<float>();
			return true;
		}
	};

	template<>
	struct convert<glm::quat>
	{
		static Node encode(const glm::quat& rhs)
		{
			Node node;
			node.push_back(rhs.w);
			node.push_back(rhs.x);
			node.push_back(rhs.y);
			node.push_back(rhs.z);
			node.SetStyle(EmitterStyle::Flow);
			return node;
		}

		static bool decode(const Node& node, glm::quat& rhs)
		{
			if (!node.IsSequence() || node.size() != 4) return false;
			rhs.w = node[0].as<float>();
			rhs.x = node[1].as<float>();
			rhs.y = node[2].as<float>();
			rhs.z = node[3].as<float>();
			return true;
		}
	};

	template<>
	struct convert<Bounds>
	{
		static Node encode(const Bounds& b)
		{
			Node node;
			node["origin"] = b.origin;
			node["sphereRadius"] = b.sphereRadius;
			node["extents"] = b.extents;
			return node;
		}

		static bool decode(const Node& node, Bounds& b)
		{
			if (!node.IsMap()) return false;
			if (node["origin"])       b.origin = node["origin"].as<glm::vec3>();
			if (node["sphereRadius"]) b.sphereRadius = node["sphereRadius"].as<float>();
			if (node["extents"])      b.extents = node["extents"].as<glm::vec3>();
			return true;
		}
	};

	// emitter overloads for glm types
	inline Emitter& operator<<(Emitter& out, const glm::vec2& v)
	{
		out << Flow << BeginSeq << v.x << v.y << EndSeq;
		return out;
	}

	inline Emitter& operator<<(Emitter& out, const glm::vec3& v)
	{
		out << Flow << BeginSeq << v.x << v.y << v.z << EndSeq;
		return out;
	}

	inline Emitter& operator<<(Emitter& out, const glm::vec4& v)
	{
		out << Flow << BeginSeq << v.x << v.y << v.z << v.w << EndSeq;
		return out;
	}

	inline Emitter& operator<<(Emitter& out, const glm::quat& q)
	{
		out << Flow << BeginSeq << q.w << q.x << q.y << q.z << EndSeq;
		return out;
	}
}