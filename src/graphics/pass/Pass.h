#pragma once

#include <string>
#include <vector>
#include <variant>

// base class for pass metrics 
class Pass {
public:
	virtual ~Pass() = default;
	virtual void cleanup() = 0;
};
