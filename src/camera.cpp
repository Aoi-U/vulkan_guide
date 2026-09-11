#include "camera.h"
#include <glm/gtx/transform.hpp>
#include <glm/gtx/quaternion.hpp>

Camera::Camera()
{
	setCursorLocked(true);
}

glm::mat4 Camera::getViewMatrix()
{
	// to create a correct model view, we need to move the world in opposite direction to the camera
	// so we will create the camera model matrix and invert it
	glm::mat4 cameraTranslation = glm::translate(glm::mat4(1.f), position);
	glm::mat4 cameraRotation = getRotationMatrix();

	return glm::inverse(cameraTranslation * cameraRotation);
}

glm::mat4 Camera::getRotationMatrix()
{
	// join the pitch and yaw into the final rotation matrix
	glm::quat pitchRotation = glm::angleAxis(pitch, glm::vec3{ 1.f, 0.f, 0.f });
	glm::quat yawRotation = glm::angleAxis(yaw, glm::vec3{ 0.f, -1.f, 0.f });

	return glm::toMat4(yawRotation) * glm::toMat4(pitchRotation);
}

void Camera::processSDLEvent(SDL_Event& e)
{
  if (e.type == SDL_MOUSEBUTTONDOWN && e.button.button == SDL_BUTTON_RIGHT) {
    isActive = true;
    setCursorLocked(true);
  }
  else if (e.type == SDL_MOUSEBUTTONUP && e.button.button == SDL_BUTTON_RIGHT) {
    isActive = false;
    setCursorLocked(false);
    keyStates.clear(); // Clear movement keys when mouse is released
  }

  if (isActive) {
    if (e.type == SDL_KEYDOWN) {
      keyStates[e.key.keysym.sym] = true;
    }
    else if (e.type == SDL_KEYUP) {
      keyStates[e.key.keysym.sym] = false;
    }
    if (e.type == SDL_MOUSEWHEEL) {
      speed += (float)e.wheel.y;
      if (speed < 1.f) speed = 1.f;
    }
    if (e.type == SDL_MOUSEMOTION && SDL_GetRelativeMouseMode() == SDL_TRUE) {
      yaw += (float)e.motion.xrel / 500.f;
      pitch -= (float)e.motion.yrel / 500.f;
    }
  }
}

void Camera::update(float deltaTime)
{
  if (!isActive) return;

  glm::vec3 localVelocity{ 0.f };
  localVelocity.x = (keyStates[SDLK_d] ? 1.f : 0.f) - (keyStates[SDLK_a] ? 1.f : 0.f);
  localVelocity.z = (keyStates[SDLK_s] ? 1.f : 0.f) - (keyStates[SDLK_w] ? 1.f : 0.f);

  float worldVerticalVelocity = (keyStates[SDLK_SPACE] ? 1.f : 0.f) - (keyStates[SDLK_LCTRL] ? 1.f : 0.f);

  glm::mat4 cameraRotation = getRotationMatrix();

  glm::vec3 rotatedLocalVelocity = glm::vec3(cameraRotation * glm::vec4(localVelocity, 0.f));
  glm::vec3 worldVelocity = rotatedLocalVelocity + glm::vec3(0.f, worldVerticalVelocity, 0.f);

  position += worldVelocity * deltaTime * speed;
}

void Camera::setCursorLocked(bool locked)
{
	SDL_SetRelativeMouseMode(locked ? SDL_TRUE : SDL_FALSE);
}
