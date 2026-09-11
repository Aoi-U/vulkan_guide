
#include <utils/vk_types.h>
#include <SDL_events.h>
#include <unordered_map>

class Camera {
public:
	Camera();

	glm::vec3 velocity;
	glm::vec3 position;

	// vertical rotation
	float pitch{ 0.f };
	// horizontal rotation
	float yaw{ 0.f };

	// camera speed
	float speed{ 10.f };

	// whether the camera is currently active and processing input
	bool isActive{ false };

	glm::mat4 getViewMatrix();
	glm::mat4 getRotationMatrix();

	std::unordered_map<SDL_Keycode, bool> keyStates;

	void processSDLEvent(SDL_Event& e);

	void update(float deltaTime);

private:
	void setCursorLocked(bool locked);
};
