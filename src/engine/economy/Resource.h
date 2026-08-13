#pragma once

#include <string>
#include <vector>

// Resource types in the game.
enum class ResourceType
{
    Iron,
    Ice,
    Crystal
};

std::string  ResourceName(ResourceType type);
ResourceType ResourceFromName(const std::string& name);

// List of all resource types — for iterating in loops.
std::vector<ResourceType> AllResourceTypes();
