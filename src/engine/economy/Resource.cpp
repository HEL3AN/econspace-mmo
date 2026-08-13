#include "economy/Resource.h"

std::string ResourceName(ResourceType type)
{
    switch (type)
    {
        case ResourceType::Iron: return "Iron";
        case ResourceType::Ice: return "Ice";
        case ResourceType::Crystal: return "Crystal";
    }
    return "Unknown";
}

ResourceType ResourceFromName(const std::string& name)
{
    if (name == "Ice")
        return ResourceType::Ice;
    if (name == "Crystal")
        return ResourceType::Crystal;
    return ResourceType::Iron;
}

std::vector<ResourceType> AllResourceTypes()
{
    return { ResourceType::Iron, ResourceType::Ice, ResourceType::Crystal };
}
