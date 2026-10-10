#pragma once

#include <string>

namespace Ui
{

// A distance as a column of them is read (#259): exact up to 9999, then three significant
// figures with k or M -- "12.5k", "480k", "1.25M". A system is a million units across
// (#159), and a column of seven-digit numbers is one nobody reads and no column fits.
// Never more than five characters, which is what a table sizes its column to.
std::string Distance(float units);

}  // namespace Ui
