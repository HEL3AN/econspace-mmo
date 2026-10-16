#pragma once

#include "doctest/doctest.h"

#include "economy/Resource.h"

// The goods of data/goods.json -- the first three of which are also the registry before any
// load -- by their ids (#343). The code never names a good; a test may, because it is a test
// of that data. A good that is not there fails the test rather than quietly becoming the
// first one.
namespace Good
{
inline ResourceType Named(const char* id)
{
    ResourceType g;
    REQUIRE(Goods::Find(id, g));
    return g;
}
inline ResourceType Iron()
{
    return Named("Iron");
}
inline ResourceType Ice()
{
    return Named("Ice");
}
inline ResourceType Crystal()
{
    return Named("Crystal");
}
}  // namespace Good
